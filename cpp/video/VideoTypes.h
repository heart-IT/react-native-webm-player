#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>

namespace media {

// Observable state of the video decoder lifecycle
enum class VideoDecoderState : uint8_t {
    NotCreated = 0,      // Decoder not yet created (initial state)
    WaitingSurface = 1,  // No render surface registered
    BackingOff = 2,      // Post-error backoff before retry
    Active = 3,          // Decoder created and decoding
    Failed = 4           // Factory returned null
};

struct EncodedVideoFrame {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    bool isKeyFrame = false;
};

struct VideoStreamMetrics {
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<int> currentFps{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<int64_t> jitterUs{0};
    std::atomic<int64_t> bufferTargetUs{0};
    std::atomic<uint64_t> lateFrames{0};
    std::atomic<uint64_t> skippedFrames{0};
    std::atomic<uint64_t> decoderResets{0};
    std::atomic<uint64_t> surfaceLostCount{0};

    // Inter-frame delivery gap tracking
    std::atomic<int64_t> maxInterFrameGapUs{0};
    std::atomic<uint64_t> gapsOver50ms{0};
    std::atomic<uint64_t> gapsOver100ms{0};
    std::atomic<uint64_t> gapsOver500ms{0};

    // Clock drift tracking
    std::atomic<int32_t> driftPpm{0};

    // Decode performance
    std::atomic<int64_t> decodeLatencyUs{0};       // EWMA of submitFrame() duration
    std::atomic<int64_t> lastDecodeTimeUs{0};       // wall-clock time of last successful decode
    std::atomic<int32_t> lastDecodeError{0};        // last platform decode error code (media_status_t / OSStatus)

    // Keyframe recovery state
    std::atomic<bool> needsKeyFrame{true};
    std::atomic<uint64_t> keyFrameRequests{0};

    // Decoder lifecycle state (for triage: why is decode not happening?)
    std::atomic<uint8_t> decoderState{static_cast<uint8_t>(VideoDecoderState::NotCreated)};

    void reset() noexcept {
        framesReceived.store(0, std::memory_order_relaxed);
        framesDecoded.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        decodeErrors.store(0, std::memory_order_relaxed);
        currentFps.store(0, std::memory_order_relaxed);
        width.store(0, std::memory_order_relaxed);
        height.store(0, std::memory_order_relaxed);
        jitterUs.store(0, std::memory_order_relaxed);
        bufferTargetUs.store(0, std::memory_order_relaxed);
        lateFrames.store(0, std::memory_order_relaxed);
        skippedFrames.store(0, std::memory_order_relaxed);
        decoderResets.store(0, std::memory_order_relaxed);
        surfaceLostCount.store(0, std::memory_order_relaxed);
        maxInterFrameGapUs.store(0, std::memory_order_relaxed);
        gapsOver50ms.store(0, std::memory_order_relaxed);
        gapsOver100ms.store(0, std::memory_order_relaxed);
        gapsOver500ms.store(0, std::memory_order_relaxed);
        driftPpm.store(0, std::memory_order_relaxed);
        decodeLatencyUs.store(0, std::memory_order_relaxed);
        lastDecodeTimeUs.store(0, std::memory_order_relaxed);
        lastDecodeError.store(0, std::memory_order_relaxed);
        needsKeyFrame.store(true, std::memory_order_relaxed);
        keyFrameRequests.store(0, std::memory_order_relaxed);
        decoderState.store(static_cast<uint8_t>(VideoDecoderState::NotCreated), std::memory_order_relaxed);
    }
};

}  // namespace media
