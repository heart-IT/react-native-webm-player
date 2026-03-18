#pragma once

#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "AudioDecodeSlot.h"
#include "ComfortNoiseGenerator.h"
#include "common/CompilerHints.h"

namespace media {

namespace mixer_constants {
    constexpr float kSoftClipThreshold = 0.9f;
    constexpr float kZeroGainThreshold = 0.001f;
    constexpr float kUnityGainTolerance = 0.001f;
    constexpr float kGainRampTolerance = 0.001f;  // Skip ramp if |target - current| < this
}

// Thread safety: mix() is called ONLY from audio callback thread.
// - setSlots() must be called before start() and not modified during mixing
// - All slot access is lock-free via atomic reads in AudioDecodeSlot
// - No allocations or blocking calls in mix() hot path
class AudioMixer {
public:
    static constexpr int kMaxSlots = config::limits::kMaxAudioSlots;
    static constexpr size_t kMixBufferSize = config::audio::kFrameSize;
    static constexpr size_t kFadeSamples = config::crossfade::kFadeSamples;

    void setSlots(std::array<AudioDecodeSlot*, kMaxSlots>& slots) noexcept {
        slots_ = &slots;
    }

    [[nodiscard]] size_t mix(float* output, size_t frameCount, int excludeParticipant = -1) noexcept {
        if (UNLIKELY(!slots_)) return 0;

        size_t sampleCount = std::min(frameCount * static_cast<size_t>(config::audio::kChannels), kMixBufferSize);

        float* tempBuffer = tempBuffer_.data();
        int slotsContributed = 0;
        bool gainExceedsUnity = false;

        for (size_t slotIdx = 0; slotIdx < static_cast<size_t>(kMaxSlots); ++slotIdx) {
            AudioDecodeSlot* slot = (*slots_)[slotIdx];
            if (UNLIKELY(!slot)) continue;

            // Prefetch next slot's cache line while processing current slot
            // Reduces cache miss latency by ~50-100 cycles on ARM64
            if (slotIdx + 1 < static_cast<size_t>(kMaxSlots)) {
                if (AudioDecodeSlot* nextSlot = (*slots_)[slotIdx + 1]) {
                    PREFETCH_READ(nextSlot);
                }
            }

            int participant = slot->boundParticipant();
            if (UNLIKELY(participant < 0)) {
                currentGain_[slotIdx] = 0.0f;
                wasContributing_[slotIdx] = false;
                continue;
            }
            if (UNLIKELY(participant == excludeParticipant)) continue;

            float targetGain = slot->targetGain();
            float currentGain = currentGain_[slotIdx];

            // Both current and target near zero — skip this slot entirely.
            // Clear wasContributing so we get a fade-in ramp when gain returns.
            if (UNLIKELY(targetGain < mixer_constants::kZeroGainThreshold &&
                         currentGain < mixer_constants::kZeroGainThreshold)) {
                wasContributing_[slotIdx] = false;
                continue;
            }

            size_t read = slot->readSamples(tempBuffer, sampleCount);
            if (UNLIKELY(read == 0)) {
                StreamState slotState = slot->state();
                if (slotState == StreamState::Playing) {
                    slot->markUnderrun();
                }

                // Fade-out: if this slot was contributing last frame, output a
                // decaying ramp from the stored tail samples to prevent clicks.
                if (wasContributing_[slotIdx] && sampleCount > 0) {
                    wasContributing_[slotIdx] = false;
                    size_t fadeSamples = std::min(kFadeSamples, sampleCount);
                    float fadeGain = currentGain;
                    float fadeDecrement = fadeGain / static_cast<float>(fadeSamples);

                    if (slotsContributed == 0) {
                        for (size_t i = 0; i < fadeSamples; ++i) {
                            fadeGain -= fadeDecrement;
                            output[i] = fadeBuffers_[slotIdx][i] * std::max(fadeGain, 0.0f);
                        }
                        if (fadeSamples < sampleCount) {
                            std::memset(output + fadeSamples, 0, (sampleCount - fadeSamples) * sizeof(float));
                        }
                    } else {
                        for (size_t i = 0; i < fadeSamples; ++i) {
                            fadeGain -= fadeDecrement;
                            output[i] += fadeBuffers_[slotIdx][i] * std::max(fadeGain, 0.0f);
                        }
                    }
                    slotsContributed++;
                } else if (slot->state() == StreamState::Underrun && sampleCount > 0) {
                    // Sustained underrun: generate shaped comfort noise (~-60dBFS)
                    // to maintain "live connection" perception instead of dead silence.
                    // CNG level is baked into kComfortNoiseLevel — no gain applied.
                    cng_[slotIdx].generate(tempBuffer, sampleCount);
                    if (slotsContributed == 0) {
                        std::memcpy(output, tempBuffer, sampleCount * sizeof(float));
                    } else {
                        for (size_t i = 0; i < sampleCount; ++i) {
                            output[i] += tempBuffer[i];
                        }
                    }
                    slotsContributed++;
                }

                currentGain_[slotIdx] = 0.0f;
                continue;
            }

            // Fade-in: if this slot was NOT contributing last frame, force gain
            // ramp from zero to prevent clicks on resume from silence.
            if (UNLIKELY(!wasContributing_[slotIdx])) {
                wasContributing_[slotIdx] = true;
                currentGain = 0.0f;
                currentGain_[slotIdx] = 0.0f;
                cng_[slotIdx].reset();
            }

            bool needsRamp = std::abs(targetGain - currentGain) > mixer_constants::kGainRampTolerance;
            if (targetGain > 1.0f + mixer_constants::kUnityGainTolerance) {
                gainExceedsUnity = true;
            }

            if (slotsContributed == 0) {
                if (UNLIKELY(needsRamp)) {
                    copyWithRamp(output, tempBuffer, read, currentGain, targetGain);
                } else {
                    copyWithGain(output, tempBuffer, read, targetGain);
                }
                if (UNLIKELY(read < sampleCount)) {
                    std::memset(output + read, 0, (sampleCount - read) * sizeof(float));
                }
            } else {
                if (UNLIKELY(needsRamp)) {
                    mixWithRamp(output, tempBuffer, read, currentGain, targetGain);
                } else {
                    mixWithGain(output, tempBuffer, read, targetGain);
                }
            }

            currentGain_[slotIdx] = targetGain;
            slotsContributed++;

            // Store tail samples for fade-out if this slot stops contributing.
            // Copy the last kFadeSamples from tempBuffer (raw slot output before
            // gain — fade-out will apply the decaying gain itself).
            size_t tailStart = (read > kFadeSamples) ? read - kFadeSamples : 0;
            size_t tailLen = read - tailStart;
            std::memcpy(fadeBuffers_[slotIdx].data(), tempBuffer + tailStart, tailLen * sizeof(float));
            if (tailLen < kFadeSamples) {
                std::memset(fadeBuffers_[slotIdx].data() + tailLen, 0,
                            (kFadeSamples - tailLen) * sizeof(float));
            }
        }

        if (UNLIKELY(slotsContributed == 0)) {
            std::memset(output, 0, sampleCount * sizeof(float));
            return 0;
        }

        // Clipping guard: skip softClip when a single slot at unity gain can't exceed [-1,1].
        // Pre-scanning for peak amplitude would cost more than the early-exit path inside
        // softClip (which skips per-chunk via SIMD comparison), so we rely on the coarse check.
        if (slotsContributed > 1 || gainExceedsUnity) {
            softClip(output, sampleCount);
        }

        return sampleCount;
    }

private:
    void copyWithGain(float* RESTRICT dst, const float* RESTRICT src,
                      size_t count, float gain) noexcept {
        bool nearUnity = (gain > (1.0f - mixer_constants::kUnityGainTolerance)) &&
                         (gain < (1.0f + mixer_constants::kUnityGainTolerance));

        if (LIKELY(nearUnity)) {
            std::memcpy(dst, src, count * sizeof(float));
            return;
        }

#if MEDIA_HAS_NEON
        float32x4_t vgain = vdupq_n_f32(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            float32x4_t vsrc = vld1q_f32(src + i);
            vst1q_f32(dst + i, vmulq_f32(vsrc, vgain));
        }

        for (; i < count; ++i) {
            dst[i] = src[i] * gain;
        }
#elif MEDIA_HAS_SSE
        __m128 vgain = _mm_set1_ps(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            __m128 vsrc = _mm_loadu_ps(src + i);
            _mm_storeu_ps(dst + i, _mm_mul_ps(vsrc, vgain));
        }

        for (; i < count; ++i) {
            dst[i] = src[i] * gain;
        }
#else
        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i] * gain;
        }
#endif
    }

    void copyWithRamp(float* RESTRICT dst, const float* RESTRICT src,
                      size_t count, float startGain, float endGain) noexcept {
        if (UNLIKELY(count == 0)) return;
        float gain = startGain;
        float increment = (endGain - startGain) / static_cast<float>(count);

#if MEDIA_HAS_NEON
        float32x4_t vgain = {gain, gain + increment, gain + 2*increment, gain + 3*increment};
        float32x4_t vinc4 = vdupq_n_f32(increment * 4);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            float32x4_t vsrc = vld1q_f32(src + i);
            vst1q_f32(dst + i, vmulq_f32(vsrc, vgain));
            vgain = vaddq_f32(vgain, vinc4);
        }

        gain = startGain + increment * static_cast<float>(i);
        for (; i < count; ++i) {
            dst[i] = src[i] * gain;
            gain += increment;
        }
#elif MEDIA_HAS_SSE
        __m128 vgain = _mm_set_ps(gain + 3*increment, gain + 2*increment, gain + increment, gain);
        __m128 vinc4 = _mm_set1_ps(increment * 4);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            __m128 vsrc = _mm_loadu_ps(src + i);
            _mm_storeu_ps(dst + i, _mm_mul_ps(vsrc, vgain));
            vgain = _mm_add_ps(vgain, vinc4);
        }

        gain = startGain + increment * static_cast<float>(i);
        for (; i < count; ++i) {
            dst[i] = src[i] * gain;
            gain += increment;
        }
#else
        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i] * gain;
            gain += increment;
        }
#endif
    }

    void mixWithRamp(float* RESTRICT dst, const float* RESTRICT src,
                     size_t count, float startGain, float endGain) noexcept {
        if (UNLIKELY(count == 0)) return;
        float gain = startGain;
        float increment = (endGain - startGain) / static_cast<float>(count);

#if MEDIA_HAS_NEON
        float32x4_t vgain = {gain, gain + increment, gain + 2*increment, gain + 3*increment};
        float32x4_t vinc4 = vdupq_n_f32(increment * 4);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            float32x4_t vdst = vld1q_f32(dst + i);
            float32x4_t vsrc = vld1q_f32(src + i);
            vdst = vmlaq_f32(vdst, vsrc, vgain);
            vst1q_f32(dst + i, vdst);
            vgain = vaddq_f32(vgain, vinc4);
        }

        gain = startGain + increment * static_cast<float>(i);
        for (; i < count; ++i) {
            dst[i] += src[i] * gain;
            gain += increment;
        }
#elif MEDIA_HAS_SSE
        __m128 vgain = _mm_set_ps(gain + 3*increment, gain + 2*increment, gain + increment, gain);
        __m128 vinc4 = _mm_set1_ps(increment * 4);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            __m128 vdst = _mm_loadu_ps(dst + i);
            __m128 vsrc = _mm_loadu_ps(src + i);
            vdst = _mm_add_ps(vdst, _mm_mul_ps(vsrc, vgain));
            _mm_storeu_ps(dst + i, vdst);
            vgain = _mm_add_ps(vgain, vinc4);
        }

        gain = startGain + increment * static_cast<float>(i);
        for (; i < count; ++i) {
            dst[i] += src[i] * gain;
            gain += increment;
        }
#else
        for (size_t i = 0; i < count; ++i) {
            dst[i] += src[i] * gain;
            gain += increment;
        }
#endif
    }

    void mixWithGain(float* RESTRICT dst, const float* RESTRICT src,
                     size_t count, float gain) noexcept {
#if MEDIA_HAS_NEON
        float32x4_t vgain = vdupq_n_f32(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            float32x4_t vdst = vld1q_f32(dst + i);
            float32x4_t vsrc = vld1q_f32(src + i);
            vdst = vmlaq_f32(vdst, vsrc, vgain);
            vst1q_f32(dst + i, vdst);
        }

        for (; i < count; ++i) {
            dst[i] += src[i] * gain;
        }
#elif MEDIA_HAS_SSE
        __m128 vgain = _mm_set1_ps(gain);
        size_t i = 0;
        size_t simdCount = count & ~size_t{3};

        for (; i < simdCount; i += 4) {
            __m128 vdst = _mm_loadu_ps(dst + i);
            __m128 vsrc = _mm_loadu_ps(src + i);
            vdst = _mm_add_ps(vdst, _mm_mul_ps(vsrc, vgain));
            _mm_storeu_ps(dst + i, vdst);
        }

        for (; i < count; ++i) {
            dst[i] += src[i] * gain;
        }
#else
        for (size_t i = 0; i < count; ++i) {
            dst[i] += src[i] * gain;
        }
#endif
    }

#if MEDIA_HAS_NEON
    // Single Newton-Raphson iteration: vrecpeq gives ~8-bit, one NR step gives ~16-bit.
    // Sufficient for soft clipping (matches SSE path which also uses one iteration).
    FORCE_INLINE float32x4_t reciprocalNR(float32x4_t x) noexcept {
        float32x4_t est = vrecpeq_f32(x);
        est = vmulq_f32(est, vrecpsq_f32(x, est));
        return est;
    }

    FORCE_INLINE float32x4_t divideNR(float32x4_t num, float32x4_t denom) noexcept {
        return vmulq_f32(num, reciprocalNR(denom));
    }
#elif MEDIA_HAS_SSE
    // SSE reciprocal with Newton-Raphson refinement (~12-bit precision from rcpps, refined to ~23-bit)
    // Much faster than _mm_div_ps (~5 cycles vs ~15 cycles)
    FORCE_INLINE __m128 reciprocalNR(__m128 x) noexcept {
        __m128 est = _mm_rcp_ps(x);
        // Newton-Raphson: est = est * (2 - x * est)
        __m128 two = _mm_set1_ps(2.0f);
        return _mm_mul_ps(est, _mm_sub_ps(two, _mm_mul_ps(x, est)));
    }

    FORCE_INLINE __m128 divideNR(__m128 num, __m128 denom) noexcept {
        return _mm_mul_ps(num, reciprocalNR(denom));
    }
#endif

    HOT_FUNCTION
    void softClip(float* RESTRICT samples, size_t count) noexcept {
        constexpr float kThreshold = mixer_constants::kSoftClipThreshold;
        constexpr float kHeadroom = 1.0f - kThreshold;
        bool clamped = false;

#if MEDIA_HAS_NEON
        const float32x4_t vThreshold = vdupq_n_f32(kThreshold);
        const float32x4_t vNegThreshold = vdupq_n_f32(-kThreshold);
        const float32x4_t vHeadroom = vdupq_n_f32(kHeadroom);
        const float32x4_t vInvHeadroom = vdupq_n_f32(1.0f / kHeadroom);
        const float32x4_t vOne = vdupq_n_f32(1.0f);
        const float32x4_t vZero = vdupq_n_f32(0.0f);
        const float32x4_t vNegOne = vdupq_n_f32(-1.0f);

        size_t simdCount = count & ~size_t{3};

        // Single-pass: detect and apply clipping inline per chunk
        for (size_t i = 0; i < simdCount; i += 4) {
            float32x4_t s = vld1q_f32(samples + i);

            // NaN guard: replace NaN with zero (NaN != NaN yields true)
            uint32x4_t nanMask = vmvnq_u32(vceqq_f32(s, s));
            s = vbslq_f32(nanMask, vZero, s);

            uint32x4_t posNeedClip = vcgtq_f32(s, vThreshold);
            uint32x4_t negNeedClip = vcltq_f32(s, vNegThreshold);
            // Include NaN lanes so sanitized zeros are stored back
            uint32x4_t anyNeedClip = vorrq_u32(vorrq_u32(posNeedClip, negNeedClip), nanMask);

            // Portable check if any lane is non-zero (avoids vmaxvq_u32 which requires ARMv8.1)
            uint32x2_t anyNeedClipReduced = vorr_u32(vget_low_u32(anyNeedClip), vget_high_u32(anyNeedClip));
            if (LIKELY(vget_lane_u32(anyNeedClipReduced, 0) == 0 && vget_lane_u32(anyNeedClipReduced, 1) == 0)) continue;

            float32x4_t posExcess = vmulq_f32(vsubq_f32(s, vThreshold), vInvHeadroom);
            float32x4_t posDenom = vaddq_f32(vOne, posExcess);
            // Use vmlaq_f32 instead of vfmaq_f32 for portability (non-fused multiply-add)
            float32x4_t posClipped = vmlaq_f32(vThreshold, vHeadroom, divideNR(posExcess, posDenom));

            float32x4_t negS = vnegq_f32(s);
            float32x4_t negExcess = vmulq_f32(vsubq_f32(negS, vThreshold), vInvHeadroom);
            float32x4_t negDenom = vaddq_f32(vOne, negExcess);
            float32x4_t negClipped = vnegq_f32(vmlaq_f32(vThreshold, vHeadroom, divideNR(negExcess, negDenom)));

            float32x4_t result = vbslq_f32(posNeedClip, posClipped, s);
            result = vbslq_f32(negNeedClip, negClipped, result);

            // Hard-clamp: NEON reciprocal approximation (~16-bit) can overshoot
            // ±1.0 by ~1 ULP, causing DAC/BT codec hard clipping artifacts
            float32x4_t clampedResult = vminq_f32(vmaxq_f32(result, vNegOne), vOne);
            // Detect if hard-clamp changed any lane (result != clampedResult)
            uint32x4_t clampDiff = vmvnq_u32(vceqq_f32(result, clampedResult));
            uint32x2_t clampReduced = vorr_u32(vget_low_u32(clampDiff), vget_high_u32(clampDiff));
            if (UNLIKELY(vget_lane_u32(clampReduced, 0) | vget_lane_u32(clampReduced, 1))) {
                clamped = true;
            }

            vst1q_f32(samples + i, clampedResult);
        }

        // Scalar tail - use reciprocal approximation to avoid division
        for (size_t i = simdCount; i < count; ++i) {
            float s = samples[i];
            if (UNLIKELY(std::isnan(s))) { samples[i] = 0.0f; continue; }
            if (s > kThreshold) {
                float x = s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = kThreshold + x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            } else if (s < -kThreshold) {
                float x = -s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = -kThreshold - x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            }
        }
#elif MEDIA_HAS_SSE
        const __m128 vThreshold = _mm_set1_ps(kThreshold);
        const __m128 vNegThreshold = _mm_set1_ps(-kThreshold);
        const __m128 vHeadroom = _mm_set1_ps(kHeadroom);
        const __m128 vInvHeadroom = _mm_set1_ps(1.0f / kHeadroom);
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vNegOne = _mm_set1_ps(-1.0f);

        size_t simdCount = count & ~size_t{3};

        // Single-pass: detect and apply clipping inline per chunk
        for (size_t i = 0; i < simdCount; i += 4) {
            __m128 s = _mm_loadu_ps(samples + i);

            // NaN guard: replace NaN with zero (NaN != NaN yields true via _mm_cmpunord_ps)
            __m128 nanMask = _mm_cmpunord_ps(s, s);
            if (UNLIKELY(_mm_movemask_ps(nanMask))) {
                s = _mm_andnot_ps(nanMask, s);  // Zero out NaN lanes
            }

            __m128 posNeedClip = _mm_cmpgt_ps(s, vThreshold);
            __m128 negNeedClip = _mm_cmplt_ps(s, vNegThreshold);
            // Include NaN lanes so sanitized zeros are stored back
            __m128 anyModified = _mm_or_ps(_mm_or_ps(posNeedClip, negNeedClip), nanMask);

            if (LIKELY(!_mm_movemask_ps(anyModified))) continue;

            __m128 posExcess = _mm_mul_ps(_mm_sub_ps(s, vThreshold), vInvHeadroom);
            __m128 posDenom = _mm_add_ps(vOne, posExcess);
            __m128 posClipped = _mm_add_ps(vThreshold,
                _mm_mul_ps(vHeadroom, divideNR(posExcess, posDenom)));

            __m128 negS = _mm_sub_ps(_mm_setzero_ps(), s);
            __m128 negExcess = _mm_mul_ps(_mm_sub_ps(negS, vThreshold), vInvHeadroom);
            __m128 negDenom = _mm_add_ps(vOne, negExcess);
            __m128 negClipped = _mm_sub_ps(_mm_setzero_ps(), _mm_add_ps(vThreshold,
                _mm_mul_ps(vHeadroom, divideNR(negExcess, negDenom))));

            __m128 result = _mm_or_ps(_mm_and_ps(posNeedClip, posClipped),
                                       _mm_andnot_ps(posNeedClip, s));
            result = _mm_or_ps(_mm_and_ps(negNeedClip, negClipped),
                               _mm_andnot_ps(negNeedClip, result));

            // Hard-clamp: SSE reciprocal approximation can overshoot ±1.0,
            // causing DAC/BT codec hard clipping artifacts
            __m128 clampedResult = _mm_min_ps(_mm_max_ps(result, vNegOne), vOne);
            // Detect if hard-clamp changed any lane
            __m128 clampDiff = _mm_cmpneq_ps(result, clampedResult);
            if (UNLIKELY(_mm_movemask_ps(clampDiff))) {
                clamped = true;
            }

            _mm_storeu_ps(samples + i, clampedResult);
        }

        // Scalar tail - use reciprocal approximation to avoid division
        for (size_t i = simdCount; i < count; ++i) {
            float s = samples[i];
            if (UNLIKELY(std::isnan(s))) { samples[i] = 0.0f; continue; }
            if (s > kThreshold) {
                float x = s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = kThreshold + x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            } else if (s < -kThreshold) {
                float x = -s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = -kThreshold - x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            }
        }
#else
        // Scalar fallback - single pass with reciprocal to avoid division
        for (size_t i = 0; i < count; ++i) {
            float s = samples[i];
            if (UNLIKELY(std::isnan(s))) { samples[i] = 0.0f; continue; }
            if (s > kThreshold) {
                float x = s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = kThreshold + x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            } else if (s < -kThreshold) {
                float x = -s - kThreshold;
                float denom = kHeadroom + x;
                float rcp = 1.0f / denom;
                float v = -kThreshold - x * kHeadroom * rcp;
                float cv = std::clamp(v, -1.0f, 1.0f);
                if (UNLIKELY(cv != v)) clamped = true;
                samples[i] = cv;
            }
        }
#endif
        if (UNLIKELY(clamped)) {
            softClipClamps_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Diagnostic: number of audio callbacks where hard-clamp activated after soft-clip
    std::atomic<uint64_t> softClipClamps_{0};

public:
    [[nodiscard]] uint64_t softClipClamps() const noexcept {
        return softClipClamps_.load(std::memory_order_relaxed);
    }
    void resetSoftClipClamps() noexcept {
        softClipClamps_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<AudioDecodeSlot*, kMaxSlots>* slots_ = nullptr;

    // Per-slot gain state for ramping (audio callback thread only, no synchronization needed)
    std::array<float, kMaxSlots> currentGain_{};

    // Per-slot contribution tracking for crossfade on underrun/resume transitions.
    // wasContributing_: true if slot produced audio in the previous mix() call.
    // fadeBuffers_: last kFadeSamples from each slot's raw output, used for fade-out
    // when a slot stops contributing (underrun → silence transition).
    std::array<bool, kMaxSlots> wasContributing_{};
    std::array<std::array<float, kFadeSamples>, kMaxSlots> fadeBuffers_{};

    // Per-slot comfort noise generators for sustained underrun periods.
    // Generates shaped noise (~-60dBFS) to replace dead silence.
    std::array<ComfortNoiseGenerator, kMaxSlots> cng_{};

    // Pre-allocated mixing buffer - avoids stack allocation in audio callback.
    // Audio callback threads have limited stack (typically 16KB on Android/iOS).
    alignas(config::kSimdAlignment) std::array<float, kMixBufferSize> tempBuffer_{};
};

}  // namespace media
