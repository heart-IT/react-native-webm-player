// Clock drift + burst catchup compensation for playback speed adjustment.
//
// Two independent ratio sources combined into one playback speed:
//   1. DriftCompensator: long-term clock drift (0.01-0.5% over 30+ minutes)
//   2. CatchupController: burst backlog drain (up to 5% speedup after bursts)
//
// Both ratios are multiplied to produce a single Speex resampler ratio.
// The Speex resampler provides higher-quality resampling than linear
// interpolation, eliminating metallic aliasing artifacts.
//
// Threading:
// - DriftCompensator::updateDrift() called from JS thread (frame arrival)
// - CatchupController::update() called from audio callback
// - SpeexDriftResampler::process() called from audio callback
#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>
#include <speex/speex_resampler.h>
#include "common/MediaConfig.h"
#include "common/CompilerHints.h"
#include "common/MediaLog.h"

namespace media {

namespace drift_config {
    // kMeasurementWindowUs and kMaxDriftPpm are defined in MediaConfig.h (shared with JitterEstimatorBase)
    constexpr uint32_t kMinSamplesForDrift = 1500;
    constexpr float kMaxRatioDeviation = 0.05f;
    constexpr int32_t kMinDriftPpm = 50;
    // Hysteresis window — targetRatio_ only updates when drift changes by >= this amount
    // from the last applied value. Suppresses pitch wobble at the noise floor.
    constexpr int32_t kDriftUpdateHysteresisPpm = 25;
    constexpr float kRatioSmoothingAlphaMin = 0.01f;
    constexpr float kRatioSmoothingAlphaMax = 0.04f;
    constexpr float kRatioSmoothingAlphaDefault = 0.02f;
}

// Clock drift compensator — computes target ratio from cumulative drift.
// Lock-free, called from JS thread (updateDrift) and audio callback (currentRatio).
class DriftCompensator {
public:
    void updateDrift(int64_t cumulativeDriftUs, int64_t measurementWindowUs,
                     uint32_t sampleCount) noexcept {
        if (measurementWindowUs <= 0 || sampleCount < drift_config::kMinSamplesForDrift) {
            return;
        }

        // Compute PPM as (drift / window) * 1M, avoiding overflow in the multiply.
        // Division first loses sub-ppm precision, which is fine for drift compensation.
        int64_t driftPpm = (cumulativeDriftUs / measurementWindowUs) * 1'000'000
                         + (cumulativeDriftUs % measurementWindowUs) * 1'000'000 / measurementWindowUs;
        driftPpm = std::clamp(driftPpm,
                              static_cast<int64_t>(-drift_config::kMaxDriftPpm),
                              static_cast<int64_t>(drift_config::kMaxDriftPpm));

        driftPpm_.store(static_cast<int32_t>(driftPpm), std::memory_order_relaxed);

        // Hysteresis: skip the update if drift hasn't moved far from our last applied value.
        // Prevents the smoothed ratio from chasing a wobbling target at the noise floor.
        int32_t lastPpm = lastAppliedPpm_.load(std::memory_order_relaxed);
        if (std::abs(static_cast<int32_t>(driftPpm) - lastPpm) < drift_config::kDriftUpdateHysteresisPpm) {
            return;
        }
        lastAppliedPpm_.store(static_cast<int32_t>(driftPpm), std::memory_order_relaxed);

        float targetRatio = 1.0f;
        if (std::abs(driftPpm) >= drift_config::kMinDriftPpm) {
            targetRatio = 1.0f + static_cast<float>(driftPpm) / 1'000'000.0f;
            targetRatio = std::clamp(targetRatio,
                                     1.0f - drift_config::kMaxRatioDeviation,
                                     1.0f + drift_config::kMaxRatioDeviation);
        }

        targetRatio_.store(targetRatio, std::memory_order_relaxed);
        smoothingAlpha_.store(drift_config::kRatioSmoothingAlphaDefault, std::memory_order_relaxed);
    }

    // Audio callback only — smoothed drift ratio.
    [[nodiscard]] HOT_FUNCTION
    float currentRatio() noexcept {
        float target = targetRatio_.load(std::memory_order_relaxed);
        float alpha = smoothingAlpha_.load(std::memory_order_relaxed);

        float smoothed = smoothedRatio_.load(std::memory_order_relaxed);
        smoothed = smoothed + alpha * (target - smoothed);
        smoothedRatio_.store(smoothed, std::memory_order_relaxed);

        return smoothed;
    }

    // Read-only snapshot of smoothed ratio for metrics (any thread).
    [[nodiscard]] float smoothedRatio() const noexcept {
        return smoothedRatio_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isActive() const noexcept {
        int32_t ppm = std::abs(driftPpm_.load(std::memory_order_relaxed));
        return ppm >= drift_config::kMinDriftPpm;
    }

    [[nodiscard]] int32_t driftPpm() const noexcept {
        return driftPpm_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        driftPpm_.store(0, std::memory_order_relaxed);
        lastAppliedPpm_.store(0, std::memory_order_relaxed);
        targetRatio_.store(1.0f, std::memory_order_relaxed);
        smoothingAlpha_.store(drift_config::kRatioSmoothingAlphaDefault, std::memory_order_relaxed);
        smoothedRatio_.store(1.0f, std::memory_order_relaxed);
    }

private:
    std::atomic<int32_t> driftPpm_{0};
    std::atomic<int32_t> lastAppliedPpm_{0};
    std::atomic<float> targetRatio_{1.0f};
    std::atomic<float> smoothingAlpha_{drift_config::kRatioSmoothingAlphaDefault};
    std::atomic<float> smoothedRatio_{1.0f};
};

// Buffer-depth-driven catchup — computes speedup ratio from buffer excess.
// Audio callback is the primary writer; metrics reads from any thread.
class CatchupController {
public:
    // Update catchup ratio based on current buffer depth vs target.
    // Called from audio callback before resampling.
    // Returns combined catchup ratio (1.0 = no catchup, >1.0 = speeding up).
    [[nodiscard]] HOT_FUNCTION
    float update(int64_t bufferedUs, int64_t targetUs) noexcept {
        int64_t excessUs = bufferedUs - targetUs - config::catchup::kExcessThresholdUs;

        float targetRatio = 1.0f;
        if (excessUs > 0) {
            float bias = static_cast<float>(excessUs) * config::catchup::kExcessToRatioGain;
            targetRatio = 1.0f + std::min(bias, config::catchup::kMaxSpeedupRatio - 1.0f);
        }

        // Asymmetric smoothing: decay toward unity faster than ramp up.
        // Reduces sustained pitch wobble when buffer normalizes after burst.
        float current = smoothedRatio_.load(std::memory_order_relaxed);
        float alpha = (targetRatio < current)
            ? config::catchup::kDecayAlpha       // decaying toward unity: faster
            : config::catchup::kSmoothingAlpha;  // ramping up: slower (less audible)
        current += alpha * (targetRatio - current);
        // Dead-zone: snap to unity when both target and smoothed are near 1.0.
        // Only snap when the target itself is unity — otherwise the dead-zone
        // traps the smoothed ratio at 1.0 and prevents convergence
        // (per-step change α * bias can be smaller than the dead-zone width).
        if (targetRatio == 1.0f &&
            std::abs(current - 1.0f) < config::catchup::kDeadZoneThreshold) {
            current = 1.0f;
            deadZoneSnaps_.fetch_add(1, std::memory_order_relaxed);
        }
        smoothedRatio_.store(current, std::memory_order_relaxed);
        return current;
    }

    [[nodiscard]] float currentRatio() const noexcept {
        return smoothedRatio_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t deadZoneSnaps() const noexcept {
        return deadZoneSnaps_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        smoothedRatio_.store(1.0f, std::memory_order_relaxed);
        deadZoneSnaps_.store(0, std::memory_order_relaxed);
    }

    // Reset ratio to unity without clearing diagnostic counters.
    // Use after fast-drain where the ratio is stale but the snap count
    // should remain cumulative for the session lifetime.
    void resetRatio() noexcept {
        smoothedRatio_.store(1.0f, std::memory_order_relaxed);
    }

private:
    std::atomic<float> smoothedRatio_{1.0f};
    std::atomic<uint64_t> deadZoneSnaps_{0};
};

// Speex-based resampler for drift/catchup compensation.
// Initialized once at creation with a non-unity ratio to pre-allocate
// filter tables. Ratio updates via set_rate_frac are RT-safe when the
// reduced denominator doesn't exceed the pre-allocated table size.
//
// Quality 3 (~80dB stopband) eliminates the aliasing artifacts that
// linear interpolation produced at audio frequencies.
class SpeexDriftResampler {
    // Speex quality for drift resampling. Quality 3 provides ~80dB stopband
    // attenuation with 48-tap filter — sufficient for ±10% ratio changes on audio.
    static constexpr int kDriftResamplerQuality = 3;

    // Denominator scale for fractional rate expression.
    // ratio = inRate / outRate. We express this as:
    //   num = kRateDenom * ratio (rounded)
    //   den = kRateDenom
    // Higher denominator = finer ratio granularity.
    //
    // CRITICAL: Must be prime. Speex computes GCD(num, den) to reduce
    // the fraction, and the reduced den_rate determines sinc table size.
    // A composite denominator (e.g. 100000 = 2^5 * 5^5) produces wildly
    // varying den_rate values as the ratio changes:
    //   ratio 1.0001 → num=100010, GCD=10 → den=10000
    //   ratio 1.005  → num=100500, GCD=500 → den=200
    // Each change triggers update_filter() which may reallocate the sinc
    // table (malloc on the audio thread → crash) or change table indexing
    // (sinc_table OOB → EXC_BAD_ACCESS on iOS AURemoteIO thread).
    // A prime denominator guarantees GCD(num, den)=1 for all num values
    // not divisible by den (impossible in our ratio range), so den_rate
    // stays constant and update_filter() never reallocates.
    // INVARIANT: kRateDenom MUST be prime. A composite value would cause GCD(num,den)
    // to vary across ratio updates, triggering sinc table reallocation (malloc) on the
    // audio callback thread. Verified prime: 99991 = prime (not divisible by any
    // integer up to sqrt(99991) ≈ 316). If changing, verify primality explicitly.
    static constexpr spx_uint32_t kRateDenom = 99991;  // prime, ~0.001% precision

    // Verify kRateDenom is not divisible by small primes — a composite denominator
    // would cause varying GCD values and trigger sinc table reallocation (malloc)
    // on the audio callback thread. Full primality is verified by inspection;
    // these checks catch accidental edits to common composite values.
    static_assert(kRateDenom % 2 != 0, "kRateDenom must be odd (prime)");
    static_assert(kRateDenom % 3 != 0, "kRateDenom must not be divisible by 3");
    static_assert(kRateDenom % 5 != 0, "kRateDenom must not be divisible by 5");
    static_assert(kRateDenom % 7 != 0, "kRateDenom must not be divisible by 7");

public:
    // Maximum combined ratio: user rate * drift * catchup.
    // max = 2.0 * 1.05 * 1.05 = 2.205. Public because callers (audio
    // sample reader) clamp incoming ratios against this ceiling.
    static constexpr float kMaxCombinedRatio =
        config::playbackrate::kMaxRate *
        (1.0f + drift_config::kMaxRatioDeviation) * config::catchup::kMaxSpeedupRatio;

    SpeexDriftResampler() = default;
    ~SpeexDriftResampler() noexcept { destroy(); }

    SpeexDriftResampler(const SpeexDriftResampler&) = delete;
    SpeexDriftResampler& operator=(const SpeexDriftResampler&) = delete;

    bool init() noexcept {
        destroy();

        // Pre-init ratio: initialize with the maximum combined ratio so Speex
        // allocates filter tables large enough for any ratio we'll use. With
        // a prime kRateDenom, GCD(kPreInitNum, kRateDenom) = 1, so den_rate
        // = kRateDenom and Speex allocates the full sinc table. All subsequent
        // setRatio() calls produce the same den_rate (prime guarantees GCD=1),
        // so update_filter() never reallocates — making setRatio() RT-safe on
        // the audio thread.
        constexpr spx_uint32_t kPreInitNum =
            static_cast<spx_uint32_t>(static_cast<float>(kRateDenom) * kMaxCombinedRatio + 1.0f);

        int err = 0;
        state_ = speex_resampler_init(1, kPreInitNum, kRateDenom,
                                       kDriftResamplerQuality, &err);
        if (!state_ || err != RESAMPLER_ERR_SUCCESS) {
            MEDIA_LOG_E("SpeexDriftResampler: init failed (err=%d)", err);
            state_ = nullptr;
            return false;
        }

        // Set to near-unity ratio (not exact unity) to keep den_rate = kRateDenom.
        // Exact unity (kRateDenom/kRateDenom) reduces to 1/1, which changes
        // den_rate and triggers sinc table resize on the next setRatio() call.
        // kRateDenom+1 is coprime with kRateDenom (prime), so den_rate stays.
        // The fast path in readSamples() skips the resampler for ratio ~= 1.0
        // anyway, so this near-unity init ratio produces no audible effect.
        speex_resampler_set_rate_frac(state_, kRateDenom + 1, kRateDenom,
                                      config::audio::kSampleRate,
                                      config::audio::kSampleRate);
        currentNum_ = kRateDenom + 1;
        return true;
    }

    void destroy() noexcept {
        if (state_) {
            speex_resampler_destroy(state_);
            state_ = nullptr;
        }
        currentNum_ = kRateDenom + 1;
    }

    // Update the resampling ratio. ratio > 1.0 = speed up (consume more input).
    // Expressed as fractional rate: inputRate = ratio * kRateDenom, outputRate = kRateDenom.
    // RT-safe when filter tables are pre-allocated (no realloc for same-order ratios).
    void setRatio(float ratio) noexcept {
        if (!state_) return;

        // Clamp to the full combined range (drift * catchup).
        // Min: user-rate slowdown * drift slowdown (0.5 * 0.95 = 0.475).
        // Max: drift speedup * catchup speedup (1.05 * 1.05 = 1.1025).
        constexpr float kMinCombinedRatio =
            config::playbackrate::kMinRate * (1.0f - drift_config::kMaxRatioDeviation);
        ratio = std::clamp(ratio, kMinCombinedRatio, kMaxCombinedRatio);
        auto num = static_cast<spx_uint32_t>(static_cast<float>(kRateDenom) * ratio + 0.5f);

        if (num == currentNum_) return;
        currentNum_ = num;

        // set_rate_frac with in_rate/out_rate set to actual sample rates for
        // correct internal cutoff frequency calculation
        auto inRate = static_cast<spx_uint32_t>(
            static_cast<float>(config::audio::kSampleRate) * ratio + 0.5f);
        speex_resampler_set_rate_frac(state_, num, kRateDenom,
                                      inRate,
                                      static_cast<spx_uint32_t>(config::audio::kSampleRate));
    }

    // Process samples through the Speex resampler.
    // Returns number of output samples written.
    // inputConsumed is set to number of input samples consumed.
    [[nodiscard]] HOT_FUNCTION
    size_t process(const float* input, size_t inputSamples,
                   float* output, size_t maxOutputSamples,
                   size_t& inputConsumed) noexcept {
        if (!state_ || inputSamples == 0 || maxOutputSamples == 0) {
            inputConsumed = 0;
            return 0;
        }

        auto inLen = static_cast<spx_uint32_t>(inputSamples);
        auto outLen = static_cast<spx_uint32_t>(maxOutputSamples);

        int err = speex_resampler_process_float(state_, 0, input, &inLen, output, &outLen);
        if (err != RESAMPLER_ERR_SUCCESS) {
            inputConsumed = 0;
            return 0;
        }

        inputConsumed = static_cast<size_t>(inLen);
        return static_cast<size_t>(outLen);
    }

    void reset() noexcept {
        if (state_) {
            speex_resampler_reset_mem(state_);
            // Near-unity, not exact unity — keeps den_rate = kRateDenom (prime).
            // See init() comment for rationale.
            speex_resampler_set_rate_frac(state_, kRateDenom + 1, kRateDenom,
                                          config::audio::kSampleRate,
                                          config::audio::kSampleRate);
        }
        currentNum_ = kRateDenom + 1;
    }

    [[nodiscard]] bool isInitialized() const noexcept { return state_ != nullptr; }

private:
    SpeexResamplerState* state_{nullptr};
    spx_uint32_t currentNum_{kRateDenom + 1};
};

}  // namespace media
