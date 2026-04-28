#include "MediaCodecDecoder.h"
#include "common/MediaLog.h"
#include "video/VP9HeaderParser.h"
#include <cstring>

namespace videomodule {

bool MediaCodecDecoder::initialize(ANativeWindow* surface) {
    // Contract: caller passes a +1-ref'd surface (from VideoSurfaceRegistry::acquireSurface).
    // We take ownership on success; release the caller's ref on any early return so the
    // factory's acquire/release balance stays intact regardless of outcome.
    if (!surface) return false;

    if (surface_ == surface) {
        // Already holding this surface — release the caller's duplicate ref.
        ANativeWindow_release(surface);
        return true;
    }

    if (surface_) {
        shutdown();  // releases our prior surface_
    }
    surface_ = surface;

    // Defer codec configuration until first keyframe provides actual dimensions.
    // VP9 keyframe headers contain resolution — no benefit to pre-configuring
    // at a default size that may require an immediate reconfigure.
    initialized_.store(true, std::memory_order_release);
    return true;
}

void MediaCodecDecoder::shutdown() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) return;
    configured_.store(false, std::memory_order_release);
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (surface_) {
        ANativeWindow_release(surface_);
        surface_ = nullptr;
    }
    consecutiveErrors_ = 0;
    resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    lastResetTimeUs_ = 0;
}

bool MediaCodecDecoder::configureCodec(int width, int height) {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        configured_.store(false, std::memory_order_release);
    }

    if (!surface_) {
        MEDIA_LOG_E("MediaCodecDecoder: configureCodec called with null surface");
        return false;
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

    decodedWidth_.store(width, std::memory_order_relaxed);
    decodedHeight_.store(height, std::memory_order_relaxed);
    configured_.store(true, std::memory_order_release);
    MEDIA_LOG_I("MediaCodecDecoder: VP9 configured %dx%d", width, height);
    return true;
}

bool MediaCodecDecoder::submitFrame(const uint8_t* data, size_t size,
                                     int64_t ptsUs, bool isKeyFrame) {
    if (!initialized_.load(std::memory_order_acquire)) return false;
    if (!data || size == 0) return false;

    // Parse VP9 keyframe header for dimension detection and validation
    if (isKeyFrame) {
        auto info = media::vp9::parseHeader(data, size);
        if (!info.valid || info.width <= 0 || info.height <= 0) {
            MEDIA_LOG_E("MediaCodecDecoder: invalid VP9 keyframe header, requesting new keyframe");
            if (keyFrameRequestFn_) keyFrameRequestFn_();
            return false;
        }
        int curW = decodedWidth_.load(std::memory_order_relaxed);
        int curH = decodedHeight_.load(std::memory_order_relaxed);
        if (!configured_.load(std::memory_order_acquire) || !codec_ ||
            info.width != curW || info.height != curH) {
            MEDIA_LOG_I("MediaCodecDecoder: VP9 %sconfigure %dx%d",
                        configured_.load(std::memory_order_relaxed) ? "re" : "",
                        info.width, info.height);
            if (!configureCodec(info.width, info.height)) return false;
        }
    } else if (!configured_.load(std::memory_order_acquire) || !codec_) {
        return false;
    }

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
    if (status != AMEDIA_OK) {
        ++consecutiveErrors_;
        lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
        MEDIA_LOG_E("MediaCodecDecoder: queueInputBuffer failed: %d (consecutive=%d)",
                    static_cast<int>(status), consecutiveErrors_);
        bool fatalError = (status == AMEDIA_ERROR_UNKNOWN);
        if (fatalError || consecutiveErrors_ >= kMaxConsecutiveErrors) {
            int64_t now = media::nowUs();
            if (now - lastResetTimeUs_ < resetBackoffUs_) {
                return false;  // Within backoff window, just drop
            }
            MEDIA_LOG_W("MediaCodecDecoder: resetting decoder after %d errors (backoff=%lldms)",
                        consecutiveErrors_, static_cast<long long>(resetBackoffUs_ / 1000));
            consecutiveErrors_ = 0;
            lastResetTimeUs_ = now;
            resetBackoffUs_ = std::min(resetBackoffUs_ * 2, media::video_config::decoder_reset::kMaxBackoffUs);
            configureCodec(decodedWidth_.load(std::memory_order_relaxed),
                          decodedHeight_.load(std::memory_order_relaxed));
            if (keyFrameRequestFn_) keyFrameRequestFn_();
        }
        return false;
    }

    consecutiveErrors_ = 0;
    resetBackoffUs_ = media::video_config::decoder_reset::kInitialBackoffUs;
    return true;
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
                        decodedWidth_.store(w, std::memory_order_relaxed);
                        decodedHeight_.store(h, std::memory_order_relaxed);
                        MEDIA_LOG_I("MediaCodecDecoder: resolution changed to %dx%d", w, h);
                    }
                }
                AMediaFormat_delete(outputFormat);
            }
            continue;
        } else if (outputIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        } else if (outputIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break;  // Normal "nothing ready" signal — not an error.
        } else {
            // Any other negative is a real error: OMX crash, codec death, etc.
            // Treat as fatal for this decoder instance: bump metrics, flag for
            // reset, request a keyframe so health watchdog can recover.
            lastError_.store(static_cast<int>(outputIdx), std::memory_order_relaxed);
            ++consecutiveErrors_;
            MEDIA_LOG_E("MediaCodecDecoder: dequeueOutputBuffer failed: %lld (consecutive=%d)",
                        static_cast<long long>(outputIdx), consecutiveErrors_);
            if (consecutiveErrors_ >= kMaxConsecutiveErrors) {
                int64_t now = media::nowUs();
                if (now - lastResetTimeUs_ >= resetBackoffUs_) {
                    MEDIA_LOG_W("MediaCodecDecoder: output-path reset after %d errors", consecutiveErrors_);
                    consecutiveErrors_ = 0;
                    lastResetTimeUs_ = now;
                    resetBackoffUs_ = std::min(resetBackoffUs_ * 2,
                                               media::video_config::decoder_reset::kMaxBackoffUs);
                    configureCodec(decodedWidth_.load(std::memory_order_relaxed),
                                   decodedHeight_.load(std::memory_order_relaxed));
                    if (keyFrameRequestFn_) keyFrameRequestFn_();
                }
            }
            break;
        }
    }

    return released;
}

void MediaCodecDecoder::postDecode() {
    releaseOutputBuffers();
}

}  // namespace videomodule
