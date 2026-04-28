// Shared adaptive jitter estimation for audio and video pipelines.
//
// Parameterized by JitterConfig (buffer targets, thresholds) so the same
// EWMA, spike detection, burst floor, and drift tracking logic serves both
// audio packets and video frames without duplication.
//
// Thread safety: All methods are lock-free using atomic operations.
// - onSample() called from JS thread (push path)
// - bufferTargetUs() called from both JS thread and decode thread
// - reset() called only during stream activate/deactivate (synchronized externally)
#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include "ArrivalConfidence.h"
#include "MediaConfig.h"
#include "MediaTime.h"

namespace media {

struct JitterConfig {
    int64_t defaultBufferUs;
    int64_t minBufferUs;
    int64_t maxBufferUs;
    int64_t maxReasonableJitterUs;
    int64_t maxNormalPtsDeltaUs;  // discontinuity threshold
    int minSamplesForEstimate;
};

template<JitterConfig Cfg>
class JitterEstimatorBase {
    static constexpr int32_t kFixedScale = 65536;
    static constexpr int32_t kJitterAlpha = 8192;           // 0.125
    static constexpr int32_t kDriftAlphaFixed = 2048;       // 0.03125
    static constexpr int32_t kDriftOneMinusAlpha = 63488;   // 0.96875
    static constexpr int64_t kDriftThresholdUs = 500;
    static constexpr int32_t kLongTermDriftAlpha = 1024;        // 0.015625
    static constexpr int32_t kLongTermDriftOneMinusAlpha = 64512; // 0.984375
    static constexpr int64_t kMinDriftWindowUs = drift_config::kMeasurementWindowUs;

    // Jitter trend (first derivative) tracking.
    // Alpha 0.0625 (1/16) — smooths over ~16 samples (~320ms for audio),
    // responsive enough to detect degradation trends before underruns.
    static constexpr int32_t kTrendAlpha = 4096;              // 0.0625
    static constexpr int32_t kTrendOneMinusAlpha = 61440;     // 0.9375
    // Trend boost multiplier: how aggressively to grow buffer on rising jitter.
    // 3x trend → if jitter is rising at 10us/sample, add 30us to buffer target.
    static constexpr int64_t kTrendBoostMultiplier = 3;

public:
    void reset() noexcept {
        jitterUs_.store(Cfg.minBufferUs, std::memory_order_relaxed);
        lastArrivalUs_.store(0, std::memory_order_relaxed);
        lastPtsUs_.store(0, std::memory_order_relaxed);
        sampleCount_.store(0, std::memory_order_relaxed);
        driftEstimateUs_.store(0, std::memory_order_relaxed);
        driftCompensationUs_.store(0, std::memory_order_relaxed);
        overrideTargetUs_.store(0, std::memory_order_relaxed);
        networkFloorUs_.store(0, std::memory_order_relaxed);

        spikeHoldUntilUs_.store(0, std::memory_order_relaxed);
        burstFloorUs_.store(0, std::memory_order_relaxed);
        spikeHistoryCount_.store(0, std::memory_order_relaxed);
        for (auto& rec : spikeHistory_) { rec = {}; }

        cumulativeDriftUs_.store(0, std::memory_order_relaxed);
        firstArrivalUs_.store(0, std::memory_order_relaxed);
        longTermDriftPpm_.store(0, std::memory_order_relaxed);
        jitterTrendUs_.store(0, std::memory_order_relaxed);
        prevJitterUs_.store(Cfg.minBufferUs, std::memory_order_relaxed);
        arrivalConfidence_.reset();
    }

    // Core entry point. skipJitter=true tracks arrival timing for drift
    // without updating the jitter estimate (used for video keyframes whose
    // larger size causes non-representative arrival jitter).
    void onSample(int64_t ptsUs, int64_t arrivalUs, bool skipJitter = false) noexcept {
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

        if (ptsDelta > Cfg.maxNormalPtsDeltaUs) return;

        if (skipJitter) return;

        constexpr int64_t kMaxDelta = config::epoch::kMaxForwardJumpUs;
        int64_t clampedArrival = std::clamp(arrivalDelta, int64_t{0}, kMaxDelta);
        int64_t clampedPts = std::clamp(ptsDelta, int64_t{0}, kMaxDelta);
        int64_t signedDeviation = clampedArrival - clampedPts;

        sampleCount_.fetch_add(1, std::memory_order_relaxed);

        updateDriftCompensation(signedDeviation);

        int64_t driftComp = driftCompensationUs_.load(std::memory_order_relaxed);
        int64_t correctedDeviation = signedDeviation - driftComp;

        arrivalConfidence_.onSample(correctedDeviation);
        int64_t absDeviation = correctedDeviation < 0 ? -correctedDeviation : correctedDeviation;
        absDeviation = std::min(absDeviation, Cfg.maxReasonableJitterUs);

        int64_t currentJitter = jitterUs_.load(std::memory_order_relaxed);

        // Spike detection: bypass EWMA for sudden jitter increases
        if (absDeviation > currentJitter * config::spike::kSpikeMultiplier &&
            absDeviation > Cfg.minBufferUs) {
            int64_t spikeJitter = std::min(absDeviation, Cfg.maxReasonableJitterUs);
            jitterUs_.store(spikeJitter, std::memory_order_relaxed);
            spikeHoldUntilUs_.store(arrivalUs + config::spike::kSpikeHoldTimeUs,
                                    std::memory_order_relaxed);
            recordSpike(arrivalUs, spikeJitter);
            // Update trend to reflect the spike (large positive delta)
            prevJitterUs_.store(spikeJitter, std::memory_order_relaxed);
            int64_t spikeDelta = spikeJitter - currentJitter;
            int64_t ct = jitterTrendUs_.load(std::memory_order_relaxed);
            jitterTrendUs_.store((kTrendAlpha * spikeDelta + kTrendOneMinusAlpha * ct) >> 16,
                                std::memory_order_relaxed);
            return;
        }

        int32_t alpha = kJitterAlpha;
        int32_t oneMinusAlpha = kFixedScale - alpha;

        int64_t newJitter = (alpha * absDeviation + oneMinusAlpha * currentJitter) >> 16;
        newJitter = std::min(newJitter, Cfg.maxReasonableJitterUs);

        int64_t holdUntil = spikeHoldUntilUs_.load(std::memory_order_relaxed);
        if (newJitter < currentJitter && arrivalUs < holdUntil) return;

        int64_t floor = burstFloorUs_.load(std::memory_order_relaxed);
        if (floor > 0) {
            if (isBurstFloorExpired(arrivalUs)) {
                burstFloorUs_.store(0, std::memory_order_relaxed);
            } else if (newJitter < floor) {
                newJitter = floor;
            }
        }

        jitterUs_.store(newJitter, std::memory_order_relaxed);

        // Track jitter trend (first derivative): positive = degrading network.
        // Asymmetric: only positive trend boosts the buffer target, negative
        // trend decays naturally.  Prevents oscillation from brief improvements.
        int64_t prev = prevJitterUs_.load(std::memory_order_relaxed);
        int64_t delta = newJitter - prev;
        prevJitterUs_.store(newJitter, std::memory_order_relaxed);
        int64_t currentTrend = jitterTrendUs_.load(std::memory_order_relaxed);
        int64_t newTrend = (kTrendAlpha * delta + kTrendOneMinusAlpha * currentTrend) >> 16;
        jitterTrendUs_.store(newTrend, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t jitterUs() const noexcept {
        return jitterUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float arrivalConfidence() const noexcept {
        return arrivalConfidence_.confidence();
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
            return std::clamp(override, Cfg.minBufferUs, Cfg.maxBufferUs);
        }

        int64_t baseTarget;
        if (sampleCount_.load(std::memory_order_relaxed) < Cfg.minSamplesForEstimate) {
            baseTarget = Cfg.defaultBufferUs;
        } else {
            int64_t jitter = jitterUs_.load(std::memory_order_relaxed);
            baseTarget = (jitter * 5) >> 1;
        }

        // Predictive boost: when jitter trend is positive (network degrading),
        // preemptively grow the buffer to absorb the anticipated gap.
        // Asymmetric: only positive trend boosts; negative trend is ignored
        // (let the buffer shrink naturally via normal EWMA decay).
        int64_t trend = jitterTrendUs_.load(std::memory_order_relaxed);
        if (trend > 0) {
            baseTarget += trend * kTrendBoostMultiplier;
        }

        int64_t netFloor = networkFloorUs_.load(std::memory_order_relaxed);
        if (netFloor > baseTarget) baseTarget = netFloor;

        return std::clamp(baseTarget, Cfg.minBufferUs, Cfg.maxBufferUs);
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
        return (nowUs() - first) >= kMinDriftWindowUs;
    }

    [[nodiscard]] int64_t measurementWindowUs() const noexcept {
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return 0;
        return nowUs() - first;
    }

    [[nodiscard]] int64_t cumulativeDriftUs() const noexcept {
        return cumulativeDriftUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t sampleCount() const noexcept {
        return sampleCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t jitterTrendUs() const noexcept {
        return jitterTrendUs_.load(std::memory_order_relaxed);
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

        // Periodic rebase: once elapsed exceeds kRebaseWindowUs, zero the accumulator
        // and re-anchor the window. Prevents unbounded growth of cumulativeDriftUs_
        // + elapsed that would lose precision (and eventually overflow) after several
        // hours of uptime. The long-term EWMA has converged by this point so a zero
        // restart is absorbed within ~50 samples.
        if (elapsed >= kRebaseWindowUs) {
            cumulativeDriftUs_.store(0, std::memory_order_relaxed);
            firstArrivalUs_.store(nowUs(), std::memory_order_relaxed);
            return;
        }

        int64_t instantPpm = (cumulative / elapsed) * 1'000'000
                           + (cumulative % elapsed) * 1'000'000 / elapsed;
        instantPpm = std::clamp(instantPpm,
                                static_cast<int64_t>(-drift_config::kMaxDriftPpm),
                                static_cast<int64_t>(drift_config::kMaxDriftPpm));

        int64_t currentPpm = longTermDriftPpm_.load(std::memory_order_relaxed);
        int64_t newPpm = (kLongTermDriftAlpha * instantPpm +
                         kLongTermDriftOneMinusAlpha * currentPpm) >> 16;

        longTermDriftPpm_.store(static_cast<int32_t>(newPpm), std::memory_order_relaxed);
    }

    // Rebase cadence: ~30 minutes is well past the EWMA convergence horizon
    // (minutes) and keeps cumulative / elapsed arithmetic safely inside int64.
    static constexpr int64_t kRebaseWindowUs = 30LL * 60LL * 1'000'000LL;

    // Read-path group: accessed by bufferTargetUs() from decode thread
    alignas(config::kCacheLineSize) std::atomic<int64_t> jitterUs_{Cfg.minBufferUs};
    std::atomic<uint32_t> sampleCount_{0};
    std::atomic<int64_t> driftCompensationUs_{0};
    std::atomic<int64_t> jitterTrendUs_{0};  // first derivative of jitter EWMA

    // Write-heavy group: updated every onSample() call from JS thread
    alignas(config::kCacheLineSize) std::atomic<int64_t> lastArrivalUs_{0};
    std::atomic<int64_t> lastPtsUs_{0};
    std::atomic<int64_t> driftEstimateUs_{0};
    std::atomic<int64_t> prevJitterUs_{Cfg.minBufferUs};  // previous jitter for trend calc

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

    std::atomic<int64_t> cumulativeDriftUs_{0};
    std::atomic<int64_t> firstArrivalUs_{0};
    std::atomic<int32_t> longTermDriftPpm_{0};

    ArrivalConfidence arrivalConfidence_;
};

}  // namespace media
