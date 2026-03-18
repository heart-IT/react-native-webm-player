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
// Ported from audio JitterEstimator (SyncController.h) with adaptations:
// - Frame interval ~33ms (30fps) vs 20ms (audio)
// - No drift PPM tracking (video has no continuous playout clock)
// - Same EWMA math, spike detection, burst floor memory, adaptive alpha
//
// Thread safety: onFrame() called from JS thread (pushVideoFrame path),
// bufferTargetUs() called from decode thread. All fields are atomic.
class VideoJitterEstimator {
    static constexpr int32_t kFixedScale = 65536;

    static constexpr int32_t kJitterAlphaMin = 4096;        // 0.0625 (slow, stable)
    static constexpr int32_t kJitterAlphaMax = 16384;       // 0.25 (fast, variable)
    static constexpr int32_t kJitterAlphaDefault = 8192;    // 0.125 (initial)

    static constexpr int32_t kVarianceAlpha = 6554;          // 0.1
    static constexpr int64_t kLowVarianceThreshold = 1000;
    static constexpr int64_t kHighVarianceThreshold = 10000;

    static constexpr int64_t kMaxReasonableJitterUs = 120000; // Higher than audio — video jitter is burstier

public:
    void reset() noexcept {
        jitterUs_.store(video_config::jitter::kDefaultBufferUs, std::memory_order_relaxed);
        lastArrivalUs_.store(0, std::memory_order_relaxed);
        lastPtsUs_.store(0, std::memory_order_relaxed);
        sampleCount_.store(0, std::memory_order_relaxed);

        jitterAlpha_.store(kJitterAlphaDefault, std::memory_order_relaxed);
        jitterVariance_.store(0, std::memory_order_relaxed);

        spikeHoldUntilUs_.store(0, std::memory_order_relaxed);
        burstFloorUs_.store(0, std::memory_order_relaxed);
        spikeHistoryCount_ = 0;
        for (auto& rec : spikeHistory_) { rec = {}; }
    }

    void onFrame(int64_t ptsUs, int64_t arrivalUs, bool isKeyFrame) noexcept {
        int64_t lastArrival = lastArrivalUs_.load(std::memory_order_relaxed);
        int64_t lastPts = lastPtsUs_.load(std::memory_order_relaxed);

        if (arrivalUs <= lastArrival) return;

        lastArrivalUs_.store(arrivalUs, std::memory_order_relaxed);
        lastPtsUs_.store(ptsUs, std::memory_order_relaxed);

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

        int64_t absDeviation = signedDeviation < 0 ? -signedDeviation : signedDeviation;
        absDeviation = std::min(absDeviation, kMaxReasonableJitterUs);

        updateJitterVariance(absDeviation);

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

        int32_t alpha = jitterAlpha_.load(std::memory_order_relaxed);
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

    [[nodiscard]] int64_t bufferTargetUs() const noexcept {
        int64_t baseTarget;
        if (sampleCount_.load(std::memory_order_relaxed) < video_config::jitter::kMinSamplesForEstimate) {
            baseTarget = video_config::jitter::kDefaultBufferUs;
        } else {
            int64_t jitter = jitterUs_.load(std::memory_order_relaxed);
            baseTarget = (jitter * 5) >> 1;  // × 2.5
        }
        return std::clamp(baseTarget, video_config::jitter::kMinBufferUs,
                          video_config::jitter::kMaxBufferUs);
    }

private:
    [[nodiscard]] bool isBurstFloorExpired(int64_t arrivalUs) const noexcept {
        constexpr size_t kSize = static_cast<size_t>(config::spike::kBurstHistorySize);
        size_t entries = static_cast<size_t>(std::min(spikeHistoryCount_, static_cast<int>(kSize)));
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
        size_t idx = static_cast<size_t>(spikeHistoryCount_) % kSize;
        spikeHistory_[idx] = {arrivalUs, spikeJitter};
        ++spikeHistoryCount_;

        int recentCount = 0;
        int64_t minJitter = spikeJitter;
        size_t entries = static_cast<size_t>(std::min(spikeHistoryCount_, static_cast<int>(kSize)));
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

    void updateJitterVariance(int64_t absDeviation) noexcept {
        int64_t currentJitter = jitterUs_.load(std::memory_order_relaxed);
        int64_t deviation = absDeviation - currentJitter;
        int64_t deviationSquared = (deviation * deviation) / 1000;

        int64_t currentVar = jitterVariance_.load(std::memory_order_relaxed);
        int64_t newVar = (kVarianceAlpha * deviationSquared +
                         (kFixedScale - kVarianceAlpha) * currentVar) >> 16;
        jitterVariance_.store(newVar, std::memory_order_relaxed);

        int32_t newAlpha;
        if (newVar < kLowVarianceThreshold) {
            newAlpha = kJitterAlphaMin;
        } else if (newVar > kHighVarianceThreshold) {
            newAlpha = kJitterAlphaMax;
        } else {
            int64_t range = kHighVarianceThreshold - kLowVarianceThreshold;
            int64_t position = newVar - kLowVarianceThreshold;
            int32_t alphaRange = kJitterAlphaMax - kJitterAlphaMin;
            newAlpha = kJitterAlphaMin + static_cast<int32_t>((position * alphaRange) / range);
        }

        jitterAlpha_.store(newAlpha, std::memory_order_relaxed);
    }

    alignas(config::kCacheLineSize) std::atomic<int64_t> jitterUs_{video_config::jitter::kDefaultBufferUs};
    std::atomic<uint32_t> sampleCount_{0};

    alignas(config::kCacheLineSize) std::atomic<int64_t> lastArrivalUs_{0};
    std::atomic<int64_t> lastPtsUs_{0};

    std::atomic<int32_t> jitterAlpha_{kJitterAlphaDefault};
    std::atomic<int64_t> jitterVariance_{0};

    std::atomic<int64_t> spikeHoldUntilUs_{0};

    struct SpikeRecord {
        int64_t timeUs{0};
        int64_t jitterUs{0};
    };
    std::array<SpikeRecord, config::spike::kBurstHistorySize> spikeHistory_{};
    int spikeHistoryCount_{0};
    std::atomic<int64_t> burstFloorUs_{0};
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
        return anchorLocalUs_ + (ptsUs - anchorPtsUs_) + bufferTargetUs_;
    }

    [[nodiscard]] bool isReadyToRender(int64_t ptsUs, int64_t nowUs) const noexcept {
        if (!anchored_) return true;
        return nowUs >= scheduledRenderTime(ptsUs);
    }

    void updateBufferTarget(int64_t newTargetUs) noexcept {
        bufferTargetUs_ = newTargetUs;
    }

    [[nodiscard]] bool isAnchored() const noexcept { return anchored_; }

    void reset() noexcept {
        anchorPtsUs_ = 0;
        anchorLocalUs_ = 0;
        bufferTargetUs_ = video_config::jitter::kDefaultBufferUs;
        anchored_ = false;
    }

private:
    int64_t anchorPtsUs_{0};
    int64_t anchorLocalUs_{0};
    int64_t bufferTargetUs_{video_config::jitter::kDefaultBufferUs};
    bool anchored_{false};
};

}  // namespace media
