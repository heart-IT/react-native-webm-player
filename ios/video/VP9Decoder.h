#pragma once

#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstdint>
#include <functional>
#include <atomic>

namespace videomodule {

class VP9Decoder {
public:
    VP9Decoder() = default;
    ~VP9Decoder() { shutdown(); }

    VP9Decoder(const VP9Decoder&) = delete;
    VP9Decoder& operator=(const VP9Decoder&) = delete;

    void setOutputLayer(AVSampleBufferDisplayLayer* layer);
    void setFps(int fps) { fps_ = fps > 0 ? fps : 30; }
    void setKeyFrameRequestFn(std::function<void()> fn) { keyFrameRequestFn_ = std::move(fn); }
    void shutdown();
    bool isInitialized() const noexcept { return layer_ != nil; }

    bool submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame);

    int decodedWidth() const noexcept { return decodedWidth_.load(std::memory_order_relaxed); }
    int decodedHeight() const noexcept { return decodedHeight_.load(std::memory_order_relaxed); }

private:
    struct VP9FrameInfo {
        int width = 0;
        int height = 0;
        int profile = 0;
        bool isKeyFrame = false;
        bool valid = false;
    };

    VP9FrameInfo parseVP9Header(const uint8_t* data, size_t size);
    bool createDecompressionSession(int width, int height);
    void destroyDecompressionSession();
    void outputCallback(CVImageBufferRef imageBuffer, int64_t ptsUs);

    static void vtDecompressionCallback(void* decompressionOutputRefCon,
                                         void* sourceFrameRefCon,
                                         OSStatus status,
                                         VTDecodeInfoFlags infoFlags,
                                         CVImageBufferRef imageBuffer,
                                         CMTime presentationTimeStamp,
                                         CMTime presentationDuration);

    AVSampleBufferDisplayLayer* layer_ = nil;
    VTDecompressionSessionRef session_ = nullptr;
    CMVideoFormatDescriptionRef formatDesc_ = nullptr;

    int fps_ = 30;
    int sessionWidth_ = 0;
    int sessionHeight_ = 0;
    std::atomic<int> decodedWidth_{0};
    std::atomic<int> decodedHeight_{0};
    bool hwSupported_ = false;
    std::function<void()> keyFrameRequestFn_;
};

}  // namespace videomodule
