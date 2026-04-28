#pragma once

// RAII wrapper around the Speex resampler (from xiph/speexdsp).
// Provides int16 and float resampling (int16 exercised by UBSan tests).
// Real-time safe after init (no allocations in process path).

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <speex/speex_resampler.h>
#include "MediaLog.h"

namespace media {

// Quality setting for Speex resampler (0-10).
// 3 = good quality, low CPU (recommended for mobile VoIP).
constexpr int kResamplerQuality = 3;

namespace detail {

template<typename T>
struct ResamplerTraits;

template<>
struct ResamplerTraits<int16_t> {
    static constexpr const char* name = "AudioResampler<int16>";
    static int process(SpeexResamplerState* st, spx_uint32_t channel,
                       const int16_t* in, spx_uint32_t* inLen,
                       int16_t* out, spx_uint32_t* outLen) {
        return speex_resampler_process_int(st, channel, in, inLen, out, outLen);
    }
};

template<>
struct ResamplerTraits<float> {
    static constexpr const char* name = "AudioResampler<float>";
    static int process(SpeexResamplerState* st, spx_uint32_t channel,
                       const float* in, spx_uint32_t* inLen,
                       float* out, spx_uint32_t* outLen) {
        return speex_resampler_process_float(st, channel, in, inLen, out, outLen);
    }
};

}  // namespace detail

template<typename T>
class AudioResampler {
    using Traits = detail::ResamplerTraits<T>;

public:
    AudioResampler() = default;
    ~AudioResampler() { destroy(); }

    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;

    bool init(int32_t srcRate, int32_t dstRate, int channels = 1) noexcept {
        destroy();
        if (srcRate == dstRate) {
            passthrough_ = true;
            return true;
        }

        int err = 0;
        state_ = speex_resampler_init(
            static_cast<spx_uint32_t>(channels),
            static_cast<spx_uint32_t>(srcRate),
            static_cast<spx_uint32_t>(dstRate),
            kResamplerQuality, &err);

        if (!state_ || err != RESAMPLER_ERR_SUCCESS) {
            MEDIA_LOG_E("%s: init failed (src=%d dst=%d err=%d)",
                        Traits::name, srcRate, dstRate, err);
            state_ = nullptr;
            return false;
        }

        srcRate_ = srcRate;
        dstRate_ = dstRate;
        passthrough_ = false;
        MEDIA_LOG_I("%s: initialized %d -> %d Hz (quality=%d)",
                    Traits::name, srcRate, dstRate, kResamplerQuality);
        return true;
    }

    void destroy() noexcept {
        if (state_) {
            speex_resampler_destroy(state_);
            state_ = nullptr;
        }
        passthrough_ = false;
    }

    void reset() noexcept {
        if (state_) {
            speex_resampler_reset_mem(state_);
        }
    }

    [[nodiscard]] bool isPassthrough() const noexcept { return passthrough_; }
    [[nodiscard]] bool isInitialized() const noexcept { return passthrough_ || state_ != nullptr; }

    // Returns number of output samples written.
    // Caller must ensure output buffer is large enough (use maxOutputSamples()).
    size_t process(T* output, const T* input, size_t inputCount) noexcept {
        if (passthrough_) {
            std::memcpy(output, input, inputCount * sizeof(T));
            return inputCount;
        }
        if (!state_) return 0;

        spx_uint32_t inLen = static_cast<spx_uint32_t>(inputCount);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(maxOutputSamples(inputCount));

        int err = Traits::process(state_, 0, input, &inLen, output, &outLen);
        if (err != RESAMPLER_ERR_SUCCESS) {
            // Atomic counter only — no logging here. process() is called from
            // iOS audio render callbacks (capture + playback) where I/O is forbidden.
            // Error count is surfaced via processErrorCount() for non-RT diagnostics.
            processErrors_.fetch_add(1, std::memory_order_relaxed);
            // Return 0 — caller is responsible for zero-padding output on short writes.
            // (Previously zeroed maxOutputSamples() bytes here, but that could exceed
            // the caller's output buffer when dstRate > srcRate.)
            return 0;
        }

        return static_cast<size_t>(outLen);
    }

    [[nodiscard]] uint64_t processErrorCount() const noexcept {
        return processErrors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t maxOutputSamples(size_t inputCount) const noexcept {
        if (passthrough_) return inputCount;
        if (srcRate_ == 0) return 0;
        // Ceiling division to avoid underallocation
        return (inputCount * static_cast<size_t>(dstRate_) + static_cast<size_t>(srcRate_) - 1)
               / static_cast<size_t>(srcRate_);
    }

private:
    SpeexResamplerState* state_{nullptr};
    int32_t srcRate_{0};
    int32_t dstRate_{0};
    bool passthrough_{false};
    std::atomic<uint64_t> processErrors_{0};
};

using AudioResamplerFloat = AudioResampler<float>;

}  // namespace media
