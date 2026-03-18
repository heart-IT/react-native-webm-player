#include "MediaCodecDecoder.h"
#include "common/MediaLog.h"
#include <cstring>

namespace videomodule {

bool MediaCodecDecoder::initialize(ANativeWindow* surface) {
    if (!surface) return false;

    surface_ = surface;
    initialized_.store(true, std::memory_order_release);

    // Configure VP9 decoder immediately — no SPS/PPS handshake needed
    return configureCodec(media::video_config::kDefaultWidth, media::video_config::kDefaultHeight);
}

void MediaCodecDecoder::shutdown() {
    initialized_.store(false, std::memory_order_release);
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (surface_) {
        ANativeWindow_release(surface_);
        surface_ = nullptr;
    }
    configured_.store(false, std::memory_order_relaxed);
}

bool MediaCodecDecoder::configureCodec(int width, int height) {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        configured_.store(false, std::memory_order_release);
    }

    // VP9 MIME type — HW decode universal on API 29+
    codec_ = AMediaCodec_createDecoderByType("video/x-vp9");
    if (!codec_) {
        MEDIA_LOG_E("MediaCodecDecoder: failed to create VP9 decoder");
        return false;
    }

    AMediaFormat* format = AMediaFormat_new();
    if (!format) {
        MEDIA_LOG_E("MediaCodecDecoder: AMediaFormat_new failed");
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/x-vp9");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);

    media_status_t status = AMediaCodec_configure(codec_, format, surface_, nullptr, 0);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        MEDIA_LOG_E("MediaCodecDecoder: configure failed: %d", static_cast<int>(status));
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        MEDIA_LOG_E("MediaCodecDecoder: start failed: %d", static_cast<int>(status));
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    decodedWidth_ = width;
    decodedHeight_ = height;
    configured_.store(true, std::memory_order_release);
    MEDIA_LOG_I("MediaCodecDecoder: VP9 configured %dx%d", width, height);
    return true;
}

bool MediaCodecDecoder::submitFrame(const uint8_t* data, size_t size,
                                     int64_t ptsUs, bool isKeyFrame) {
    if (!initialized_.load(std::memory_order_acquire)) return false;
    if (!data || size == 0) return false;
    if (!configured_.load(std::memory_order_acquire) || !codec_) return false;

    ssize_t inputIdx = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (inputIdx < 0) return false;

    size_t bufferSize = 0;
    uint8_t* inputBuffer = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(inputIdx), &bufferSize);
    if (!inputBuffer || bufferSize < size) {
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(inputIdx), 0, 0, 0, 0);
        return false;
    }

    std::memcpy(inputBuffer, data, size);

    uint32_t flags = isKeyFrame ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
    media_status_t status = AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(inputIdx),
                                                           0, size, static_cast<uint64_t>(ptsUs), flags);
    return status == AMEDIA_OK;
}

int MediaCodecDecoder::releaseOutputBuffers() {
    if (!codec_) return 0;

    int released = 0;
    AMediaCodecBufferInfo info;

    while (true) {
        ssize_t outputIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (outputIdx >= 0) {
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(outputIdx), true);
            ++released;
        } else if (outputIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // VP9 adaptive resolution: update dimensions when format changes
            AMediaFormat* outputFormat = AMediaCodec_getOutputFormat(codec_);
            if (outputFormat) {
                int32_t w = 0, h = 0;
                if (AMediaFormat_getInt32(outputFormat, AMEDIAFORMAT_KEY_WIDTH, &w) &&
                    AMediaFormat_getInt32(outputFormat, AMEDIAFORMAT_KEY_HEIGHT, &h)) {
                    if (w > 0 && h > 0) {
                        decodedWidth_ = w;
                        decodedHeight_ = h;
                        MEDIA_LOG_I("MediaCodecDecoder: resolution changed to %dx%d", w, h);
                    }
                }
                AMediaFormat_delete(outputFormat);
            }
            continue;
        } else if (outputIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        } else {
            break;
        }
    }

    return released;
}

void MediaCodecDecoder::flush() {
    if (!codec_) return;
    AMediaCodec_flush(codec_);
}

}  // namespace videomodule
