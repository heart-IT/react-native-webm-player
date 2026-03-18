#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>

namespace media {

struct EncodedVideoFrame {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    bool isKeyFrame = false;

    void clear() noexcept {
        data.clear();
        ptsUs = 0;
        isKeyFrame = false;
    }
};

struct VideoCaptureConfig {
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2'000'000;
    int keyFrameIntervalSec = 2;
    bool useFrontCamera = false;
};

struct VideoStreamMetrics {
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesEncoded{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> encodeErrors{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<int> currentFps{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<int64_t> jitterUs{0};
    std::atomic<int64_t> bufferTargetUs{0};
    std::atomic<uint64_t> lateFrames{0};
    std::atomic<uint64_t> skippedFrames{0};

    void reset() noexcept {
        framesReceived.store(0, std::memory_order_relaxed);
        framesEncoded.store(0, std::memory_order_relaxed);
        framesDecoded.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        encodeErrors.store(0, std::memory_order_relaxed);
        decodeErrors.store(0, std::memory_order_relaxed);
        currentFps.store(0, std::memory_order_relaxed);
        width.store(0, std::memory_order_relaxed);
        height.store(0, std::memory_order_relaxed);
        jitterUs.store(0, std::memory_order_relaxed);
        bufferTargetUs.store(0, std::memory_order_relaxed);
        lateFrames.store(0, std::memory_order_relaxed);
        skippedFrames.store(0, std::memory_order_relaxed);
    }
};

}  // namespace media
