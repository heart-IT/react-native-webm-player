// Audio pipeline data types: frames, stream state, and metrics counters.
// All metric counters are std::atomic for lock-free access from the audio callback thread.
#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <type_traits>
#include "common/MediaConfig.h"

namespace media {

// Audio pipeline lifecycle state.
enum class StreamState : uint8_t {
    Inactive = 0,
    Buffering,
    Playing,
    Underrun,
    Paused
};

struct RawAudioFrame {
    long long absOffset = 0;
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
// FramePool returns frames by index and the token's destructor runs from the audio
// callback (via DecodedAudioToken::release). Non-trivial destruction would be
// RT-unsafe — tripwire any future member that breaks this invariant.
static_assert(std::is_trivially_destructible_v<DecodedAudioFrame>,
              "DecodedAudioFrame must remain trivially destructible for RT-safe pool release");

struct PendingAudioFrame {
    long long absOffset = 0;
    size_t size = 0;
    int64_t ptsUs = 0;
    int64_t durationUs = 0;

    void clear() noexcept {
        absOffset = 0;
        size = 0;
        ptsUs = 0;
        durationUs = 0;
    }
};

struct StreamMetrics {
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesDropped{0};        // rolling total — sum of named counters below
    std::atomic<uint64_t> oversizedFrameDrops{0};   // size==0 or > kMaxEncodedFrameSize at push
    std::atomic<uint64_t> bufferFullDrops{0};       // bufferedDurationUs >= kMaxBufferedUs at push
    std::atomic<uint64_t> encodedPoolExhaustionDrops{0};  // encQueue_.acquire() returned null
    std::atomic<uint64_t> encodedPushFailDrops{0};  // encQueue_.push() failed (queue full)
    std::atomic<uint64_t> decodedPushFailDrops{0};  // decodedQueue_.push() failed (PLC/FEC/normal)
    std::atomic<uint64_t> framesDrained{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<uint64_t> decoderResets{0};
    std::atomic<uint64_t> underruns{0};             // Playing→Underrun state-edge transitions
    std::atomic<uint64_t> silenceCallbacks{0};      // audio callbacks that produced zero samples
                                                    // (per-callback event counter — what triage trees expect)
    std::atomic<uint64_t> plcFrames{0};             // total PLC concealment frames generated
    std::atomic<uint64_t> fecFrames{0};             // frames recovered via Opus in-band FEC
    std::atomic<uint64_t> silenceSkipFrames{0};     // silence frames skipped for latency catchup
    std::atomic<uint64_t> samplesOutput{0};
    std::atomic<uint64_t> ptsDiscontinuities{0};

    // Playback diagnostics
    std::atomic<uint64_t> fastPathSwitches{0};       // fast-path ↔ resampler mode transitions

    // PLC quality tracking
    std::atomic<uint32_t> peakConsecutivePLC{0};      // high-water mark of consecutive PLC frames

    // Opus decode performance
    std::atomic<int64_t> decodeLatencyUs{0};          // EWMA of opus_decode_float() duration
    std::atomic<int32_t> lastDecodeError{0};          // last non-zero Opus error code (OPUS_* negative)

    // Inter-frame delivery gap tracking (pushEncodedFrame timing)
    std::atomic<int64_t> maxInterFrameGapUs{0};      // largest gap between consecutive pushes
    std::atomic<uint64_t> gapsOver50ms{0};            // delivery gaps > 50ms
    std::atomic<uint64_t> gapsOver100ms{0};           // delivery gaps > 100ms
    std::atomic<uint64_t> gapsOver500ms{0};           // delivery gaps > 500ms

    void reset() noexcept {
        framesReceived.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        oversizedFrameDrops.store(0, std::memory_order_relaxed);
        bufferFullDrops.store(0, std::memory_order_relaxed);
        encodedPoolExhaustionDrops.store(0, std::memory_order_relaxed);
        encodedPushFailDrops.store(0, std::memory_order_relaxed);
        decodedPushFailDrops.store(0, std::memory_order_relaxed);
        framesDrained.store(0, std::memory_order_relaxed);
        decodeErrors.store(0, std::memory_order_relaxed);
        decoderResets.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        silenceCallbacks.store(0, std::memory_order_relaxed);
        plcFrames.store(0, std::memory_order_relaxed);
        fecFrames.store(0, std::memory_order_relaxed);
        silenceSkipFrames.store(0, std::memory_order_relaxed);
        samplesOutput.store(0, std::memory_order_relaxed);
        ptsDiscontinuities.store(0, std::memory_order_relaxed);
        peakConsecutivePLC.store(0, std::memory_order_relaxed);
        fastPathSwitches.store(0, std::memory_order_relaxed);
        decodeLatencyUs.store(0, std::memory_order_relaxed);
        lastDecodeError.store(0, std::memory_order_relaxed);
        maxInterFrameGapUs.store(0, std::memory_order_relaxed);
        gapsOver50ms.store(0, std::memory_order_relaxed);
        gapsOver100ms.store(0, std::memory_order_relaxed);
        gapsOver500ms.store(0, std::memory_order_relaxed);
    }
};

enum class LatencyMode : uint8_t {
    Unknown = 0,
    LowLatency,   // Lowest latency (exclusive hardware access on Android)
    Standard       // Higher latency (shared with other apps on Android)
};

inline const char* latencyModeToString(LatencyMode mode) noexcept {
    switch (mode) {
        case LatencyMode::LowLatency: return "low_latency";
        case LatencyMode::Standard:   return "standard";
        default:                      return "unknown";
    }
}

}  // namespace media
