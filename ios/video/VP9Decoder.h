// iOS VP9 hardware decoder using VTDecompressionSession.
// Submits encoded VP9 frames and renders decoded output to an AVSampleBufferDisplayLayer.
// Implements the VideoDecoder interface for use by VideoDecodeThread.
#pragma once

#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include "../../cpp/video/VideoDecoder.h"
#include "../../cpp/video/VideoConfig.h"
#include "../../cpp/common/MediaTime.h"

namespace videomodule {

class VP9Decoder : public media::VideoDecoder {
public:
    VP9Decoder();
    ~VP9Decoder() override {
        shutdown();
    }

    VP9Decoder(const VP9Decoder&) = delete;
    VP9Decoder& operator=(const VP9Decoder&) = delete;

    void setOutputLayer(AVSampleBufferDisplayLayer* layer);
    void setFps(int fps) { fps_ = fps > 0 ? fps : 30; }
    void setKeyFrameRequestFn(std::function<void()> fn) { keyFrameRequestFn_ = std::move(fn); }
    void shutdown() override;
    bool isInitialized() const noexcept { return layerRaw_.load(std::memory_order_acquire) != nullptr; }

    bool submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame) override;

    int decodedWidth() const override { return decodedWidth_.load(std::memory_order_relaxed); }
    int decodedHeight() const override { return decodedHeight_.load(std::memory_order_relaxed); }
    int lastError() const override { return lastError_.load(std::memory_order_relaxed); }

private:
    bool createDecompressionSession(int width, int height, int profile);
    void destroyDecompressionSession();
    void outputCallback(CVImageBufferRef imageBuffer, int64_t ptsUs);

    static void vtDecompressionCallback(void* decompressionOutputRefCon,
                                         void* sourceFrameRefCon,
                                         OSStatus status,
                                         VTDecodeInfoFlags infoFlags,
                                         CVImageBufferRef imageBuffer,
                                         CMTime presentationTimeStamp,
                                         CMTime presentationDuration);

    std::atomic<void*> layerRaw_{nullptr};  // Unretained bridge — see setOutputLayer/shutdown

    // Guards session_/formatDesc_ against concurrent submitFrame (decode thread) vs
    // shutdown (any thread — e.g. surface release from main). Held for the duration of
    // VTDecompressionSessionDecodeFrame; shutdown blocks until in-flight submit returns.
    std::mutex sessionMutex_;
    VTDecompressionSessionRef session_ = nullptr;
    CMVideoFormatDescriptionRef formatDesc_ = nullptr;

    static constexpr int kMaxConsecutiveErrors = media::video_config::decoder_reset::kMaxConsecutiveErrors;

    int fps_ = 30;
    int sessionWidth_ = 0;
    int sessionHeight_ = 0;
    std::atomic<int> decodedWidth_{0};
    std::atomic<int> decodedHeight_{0};
    bool hwSupported_ = false;
    int consecutiveErrors_ = 0;
    std::atomic<int> lastError_{0};
    int64_t lastResetTimeUs_ = 0;
    int64_t resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    std::function<void()> keyFrameRequestFn_;
    dispatch_queue_t renderQueue_ = nullptr;
};

}  // namespace videomodule
