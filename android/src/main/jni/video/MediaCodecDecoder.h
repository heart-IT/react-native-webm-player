// Android VP9 hardware decoder using MediaCodec NDK API.
// Submits encoded VP9 frames and renders decoded output to an ANativeWindow surface.
// Implements the VideoDecoder interface for use by VideoDecodeThread.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <functional>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include "video/VideoDecoder.h"
#include "video/VideoConfig.h"
#include "common/MediaTime.h"

namespace videomodule {

// VP9 hardware decoder via MediaCodec.
// VP9 HW decode is universal on Android API 29+ (our min SDK).
// VP9 frames are raw bitstream — no NAL/Annex B conversion needed.
class MediaCodecDecoder : public media::VideoDecoder {
public:
    MediaCodecDecoder() = default;
    ~MediaCodecDecoder() { shutdown(); }

    MediaCodecDecoder(const MediaCodecDecoder&) = delete;
    MediaCodecDecoder& operator=(const MediaCodecDecoder&) = delete;

    bool initialize(ANativeWindow* surface);
    void shutdown() override;
    bool isInitialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

    void setKeyFrameRequestFn(std::function<void()> fn) noexcept { keyFrameRequestFn_ = std::move(fn); }

    bool submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame) override;
    void postDecode() override;
    int releaseOutputBuffers();

    int decodedWidth() const override { return decodedWidth_.load(std::memory_order_relaxed); }
    int decodedHeight() const override { return decodedHeight_.load(std::memory_order_relaxed); }
    int lastError() const override { return lastError_.load(std::memory_order_relaxed); }

private:
    bool configureCodec(int width, int height);

    static constexpr int kMaxConsecutiveErrors = media::video_config::decoder_reset::kMaxConsecutiveErrors;

    AMediaCodec* codec_ = nullptr;
    ANativeWindow* surface_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> configured_{false};

    std::atomic<int> decodedWidth_{0};
    std::atomic<int> decodedHeight_{0};
    int consecutiveErrors_ = 0;
    std::atomic<int> lastError_{0};
    int64_t lastResetTimeUs_ = 0;
    int64_t resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    std::function<void()> keyFrameRequestFn_;
};

}  // namespace videomodule
