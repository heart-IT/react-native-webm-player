#pragma once

#include <atomic>
#include <array>
#include <algorithm>
#include <cstdlib>
#include "VideoConfig.h"
#include "common/MediaConfig.h"
#include "common/MediaTime.h"

namespace media {

// Adaptive jitter estimation for video frames.
// Ported from audio JitterEstimator (JitterEstimator.h) with adaptations:
// - Frame interval ~33ms (30fps) vs 20ms (audio)
// - Drift PPM tracking matches audio JitterEstimator for clock skew compensation
// - Same EWMA math, spike detection, burst floor memory, adaptive alpha
//
// Thread safety: onFrame() and reset() called from JS thread only (serialized
// by VideoFrameQueue::mtx_). bufferTargetUs() called from decode thread.
// Atomics used for cross-thread visibility of jitter estimate, not for
// internal synchronization of spikeHistory_ (which is JS-thread-only).
class VideoJitterEstimator {
    static constexpr int32_t kFixedScale = 65536;

    // Fixed alpha for jitter EWMA — 0.125 balances responsiveness and stability
    static constexpr int32_t kJitterAlpha = 8192;            // 0.125

    static constexpr int32_t kDriftAlphaFixed = 2048;        // 0.03125 * 65536
    static constexpr int32_t kDriftOneMinusAlpha = 63488;    // 0.96875 * 65536
    static constexpr int64_t kDriftThresholdUs = 500;

    // Long-term drift tracking for clock drift compensation
    static constexpr int32_t kLongTermDriftAlpha = 1024;         // 0.015625 * 65536
    static constexpr int32_t kLongTermDriftOneMinusAlpha = 64512; // 0.984375 * 65536
    static constexpr int64_t kMinDriftWindowUs = 30'000'000;     // 30 seconds

    static constexpr int64_t kMaxReasonableJitterUs = 120000; // Higher than audio — video jitter is burstier

public:
    void reset() noexcept {
        jitterUs_.store(video_config::jitter::kDefaultBufferUs, std::memory_order_relaxed);
        lastArrivalUs_.store(0, std::memory_order_relaxed);
        lastPtsUs_.store(0, std::memory_order_relaxed);
        sampleCount_.store(0, std::memory_order_relaxed);
        spikeHoldUntilUs_.store(0, std::memory_order_relaxed);
        burstFloorUs_.store(0, std::memory_order_relaxed);
        spikeHistoryCount_.store(0, std::memory_order_relaxed);
        overrideTargetUs_.store(0, std::memory_order_relaxed);
        networkFloorUs_.store(0, std::memory_order_relaxed);
        for (auto& rec : spikeHistory_) { rec = {}; }

        driftEstimateUs_.store(0, std::memory_order_relaxed);
        driftCompensationUs_.store(0, std::memory_order_relaxed);
        cumulativeDriftUs_.store(0, std::memory_order_relaxed);
        firstArrivalUs_.store(0, std::memory_order_relaxed);
        longTermDriftPpm_.store(0, std::memory_order_relaxed);
    }

    void onFrame(int64_t ptsUs, int64_t arrivalUs, bool isKeyFrame) noexcept {
        int64_t lastArrival = lastArrivalUs_.load(std::memory_order_relaxed);
        int64_t lastPts = lastPtsUs_.load(std::memory_order_relaxed);

        if (arrivalUs <= lastArrival) return;

        lastArrivalUs_.store(arrivalUs, std::memory_order_relaxed);
        lastPtsUs_.store(ptsUs, std::memory_order_relaxed);

        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) {
            firstArrivalUs_.store(arrivalUs, std::memory_order_relaxed);
        }

        if (lastArrival == 0 || lastPts == 0) return;

        int64_t arrivalDelta = arrivalUs - lastArrival;
        int64_t ptsDelta = ptsUs - lastPts;

        if (ptsDelta <= 0 || arrivalDelta <= 0) return;
        if (ptsDelta > config::epoch::kMaxForwardJumpUs) return;

        // Discontinuity detection: if ptsDelta >> expected frame interval (~33ms),
        // frames were likely dropped — don't update jitter estimate.
        if (ptsDelta > video_config::jitter::kMaxNormalPtsDeltaUs) return;

        // Keyframes are often significantly larger and arrive with more jitter
        // than P-frames. Don't let keyframe arrival jitter inflate the estimate
        // used for inter-frame timing — keyframes have their own buffer headroom
        // via the decode queue depth.
        if (isKeyFrame) return;

        constexpr int64_t kMaxDelta = config::epoch::kMaxForwardJumpUs;
        int64_t clampedArrival = std::clamp(arrivalDelta, int64_t{0}, kMaxDelta);
        int64_t clampedPts = std::clamp(ptsDelta, int64_t{0}, kMaxDelta);
        int64_t signedDeviation = clampedArrival - clampedPts;

        sampleCount_.fetch_add(1, std::memory_order_relaxed);

        updateDriftCompensation(signedDeviation);

        int64_t driftComp = driftCompensationUs_.load(std::memory_order_relaxed);
        int64_t correctedDeviation = signedDeviation - driftComp;
        int64_t absDeviation = correctedDeviation < 0 ? -correctedDeviation : correctedDeviation;
        absDeviation = std::min(absDeviation, kMaxReasonableJitterUs);

        int64_t currentJitter = jitterUs_.load(std::memory_order_relaxed);

        // Spike detection: bypass EWMA for sudden jitter increases
        if (absDeviation > currentJitter * config::spike::kSpikeMultiplier &&
            absDeviation > video_config::jitter::kMinBufferUs) {
            int64_t spikeJitter = std::min(absDeviation, kMaxReasonableJitterUs);
            jitterUs_.store(spikeJitter, std::memory_order_relaxed);
            spikeHoldUntilUs_.store(arrivalUs + config::spike::kSpikeHoldTimeUs,
                                    std::memory_order_relaxed);
            recordSpike(arrivalUs, spikeJitter);
            return;
        }

        int32_t alpha = kJitterAlpha;
        int32_t oneMinusAlpha = kFixedScale - alpha;

        int64_t newJitter = (alpha * absDeviation + oneMinusAlpha * currentJitter) >> 16;
        newJitter = std::min(newJitter, kMaxReasonableJitterUs);

        // During spike hold, don't let jitter decrease
        int64_t holdUntil = spikeHoldUntilUs_.load(std::memory_order_relaxed);
        if (newJitter < currentJitter && arrivalUs < holdUntil) return;

        // Burst floor: prevent decay below floor from recurring spikes
        int64_t floor = burstFloorUs_.load(std::memory_order_relaxed);
        if (floor > 0) {
            if (isBurstFloorExpired(arrivalUs)) {
                burstFloorUs_.store(0, std::memory_order_relaxed);
            } else if (newJitter < floor) {
                newJitter = floor;
            }
        }

        jitterUs_.store(newJitter, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t jitterUs() const noexcept {
        return jitterUs_.load(std::memory_order_relaxed);
    }

    void setBufferTargetOverride(int64_t us) noexcept {
        overrideTargetUs_.store(us, std::memory_order_relaxed);
    }

    void setNetworkJitterFloorUs(int64_t us) noexcept {
        networkFloorUs_.store(us, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t bufferTargetUs() const noexcept {
        int64_t override = overrideTargetUs_.load(std::memory_order_relaxed);
        if (override > 0) {
            return std::clamp(override, video_config::jitter::kMinBufferUs,
                              video_config::jitter::kMaxBufferUs);
        }

        int64_t baseTarget;
        if (sampleCount_.load(std::memory_order_relaxed) < video_config::jitter::kMinSamplesForEstimate) {
            baseTarget = video_config::jitter::kDefaultBufferUs;
        } else {
            int64_t jitter = jitterUs_.load(std::memory_order_relaxed);
            baseTarget = (jitter * 5) >> 1;
        }

        int64_t netFloor = networkFloorUs_.load(std::memory_order_relaxed);
        if (netFloor > baseTarget) baseTarget = netFloor;

        return std::clamp(baseTarget, video_config::jitter::kMinBufferUs,
                          video_config::jitter::kMaxBufferUs);
    }

    [[nodiscard]] int64_t estimatedDriftUs() const noexcept {
        return driftCompensationUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t longTermDriftPpm() const noexcept {
        return longTermDriftPpm_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool hasValidLongTermDrift() const noexcept {
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return false;
        int64_t elapsed = nowUs() - first;
        return elapsed >= kMinDriftWindowUs;
    }

private:
    [[nodiscard]] bool isBurstFloorExpired(int64_t arrivalUs) const noexcept {
        constexpr size_t kSize = static_cast<size_t>(config::spike::kBurstHistorySize);
        uint32_t count = spikeHistoryCount_.load(std::memory_order_relaxed);
        size_t entries = static_cast<size_t>(std::min(count, static_cast<uint32_t>(kSize)));
        if (entries == 0) return true;
        int recentCount = 0;
        for (size_t i = 0; i < entries; ++i) {
            if (arrivalUs - spikeHistory_[i].timeUs <= config::spike::kBurstPatternWindowUs) {
                ++recentCount;
            }
        }
        return recentCount < config::spike::kMinSpikesForPattern;
    }

    void recordSpike(int64_t arrivalUs, int64_t spikeJitter) noexcept {
        constexpr size_t kSize = static_cast<size_t>(config::spike::kBurstHistorySize);
        uint32_t count = spikeHistoryCount_.load(std::memory_order_relaxed);
        size_t idx = static_cast<size_t>(count) % kSize;
        spikeHistory_[idx] = {arrivalUs, spikeJitter};
        spikeHistoryCount_.store(count + 1, std::memory_order_relaxed);

        int recentCount = 0;
        int64_t minJitter = spikeJitter;
        size_t entries = static_cast<size_t>(std::min(count + 1, static_cast<uint32_t>(kSize)));
        for (size_t i = 0; i < entries; ++i) {
            if (arrivalUs - spikeHistory_[i].timeUs <= config::spike::kBurstPatternWindowUs) {
                ++recentCount;
                if (spikeHistory_[i].jitterUs < minJitter) {
                    minJitter = spikeHistory_[i].jitterUs;
                }
            }
        }

        if (recentCount >= config::spike::kMinSpikesForPattern) {
            int64_t floorVal = static_cast<int64_t>(
                static_cast<float>(minJitter) * config::spike::kBurstFloorDamping);
            burstFloorUs_.store(floorVal, std::memory_order_relaxed);
        } else {
            burstFloorUs_.store(0, std::memory_order_relaxed);
        }
    }

    void updateDriftCompensation(int64_t signedDeviation) noexcept {
        int64_t currentEstimate = driftEstimateUs_.load(std::memory_order_relaxed);
        int64_t newEstimate = (kDriftAlphaFixed * signedDeviation +
                              kDriftOneMinusAlpha * currentEstimate) >> 16;
        driftEstimateUs_.store(newEstimate, std::memory_order_relaxed);

        if (newEstimate > kDriftThresholdUs || newEstimate < -kDriftThresholdUs) {
            driftCompensationUs_.store(newEstimate, std::memory_order_relaxed);
        } else {
            driftCompensationUs_.store(0, std::memory_order_relaxed);
        }

        updateLongTermDrift(signedDeviation);
    }

    void updateLongTermDrift(int64_t signedDeviation) noexcept {
        int64_t cumulative = cumulativeDriftUs_.fetch_add(signedDeviation, std::memory_order_relaxed)
                           + signedDeviation;

        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return;

        int64_t elapsed = nowUs() - first;
        if (elapsed < kMinDriftWindowUs) return;

        int64_t instantPpm = (cumulative * 1'000'000) / elapsed;
        instantPpm = std::clamp(instantPpm, int64_t{-5000}, int64_t{5000});

        int64_t currentPpm = longTermDriftPpm_.load(std::memory_order_relaxed);
        int64_t newPpm = (kLongTermDriftAlpha * instantPpm +
                         kLongTermDriftOneMinusAlpha * currentPpm) >> 16;

        longTermDriftPpm_.store(static_cast<int32_t>(newPpm), std::memory_order_relaxed);
    }

    alignas(config::kCacheLineSize) std::atomic<int64_t> jitterUs_{video_config::jitter::kDefaultBufferUs};
    std::atomic<uint32_t> sampleCount_{0};
    std::atomic<int64_t> driftCompensationUs_{0};

    alignas(config::kCacheLineSize) std::atomic<int64_t> lastArrivalUs_{0};
    std::atomic<int64_t> lastPtsUs_{0};
    std::atomic<int64_t> driftEstimateUs_{0};

    std::atomic<int64_t> spikeHoldUntilUs_{0};

    struct SpikeRecord {
        int64_t timeUs{0};
        int64_t jitterUs{0};
    };
    std::array<SpikeRecord, config::spike::kBurstHistorySize> spikeHistory_{};
    std::atomic<uint32_t> spikeHistoryCount_{0};
    std::atomic<int64_t> burstFloorUs_{0};
    std::atomic<int64_t> overrideTargetUs_{0};
    std::atomic<int64_t> networkFloorUs_{0};

    // Long-term drift tracking
    std::atomic<int64_t> cumulativeDriftUs_{0};
    std::atomic<int64_t> firstArrivalUs_{0};
    std::atomic<int32_t> longTermDriftPpm_{0};
};

// Presentation timing for video frames.
// Maps sender PTS to local render time using a fixed anchor + buffer target.
//
// renderTime = anchorLocal + (framePts - anchorPts) + bufferTarget
//
// Thread safety: anchor/updateBufferTarget called from decode thread only.
// isReadyToRender called from decode thread only.
class VideoRenderClock {
public:
    void anchor(int64_t firstPtsUs, int64_t localTimeUs, int64_t bufferTargetUs) noexcept {
        anchorPtsUs_ = firstPtsUs;
        anchorLocalUs_ = localTimeUs;
        bufferTargetUs_ = bufferTargetUs;
        anchored_ = true;
    }

    [[nodiscard]] int64_t scheduledRenderTime(int64_t ptsUs) const noexcept {
        if (!anchored_) return 0;
        float rate = playbackRate_.load(std::memory_order_relaxed);
        int64_t ptsDelta = ptsUs - anchorPtsUs_;
        int64_t scaledDelta = (rate != 1.0f)
            ? static_cast<int64_t>(static_cast<float>(ptsDelta) / rate)
            : ptsDelta;
        return anchorLocalUs_ + scaledDelta + bufferTargetUs_;
    }

    [[nodiscard]] bool isReadyToRender(int64_t ptsUs, int64_t nowUs) const noexcept {
        if (!anchored_) return true;
        return nowUs >= scheduledRenderTime(ptsUs);
    }

    void updateBufferTarget(int64_t newTargetUs) noexcept {
        bufferTargetUs_ = newTargetUs;
    }

    void setPlaybackRate(float rate) noexcept {
        playbackRate_.store(std::clamp(rate,
            config::playbackrate::kMinRate, config::playbackrate::kMaxRate),
            std::memory_order_relaxed);
    }

    [[nodiscard]] bool isAnchored() const noexcept { return anchored_; }

    void reset() noexcept {
        anchorPtsUs_ = 0;
        anchorLocalUs_ = 0;
        bufferTargetUs_ = video_config::jitter::kDefaultBufferUs;
        anchored_ = false;
        playbackRate_.store(1.0f, std::memory_order_relaxed);
    }

private:
    int64_t anchorPtsUs_{0};
    int64_t anchorLocalUs_{0};
    int64_t bufferTargetUs_{video_config::jitter::kDefaultBufferUs};
    bool anchored_{false};
    std::atomic<float> playbackRate_{1.0f};
};

}  // namespace media
