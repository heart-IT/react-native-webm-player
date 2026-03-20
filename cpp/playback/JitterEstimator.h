// Adaptive jitter estimation for single-stream audio playback.
#pragma once

#include <atomic>
#include <algorithm>
#include <bit>
#include "common/MediaConfig.h"
#include "common/MediaTime.h"

namespace media {

// Thread safety: All methods are lock-free using atomic operations.
// - onPacket() called from JS thread (frame push path)
// - bufferTargetUs() called from both JS thread and decode thread
// - reset() called only during stream activate/deactivate (synchronized externally)
//
// Adaptive jitter estimation:
// - Alpha adapts to network stability
// - Stable connections use slower alpha (more smoothing, less reactive)
// - Variable connections use faster alpha (more reactive to changes)
class JitterEstimator {
    // Fixed-point constants with 16-bit fractional precision (scale = 65536)
    // Using integer math eliminates float precision drift over long sessions
    static constexpr int32_t kFixedScale = 65536;

    // Fixed alpha for jitter EWMA — 0.125 balances responsiveness and stability
    static constexpr int32_t kJitterAlpha = 8192;           // 0.125 * 65536

    static constexpr int32_t kDriftAlphaFixed = 2048;       // 0.03125 * 65536
    static constexpr int32_t kDriftOneMinusAlpha = 63488;   // 0.96875 * 65536

    static constexpr int64_t kDriftThresholdUs = 500;
    static constexpr int64_t kMaxReasonableJitterUs = 80000;

    // Long-term drift tracking for clock drift compensation (30+ minute calls)
    // EWMA alpha for long-term drift (very slow: ~60 second time constant)
    static constexpr int32_t kLongTermDriftAlpha = 1024;        // 0.015625 * 65536
    static constexpr int32_t kLongTermDriftOneMinusAlpha = 64512;  // 0.984375 * 65536
    // Minimum measurement window before drift estimate is valid
    static constexpr int64_t kMinDriftWindowUs = 30'000'000;  // 30 seconds

public:
    void reset() noexcept {
        jitterUs_.store(config::jitter::kDefaultBufferUs, std::memory_order_relaxed);
        lastArrivalUs_.store(0, std::memory_order_relaxed);
        lastPtsUs_.store(0, std::memory_order_relaxed);
        sampleCount_.store(0, std::memory_order_relaxed);
        driftEstimateUs_.store(0, std::memory_order_relaxed);
        driftCompensationUs_.store(0, std::memory_order_relaxed);
        overrideTargetUs_.store(0, std::memory_order_relaxed);

        // Reset spike detection and burst memory
        spikeHoldUntilUs_.store(0, std::memory_order_relaxed);
        burstFloorUs_.store(0, std::memory_order_relaxed);
        spikeHistoryCount_ = 0;
        for (auto& rec : spikeHistory_) { rec = {}; }

        // Reset long-term drift tracking
        cumulativeDriftUs_.store(0, std::memory_order_relaxed);
        firstArrivalUs_.store(0, std::memory_order_relaxed);
        longTermDriftPpm_.store(0, std::memory_order_relaxed);
    }

    void onPacket(int64_t ptsUs, int64_t arrivalUs) noexcept {
        // Group loads to reduce memory fence overhead
        int64_t lastArrival = lastArrivalUs_.load(std::memory_order_relaxed);
        int64_t lastPts = lastPtsUs_.load(std::memory_order_relaxed);

        if (arrivalUs <= lastArrival) return;

        // Store new values early - these are independent of calculations below
        lastArrivalUs_.store(arrivalUs, std::memory_order_relaxed);
        lastPtsUs_.store(ptsUs, std::memory_order_relaxed);

        // Track first arrival for long-term drift measurement window
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) {
            firstArrivalUs_.store(arrivalUs, std::memory_order_relaxed);
        }

        // Skip first packet or invalid deltas
        if (lastArrival == 0 || lastPts == 0) return;

        int64_t arrivalDelta = arrivalUs - lastArrival;
        int64_t ptsDelta = ptsUs - lastPts;

        if (ptsDelta <= 0 || arrivalDelta <= 0) return;
        if (ptsDelta > config::epoch::kMaxForwardJumpUs) return;

        // DISCONTINUITY DETECTION: If ptsDelta >> expected frame duration,
        // frames were likely dropped. Don't update jitter estimate as this
        // would incorrectly interpret dropped frames as network jitter.
        // Expected: 20ms per frame. Allow 3x tolerance (60ms) for burst arrivals.
        constexpr int64_t kMaxNormalPtsDeltaUs = 60000;
        if (ptsDelta > kMaxNormalPtsDeltaUs) {
            // Reset last values so next packet starts fresh comparison
            // Don't update jitter - this was a discontinuity, not jitter
            return;
        }

        constexpr int64_t kMaxDelta = config::epoch::kMaxForwardJumpUs;
        int64_t clampedArrival = std::clamp(arrivalDelta, int64_t{0}, kMaxDelta);
        int64_t clampedPts = std::clamp(ptsDelta, int64_t{0}, kMaxDelta);
        int64_t signedDeviation = clampedArrival - clampedPts;

        sampleCount_.fetch_add(1, std::memory_order_relaxed);

        updateDriftCompensation(signedDeviation);

        int64_t driftComp = driftCompensationUs_.load(std::memory_order_relaxed);
        int64_t correctedDeviation = signedDeviation - driftComp;
        int64_t absDeviation = correctedDeviation < 0 ? -correctedDeviation : correctedDeviation;

        // Clamp before multiplication to prevent overflow in fixed-point EWMA
        absDeviation = std::min(absDeviation, kMaxReasonableJitterUs);

        int64_t currentJitter = jitterUs_.load(std::memory_order_relaxed);

        // Spike detection: bypass EWMA for sudden jitter increases.
        // A single late packet should immediately raise the buffer target
        // rather than waiting 4-8 packets for EWMA to converge.
        if (absDeviation > currentJitter * config::spike::kSpikeMultiplier &&
            absDeviation > config::jitter::kMinBufferUs) {
            int64_t spikeJitter = std::min(absDeviation, kMaxReasonableJitterUs);
            jitterUs_.store(spikeJitter, std::memory_order_relaxed);
            spikeHoldUntilUs_.store(arrivalUs + config::spike::kSpikeHoldTimeUs,
                                    std::memory_order_relaxed);
            recordSpike(arrivalUs, spikeJitter);
            return;
        }

        int32_t alpha = kJitterAlpha;
        int32_t oneMinusAlpha = kFixedScale - alpha;

        // Fixed-point EWMA: newJitter = alpha * absDeviation + (1-alpha) * currentJitter
        // Using 16-bit fractional precision to avoid float drift
        int64_t newJitter = (alpha * absDeviation + oneMinusAlpha * currentJitter) >> 16;
        newJitter = std::min(newJitter, kMaxReasonableJitterUs);

        // During spike hold period, don't let jitter estimate decrease.
        // This keeps the buffer target elevated after a spike to absorb
        // any subsequent late packets in the same burst.
        int64_t holdUntil = spikeHoldUntilUs_.load(std::memory_order_relaxed);
        if (newJitter < currentJitter && arrivalUs < holdUntil) {
            return;
        }

        // Burst floor: when recurring spikes are detected, prevent jitter
        // estimate from decaying below a floor derived from recent spike
        // magnitudes. Without this, the EWMA decays to baseline between
        // periodic bursts, causing fast-drain on each burst arrival.
        // Expire the floor if all spikes in the history are older than the
        // pattern window — handles the case where bursts stop and no new
        // spikes trigger recordSpike() to clear the floor.
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

    [[nodiscard]] int64_t bufferTargetUs() const noexcept {
        int64_t override = overrideTargetUs_.load(std::memory_order_relaxed);
        if (override > 0) {
            return std::clamp(override, config::jitter::kMinBufferUs, config::jitter::kMaxBufferUs);
        }

        int64_t baseTarget;
        if (sampleCount_.load(std::memory_order_relaxed) < config::jitter::kMinSamplesForEstimate) {
            baseTarget = config::jitter::kDefaultBufferUs;
        } else {
            int64_t jitter = jitterUs_.load(std::memory_order_relaxed);
            baseTarget = (jitter * 5) >> 1;
        }

        return std::clamp(baseTarget, config::jitter::kMinBufferUs, config::jitter::kMaxBufferUs);
    }

    [[nodiscard]] int64_t estimatedDriftUs() const noexcept {
        return driftCompensationUs_.load(std::memory_order_relaxed);
    }

    // Long-term drift in PPM (parts per million) for clock drift compensation
    // Positive = sender clock is faster, negative = sender clock is slower
    [[nodiscard]] int32_t longTermDriftPpm() const noexcept {
        return longTermDriftPpm_.load(std::memory_order_relaxed);
    }

    // Check if long-term drift estimate is valid (requires 30s+ of data)
    [[nodiscard]] bool hasValidLongTermDrift() const noexcept {
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return false;
        int64_t elapsed = nowUs() - first;
        return elapsed >= kMinDriftWindowUs;
    }

    // Get measurement window duration for drift calculation
    [[nodiscard]] int64_t measurementWindowUs() const noexcept {
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return 0;
        return nowUs() - first;
    }

    // Get cumulative drift for DriftCompensator
    [[nodiscard]] int64_t cumulativeDriftUs() const noexcept {
        return cumulativeDriftUs_.load(std::memory_order_relaxed);
    }

    // Get sample count for drift calculation
    [[nodiscard]] uint32_t sampleCount() const noexcept {
        return sampleCount_.load(std::memory_order_relaxed);
    }

private:
    // Check if all spikes in history are older than the burst pattern window.
    // Called from EWMA path to expire stale floors when bursts stop arriving.
    // JS thread only.
    [[nodiscard]] bool isBurstFloorExpired(int64_t arrivalUs) const noexcept {
        constexpr size_t kSize = static_cast<size_t>(config::spike::kBurstHistorySize);
        size_t entries = static_cast<size_t>(std::min(spikeHistoryCount_, static_cast<uint32_t>(kSize)));
        if (entries == 0) return true;
        int recentCount = 0;
        for (size_t i = 0; i < entries; ++i) {
            if (arrivalUs - spikeHistory_[i].timeUs <= config::spike::kBurstPatternWindowUs) {
                ++recentCount;
            }
        }
        return recentCount < config::spike::kMinSpikesForPattern;
    }

    // Record a spike and update burst floor if a recurring pattern is detected.
    // JS thread only — spikeHistory_ is plain data, no synchronization needed.
    void recordSpike(int64_t arrivalUs, int64_t spikeJitter) noexcept {
        // Write into ring buffer at next position
        constexpr size_t kSize = static_cast<size_t>(config::spike::kBurstHistorySize);
        size_t idx = static_cast<size_t>(spikeHistoryCount_) % kSize;
        spikeHistory_[idx] = {arrivalUs, spikeJitter};
        ++spikeHistoryCount_;

        // Count recent spikes within the pattern window and find minimum jitter
        int recentCount = 0;
        int64_t minJitter = spikeJitter;
        size_t entries = static_cast<size_t>(std::min(spikeHistoryCount_, static_cast<uint32_t>(kSize)));
        for (size_t i = 0; i < entries; ++i) {
            if (arrivalUs - spikeHistory_[i].timeUs <= config::spike::kBurstPatternWindowUs) {
                ++recentCount;
                if (spikeHistory_[i].jitterUs < minJitter) {
                    minJitter = spikeHistory_[i].jitterUs;
                }
            }
        }

        if (recentCount >= config::spike::kMinSpikesForPattern) {
            // Recurring burst detected — set floor to damped minimum spike jitter
            int64_t floor = static_cast<int64_t>(
                static_cast<float>(minJitter) * config::spike::kBurstFloorDamping);
            burstFloorUs_.store(floor, std::memory_order_relaxed);
        } else {
            burstFloorUs_.store(0, std::memory_order_relaxed);
        }
    }

    void updateDriftCompensation(int64_t signedDeviation) noexcept {
        // Fixed-point EWMA for short-term drift tracking (jitter compensation)
        int64_t currentEstimate = driftEstimateUs_.load(std::memory_order_relaxed);
        int64_t newEstimate = (kDriftAlphaFixed * signedDeviation +
                              kDriftOneMinusAlpha * currentEstimate) >> 16;
        driftEstimateUs_.store(newEstimate, std::memory_order_relaxed);

        // Apply compensation only when drift exceeds threshold
        if (newEstimate > kDriftThresholdUs || newEstimate < -kDriftThresholdUs) {
            driftCompensationUs_.store(newEstimate, std::memory_order_relaxed);
        } else {
            driftCompensationUs_.store(0, std::memory_order_relaxed);
        }

        // Update long-term drift tracking for clock drift compensation
        updateLongTermDrift(signedDeviation);
    }

    void updateLongTermDrift(int64_t signedDeviation) noexcept {
        // Accumulate total drift (sum of all deviations)
        int64_t cumulative = cumulativeDriftUs_.fetch_add(signedDeviation, std::memory_order_relaxed)
                           + signedDeviation;

        // Calculate PPM if we have enough measurement window
        int64_t first = firstArrivalUs_.load(std::memory_order_relaxed);
        if (first == 0) return;

        int64_t elapsed = nowUs() - first;
        if (elapsed < kMinDriftWindowUs) return;

        // PPM = (cumulative_drift_us / elapsed_us) * 1,000,000
        // Using EWMA to smooth the PPM estimate
        int64_t instantPpm = (cumulative * 1'000'000) / elapsed;

        // Clamp to reasonable range (±0.5% = ±5000 PPM)
        instantPpm = std::clamp(instantPpm, int64_t{-5000}, int64_t{5000});

        // EWMA for long-term PPM (very slow alpha for stability)
        int64_t currentPpm = longTermDriftPpm_.load(std::memory_order_relaxed);
        int64_t newPpm = (kLongTermDriftAlpha * instantPpm +
                         kLongTermDriftOneMinusAlpha * currentPpm) >> 16;

        longTermDriftPpm_.store(static_cast<int32_t>(newPpm), std::memory_order_relaxed);
    }

    // Read-path group: accessed by bufferTargetUs() from decode thread / audio callback.
    // Separated from write-heavy group to avoid false sharing.
    alignas(config::kCacheLineSize) std::atomic<int64_t> jitterUs_{config::jitter::kDefaultBufferUs};
    std::atomic<uint32_t> sampleCount_{0};
    std::atomic<int64_t> driftCompensationUs_{0};

    // Write-heavy group: updated every onPacket() call from JS thread
    alignas(config::kCacheLineSize) std::atomic<int64_t> lastArrivalUs_{0};
    std::atomic<int64_t> lastPtsUs_{0};
    std::atomic<int64_t> driftEstimateUs_{0};

    // Spike detection: hold timer prevents jitter estimate from decreasing after a spike
    std::atomic<int64_t> spikeHoldUntilUs_{0};

    // Burst memory: track recurring spikes to maintain elevated jitter floor.
    // THREAD CONTRACT: spikeHistory_ and spikeHistoryCount_ are JS-thread-only —
    // written in recordSpike/isBurstFloorExpired, called from onPacket. Do not
    // access from decode thread or audio callback thread. burstFloorUs_ is written
    // on JS thread, read on JS thread (EWMA update) — atomic for cross-cache-line
    // visibility, not contention.
    struct SpikeRecord {
        int64_t timeUs{0};
        int64_t jitterUs{0};
    };
    std::array<SpikeRecord, config::spike::kBurstHistorySize> spikeHistory_{};
    uint32_t spikeHistoryCount_{0};
    std::atomic<int64_t> burstFloorUs_{0};

    // Buffer target override (0 = use adaptive estimate)
    std::atomic<int64_t> overrideTargetUs_{0};

    // Long-term drift tracking for clock drift compensation
    std::atomic<int64_t> cumulativeDriftUs_{0};
    std::atomic<int64_t> firstArrivalUs_{0};
    std::atomic<int32_t> longTermDriftPpm_{0};
};

}  // namespace media
