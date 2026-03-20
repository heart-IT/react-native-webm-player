#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <functional>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include "video/VideoConfig.h"
#include "video/VideoTypes.h"
#include "common/MediaTime.h"

namespace videomodule {

// VP9 hardware decoder via MediaCodec.
// VP9 HW decode is universal on Android API 29+ (our min SDK).
// VP9 frames are raw bitstream — no NAL/Annex B conversion needed.
class MediaCodecDecoder {
public:
    MediaCodecDecoder() = default;
    ~MediaCodecDecoder() { shutdown(); }

    MediaCodecDecoder(const MediaCodecDecoder&) = delete;
    MediaCodecDecoder& operator=(const MediaCodecDecoder&) = delete;

    bool initialize(ANativeWindow* surface);
    void shutdown();
    bool isInitialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

    // Submit VP9 raw frame for decoding.
    void setVideoMetrics(media::VideoStreamMetrics* m) noexcept { videoMetrics_ = m; }
    void setKeyFrameRequestFn(std::function<void()> fn) noexcept { keyFrameRequestFn_ = std::move(fn); }

    bool submitFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyFrame);

    // Release decoded output buffers (renders them to the surface).
    int releaseOutputBuffers();

    void flush();

    int decodedWidth() const noexcept { return decodedWidth_; }
    int decodedHeight() const noexcept { return decodedHeight_; }

private:
    bool configureCodec(int width, int height);

    static constexpr int kMaxConsecutiveErrors = 5;

    AMediaCodec* codec_ = nullptr;
    ANativeWindow* surface_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> configured_{false};

    int decodedWidth_ = 0;
    int decodedHeight_ = 0;
    int consecutiveErrors_ = 0;
    int64_t lastResetTimeUs_ = 0;
    int64_t resetBackoffUs_ = 100000;  // 100ms initial backoff
    int resetsSinceSuccess_ = 0;
    media::VideoStreamMetrics* videoMetrics_ = nullptr;
    std::function<void()> keyFrameRequestFn_;
};

}  // namespace videomodule
