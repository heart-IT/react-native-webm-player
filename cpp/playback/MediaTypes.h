#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include "common/MediaConfig.h"

namespace media {

enum class StreamState : uint8_t {
    Inactive = 0,
    Buffering,
    Playing,
    Underrun
};

struct RawAudioFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int64_t timestampUs = 0;
    int64_t durationUs = 0;
};

struct DecodedAudioFrame {
    alignas(config::kSimdAlignment) std::array<float, config::audio::kFrameSize> samples{};
    uint32_t sampleCount = 0;
    int64_t ptsUs = 0;
    int64_t durationUs = 0;
    bool isConcealed = false;  // true for PLC and FEC frames

    void clear() noexcept {
        sampleCount = 0;
        ptsUs = 0;
        durationUs = 0;
        isConcealed = false;
    }
};

struct PendingAudioFrame {
    std::array<uint8_t, config::audio::kMaxEncodedFrameSize> data{};
    size_t size = 0;
    int64_t ptsUs = 0;
    int64_t durationUs = 0;

    void clear() noexcept {
        size = 0;
        ptsUs = 0;
        durationUs = 0;
    }
};

struct StreamMetrics {
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> framesDrained{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<uint64_t> decoderResets{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> samplesOutput{0};
    std::atomic<uint64_t> ptsDiscontinuities{0};

    // Bluetooth crackling diagnostics
    std::atomic<uint64_t> fastPathSwitches{0};       // fast-path ↔ resampler mode transitions
    std::atomic<uint64_t> softClipClamps{0};         // hard-clamp activations after soft-clip
    std::atomic<uint64_t> catchupDeadZoneSnaps{0};   // catchup ratio snapped to unity

    // Inter-frame delivery gap tracking (pushEncodedFrame timing)
    std::atomic<int64_t> maxInterFrameGapUs{0};      // largest gap between consecutive pushes
    std::atomic<uint64_t> gapsOver50ms{0};            // delivery gaps > 50ms
    std::atomic<uint64_t> gapsOver100ms{0};           // delivery gaps > 100ms
    std::atomic<uint64_t> gapsOver500ms{0};           // delivery gaps > 500ms

    void reset() noexcept {
        framesReceived.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        framesDrained.store(0, std::memory_order_relaxed);
        decodeErrors.store(0, std::memory_order_relaxed);
        decoderResets.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        samplesOutput.store(0, std::memory_order_relaxed);
        ptsDiscontinuities.store(0, std::memory_order_relaxed);
        fastPathSwitches.store(0, std::memory_order_relaxed);
        softClipClamps.store(0, std::memory_order_relaxed);
        catchupDeadZoneSnaps.store(0, std::memory_order_relaxed);
        maxInterFrameGapUs.store(0, std::memory_order_relaxed);
        gapsOver50ms.store(0, std::memory_order_relaxed);
        gapsOver100ms.store(0, std::memory_order_relaxed);
        gapsOver500ms.store(0, std::memory_order_relaxed);
    }
};

}  // namespace media
