#pragma once

#include <cstdint>
#include <cstddef>
#include "common/MediaConfig.h"

namespace media {

// Generates spectrally-shaped comfort noise for DTX silence periods.
// Pure digital silence sounds "dead" — low-level shaped noise (~-60dBFS)
// maintains the perception of a live connection.
//
// Uses a Galois LFSR for white noise generation (no allocations, deterministic,
// audio-thread safe) and a single-pole IIR lowpass to roll off above ~1kHz,
// approximating room tone character.
//
// Thread safety: not thread-safe. Each OpusDecoderAdapter owns its own instance,
// called exclusively from the decode thread.
class ComfortNoiseGenerator {
public:
    // Fill output buffer with shaped comfort noise.
    // sampleCount: number of mono samples to generate (typically kFrameSamples = 960)
    void generate(float* output, size_t sampleCount) noexcept {
        for (size_t i = 0; i < sampleCount; ++i) {
            // Galois LFSR: maximal-length 32-bit with polynomial 0xB4BCD35C
            // Period: 2^32 - 1 (~4 billion samples = ~24 hours at 48kHz)
            uint32_t bit = lfsr_ & 1u;
            lfsr_ >>= 1;
            if (bit) {
                lfsr_ ^= 0xB4BCD35Cu;
            }

            // Convert to float [-1, 1]: map [0, UINT32_MAX] → [-1, 1]
            float white = static_cast<float>(static_cast<int32_t>(lfsr_)) * kIntToFloat;

            // Single-pole IIR lowpass: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
            // Cutoff ~1kHz at 48kHz sample rate
            lpState_ += kAlpha * (white - lpState_);

            output[i] = lpState_ * config::dtx::kComfortNoiseLevel;
        }
    }

    void reset() noexcept {
        lfsr_ = kDefaultSeed;
        lpState_ = 0.0f;
    }

private:
    static constexpr uint32_t kDefaultSeed = 0xACE1u;

    // Single-pole IIR coefficient: 2 * pi * cutoffHz / sampleRate
    static constexpr float kAlpha = 2.0f * 3.14159265f * config::dtx::kComfortNoiseCutoffHz
                                    / static_cast<float>(config::audio::kSampleRate);

    // Scale factor: 1.0 / 2^31 to map int32_t range to [-1, 1]
    static constexpr float kIntToFloat = 1.0f / 2147483648.0f;

    uint32_t lfsr_ = kDefaultSeed;
    float lpState_ = 0.0f;
};

}  // namespace media
