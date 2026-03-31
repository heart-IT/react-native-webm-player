#pragma once

#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstdint>
#include <functional>
#include <atomic>
#include "../../cpp/video/VideoDecoder.h"
#include "../../cpp/common/MediaTime.h"

namespace videomodule {

class VP9Decoder : public media::VideoDecoder {
public:
    VP9Decoder();
    ~VP9Decoder() override { shutdown(); }

    VP9Decoder(const VP9Decoder&) = delete;
    VP9Decoder& operator=(const VP9Decoder&) = delete;

    void setOutputLayer(AVSampleBufferDisplayLayer* layer);
    void setFps(int fps) { fps_ = fps > 0 ? fps : 30; }
    void setKeyFrameRequestFn(std::function<void()> fn) { keyFrameRequestFn_ = std::move(fn); }
    void shutdown() override;
    bool isInitialized() const noexcept { return layer_.load(std::memory_order_acquire) != nil; }

    bool submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame) override;

    int decodedWidth() const override { return decodedWidth_.load(std::memory_order_relaxed); }
    int decodedHeight() const override { return decodedHeight_.load(std::memory_order_relaxed); }

private:
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

    std::atomic<AVSampleBufferDisplayLayer*> layer_{nil};
    VTDecompressionSessionRef session_ = nullptr;
    CMVideoFormatDescriptionRef formatDesc_ = nullptr;

    static constexpr int kMaxConsecutiveErrors = 5;

    int fps_ = 30;
    int sessionWidth_ = 0;
    int sessionHeight_ = 0;
    std::atomic<int> decodedWidth_{0};
    std::atomic<int> decodedHeight_{0};
    bool hwSupported_ = false;
    int consecutiveErrors_ = 0;
    int64_t lastResetTimeUs_ = 0;
    int64_t resetBackoffUs_ = 100000;
    int resetsSinceSuccess_ = 0;
    std::function<void()> keyFrameRequestFn_;
    dispatch_queue_t renderQueue_ = nullptr;
};

}  // namespace videomodule
