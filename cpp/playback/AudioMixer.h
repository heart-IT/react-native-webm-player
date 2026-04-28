#pragma once

#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "AudioDecodeChannel.h"
#include "common/CompilerHints.h"

namespace media {

// Lock-free audio level meter. Computes peak and RMS per callback,
// stores results in atomics for non-RT readers (metrics, JS).
// All computation in mix() hot path — no allocations, no locks.
class AudioLevelMeter {
public:
    // Update levels from output buffer. Called from audio callback only.
    HOT_FUNCTION
    void update(const float* samples, size_t count) noexcept {
        if (count == 0) {
            peakLevel_.store(0.0f, std::memory_order_relaxed);
            rmsLevel_.store(0.0f, std::memory_order_relaxed);
            return;
        }

        float peak = 0.0f;
        float sumSquares = 0.0f;

#if MEDIA_HAS_NEON
        float32x4_t vPeak = vdupq_n_f32(0.0f);
        float32x4_t vSum = vdupq_n_f32(0.0f);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        for (; i < simdCount; i += 4) {
            float32x4_t v = vld1q_f32(samples + i);
            float32x4_t absV = vabsq_f32(v);
            vPeak = vmaxq_f32(vPeak, absV);
            vSum = vmlaq_f32(vSum, v, v);
        }
        peak = vmaxvq_f32(vPeak);
        sumSquares = vaddvq_f32(vSum);
        for (; i < count; ++i) {
            float absVal = samples[i] < 0 ? -samples[i] : samples[i];
            if (absVal > peak) peak = absVal;
            sumSquares += samples[i] * samples[i];
        }
#elif MEDIA_HAS_SSE
        __m128 vPeak = _mm_setzero_ps();
        __m128 vSum = _mm_setzero_ps();
        __m128 signMask = _mm_set1_ps(-0.0f);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        for (; i < simdCount; i += 4) {
            __m128 v = _mm_loadu_ps(samples + i);
            __m128 absV = _mm_andnot_ps(signMask, v);
            vPeak = _mm_max_ps(vPeak, absV);
            vSum = _mm_add_ps(vSum, _mm_mul_ps(v, v));
        }
        // Horizontal max and sum
        alignas(16) float peakArr[4], sumArr[4];
        _mm_store_ps(peakArr, vPeak);
        _mm_store_ps(sumArr, vSum);
        peak = std::max({peakArr[0], peakArr[1], peakArr[2], peakArr[3]});
        sumSquares = sumArr[0] + sumArr[1] + sumArr[2] + sumArr[3];
        for (; i < count; ++i) {
            float absVal = samples[i] < 0 ? -samples[i] : samples[i];
            if (absVal > peak) peak = absVal;
            sumSquares += samples[i] * samples[i];
        }
#else
        for (size_t i = 0; i < count; ++i) {
            float absVal = samples[i] < 0 ? -samples[i] : samples[i];
            if (absVal > peak) peak = absVal;
            sumSquares += samples[i] * samples[i];
        }
#endif

        // __builtin_sqrtf compiles to a single FSQRT instruction on ARM64/SSE —
        // no library call, no FP exception risk, RT-safe.
        float rms = __builtin_sqrtf(sumSquares / static_cast<float>(count));

        peakLevel_.store(peak, std::memory_order_relaxed);
        rmsLevel_.store(rms, std::memory_order_relaxed);

        if (peak >= 1.0f) {
            clipCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void silence() noexcept {
        peakLevel_.store(0.0f, std::memory_order_relaxed);
        rmsLevel_.store(0.0f, std::memory_order_relaxed);
    }

    // Read from any thread (metrics, JS)
    [[nodiscard]] float peakLevel() const noexcept { return peakLevel_.load(std::memory_order_relaxed); }
    [[nodiscard]] float rmsLevel() const noexcept { return rmsLevel_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t clipCount() const noexcept { return clipCount_.load(std::memory_order_relaxed); }

    // Convert linear level to dBFS. Returns -infinity for zero input.
    static float toDbfs(float linear) noexcept {
        if (linear <= 0.0f) return -100.0f;
        return 20.0f * std::log10(linear);
    }

private:
    std::atomic<float> peakLevel_{0.0f};
    std::atomic<float> rmsLevel_{0.0f};
    std::atomic<uint64_t> clipCount_{0};
};

namespace mixer_constants {
    constexpr float kZeroGainThreshold = 0.001f;
    constexpr float kUnityGainTolerance = 0.001f;
    constexpr float kGainRampTolerance = 0.001f;
}

// Single-stream audio reader with gain control.
// Thread safety: mix() is called ONLY from audio callback thread.
// No allocations or blocking calls in mix() hot path.
class AudioMixer {
public:
    static constexpr size_t kFadeSamples = config::crossfade::kFadeSamples;

    void setChannel(AudioDecodeChannel* ch) noexcept { channel_ = ch; }

    [[nodiscard]] const AudioLevelMeter& levelMeter() const noexcept { return levelMeter_; }

    [[nodiscard]] size_t mix(float* output, size_t frameCount) noexcept {
        if (UNLIKELY(!channel_)) return 0;

        size_t sampleCount = frameCount * static_cast<size_t>(config::audio::kChannels);

        float targetGain = channel_->targetGain();
        float currentGain = currentGain_;

        // Both current and target near zero — output silence
        if (UNLIKELY(targetGain < mixer_constants::kZeroGainThreshold &&
                     currentGain < mixer_constants::kZeroGainThreshold)) {
            wasContributing_ = false;
            std::memset(output, 0, sampleCount * sizeof(float));
            levelMeter_.silence();
            return 0;
        }

        size_t read = channel_->readSamples(output, sampleCount);
        if (UNLIKELY(read == 0)) {
            // Always count the silenced callback — `underruns` is an edge
            // counter on Playing→Underrun, which structurally cannot represent
            // sustained silence. Triage tree §1 (audio glitches) reads this.
            channel_->markSilenceCallback();
            StreamState channelState = channel_->state();
            if (channelState == StreamState::Playing) {
                channel_->markUnderrun();
            }

            // Fade-out: if channel was contributing last frame, output a
            // decaying ramp from stored tail samples to prevent clicks.
            if (wasContributing_ && sampleCount > 0) {
                wasContributing_ = false;
                size_t fadeSamples = std::min(kFadeSamples, sampleCount);
                float fadeGain = currentGain;
                float fadeDecrement = fadeGain / static_cast<float>(fadeSamples);

                for (size_t i = 0; i < fadeSamples; ++i) {
                    fadeGain -= fadeDecrement;
                    output[i] = fadeBuffer_[i] * std::max(fadeGain, 0.0f);
                }
                if (fadeSamples < sampleCount) {
                    std::memset(output + fadeSamples, 0, (sampleCount - fadeSamples) * sizeof(float));
                }
                currentGain_ = 0.0f;
                return sampleCount;
            }

            std::memset(output, 0, sampleCount * sizeof(float));
            currentGain_ = 0.0f;
            levelMeter_.silence();
            return 0;
        }

        // Fade-in: if channel was NOT contributing last frame, force gain
        // ramp from zero to prevent clicks on resume from silence.
        if (UNLIKELY(!wasContributing_)) {
            wasContributing_ = true;
            currentGain = 0.0f;
            currentGain_ = 0.0f;
        }

        bool needsRamp = std::abs(targetGain - currentGain) > mixer_constants::kGainRampTolerance;

        if (UNLIKELY(needsRamp)) {
            applyRamp(output, read, currentGain, targetGain);
        } else {
            applyGain(output, read, targetGain);
        }

        // Zero-fill remainder if short read
        if (UNLIKELY(read < sampleCount)) {
            std::memset(output + read, 0, (sampleCount - read) * sizeof(float));
        }

        currentGain_ = targetGain;

        levelMeter_.update(output, read);

        // Store tail samples for fade-out if channel stops contributing
        size_t tailStart = (read > kFadeSamples) ? read - kFadeSamples : 0;
        size_t tailLen = read - tailStart;
        std::memcpy(fadeBuffer_.data(), output + tailStart, tailLen * sizeof(float));
        if (tailLen < kFadeSamples) {
            std::memset(fadeBuffer_.data() + tailLen, 0, (kFadeSamples - tailLen) * sizeof(float));
        }

        return sampleCount;
    }

private:
    void applyGain(float* RESTRICT samples, size_t count, float gain) noexcept {
        bool nearUnity = (gain > (1.0f - mixer_constants::kUnityGainTolerance)) &&
                         (gain < (1.0f + mixer_constants::kUnityGainTolerance));
        if (LIKELY(nearUnity)) return;

#if MEDIA_HAS_NEON
        float32x4_t vgain = vdupq_n_f32(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        for (; i < simdCount; i += 4) {
            float32x4_t v = vld1q_f32(samples + i);
            vst1q_f32(samples + i, vmulq_f32(v, vgain));
        }
        for (; i < count; ++i) samples[i] *= gain;
#elif MEDIA_HAS_SSE
        __m128 vgain = _mm_set1_ps(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        for (; i < simdCount; i += 4) {
            __m128 v = _mm_loadu_ps(samples + i);
            _mm_storeu_ps(samples + i, _mm_mul_ps(v, vgain));
        }
        for (; i < count; ++i) samples[i] *= gain;
#else
        for (size_t i = 0; i < count; ++i) samples[i] *= gain;
#endif
    }

    void applyRamp(float* RESTRICT samples, size_t count, float startGain, float endGain) noexcept {
        if (UNLIKELY(count == 0)) return;
        float gain = startGain;
        float increment = (endGain - startGain) / static_cast<float>(count);

#if MEDIA_HAS_NEON
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        if (simdCount > 0) {
            float32x4_t vgain = {gain, gain + increment, gain + 2 * increment, gain + 3 * increment};
            float32x4_t vinc4 = vdupq_n_f32(increment * 4.0f);
            for (; i < simdCount; i += 4) {
                float32x4_t v = vld1q_f32(samples + i);
                vst1q_f32(samples + i, vmulq_f32(v, vgain));
                vgain = vaddq_f32(vgain, vinc4);
            }
            gain = vgetq_lane_f32(vgain, 0);
        }
        for (; i < count; ++i) {
            samples[i] *= gain;
            gain += increment;
        }
#elif MEDIA_HAS_SSE
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};
        if (simdCount > 0) {
            __m128 vgain = _mm_set_ps(gain + 3 * increment, gain + 2 * increment,
                                       gain + increment, gain);
            __m128 vinc4 = _mm_set1_ps(increment * 4.0f);
            for (; i < simdCount; i += 4) {
                __m128 v = _mm_loadu_ps(samples + i);
                _mm_storeu_ps(samples + i, _mm_mul_ps(v, vgain));
                vgain = _mm_add_ps(vgain, vinc4);
            }
            gain = _mm_cvtss_f32(vgain);
        }
        for (; i < count; ++i) {
            samples[i] *= gain;
            gain += increment;
        }
#else
        for (size_t i = 0; i < count; ++i) {
            samples[i] *= gain;
            gain += increment;
        }
#endif
    }

    AudioDecodeChannel* channel_ = nullptr;
    float currentGain_ = 0.0f;
    bool wasContributing_ = false;
    std::array<float, kFadeSamples> fadeBuffer_{};
    AudioLevelMeter levelMeter_;
};

}  // namespace media
