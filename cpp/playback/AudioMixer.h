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
            return 0;
        }

        size_t read = channel_->readSamples(output, sampleCount);
        if (UNLIKELY(read == 0)) {
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
};

}  // namespace media
