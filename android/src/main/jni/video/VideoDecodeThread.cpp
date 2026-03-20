#include "VideoDecodeThread.h"
#include "common/MediaLog.h"
#include "common/MediaTime.h"
#include "common/ThreadAffinity.h"
#include <android/native_window_jni.h>

namespace videomodule {

bool VideoDecodeThread::start(media::VideoFrameQueue* queue) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (!queue) return false;

    frameQueue_ = queue;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&VideoDecodeThread::run, this);
    return true;
}

void VideoDecodeThread::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }

    std::lock_guard<std::mutex> lk(decoderMtx_);
    decoder_.reset();
    renderClock_.reset();
    fpsCounter_.reset();
    lastAnchorTimeUs_ = 0;
    syncCoordinator_.reset();
    frameQueue_ = nullptr;
}

MediaCodecDecoder* VideoDecodeThread::getOrCreateDecoder() {
    std::lock_guard<std::mutex> lk(decoderMtx_);

    if (decoder_) return decoder_.get();

    auto* surface = static_cast<ANativeWindow*>(
        media::VideoSurfaceRegistry::instance().acquireSurface());
    if (!surface) return nullptr;

    auto dec = std::make_unique<MediaCodecDecoder>();
    if (!dec->initialize(surface)) {
        ANativeWindow_release(surface);
        MEDIA_LOG_E("VideoDecodeThread: failed to init decoder");
        return nullptr;
    }
    dec->setKeyFrameRequestFn([]() {
        media::VideoSurfaceRegistry::instance().requestKeyFrame();
    });

    decoder_ = std::move(dec);
    return decoder_.get();
}

void VideoDecodeThread::destroyDecoder() {
    std::lock_guard<std::mutex> lk(decoderMtx_);
    decoder_.reset();
}

void VideoDecodeThread::run() {
    media::thread_affinity::setThreadName("VP9Decode");
    media::thread_affinity::configureCurrentThreadForDecode();

    media::EncodedVideoFrame frame;

    while (running_.load(std::memory_order_acquire)) {
        bool didWork = false;

        if (!media::VideoSurfaceRegistry::instance().hasSurface()) {
            std::lock_guard<std::mutex> lk(decoderMtx_);
            decoder_.reset();
        } else {
            int64_t now = media::nowUs();

            // Anchor render clock on first frame
            if (!renderClock_.isAnchored()) {
                int64_t nextPts;
                if (frameQueue_->peekNextPts(nextPts)) {
                    int64_t adjustedTarget = frameQueue_->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                    adjustedTarget = std::clamp(adjustedTarget,
                                                media::video_config::jitter::kMinBufferUs,
                                                media::video_config::jitter::kMaxBufferUs);
                    renderClock_.anchor(nextPts, now, adjustedTarget);
                    lastAnchorTimeUs_ = now;
                }
            } else if (lastAnchorTimeUs_ > 0 &&
                       (now - lastAnchorTimeUs_) >= media::video_config::kReanchorIntervalUs) {
                renderClock_.reset();
                lastAnchorTimeUs_ = 0;
            }

            // Update buffer target with A/V sync correction
            int64_t adjustedTarget = frameQueue_->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
            adjustedTarget = std::clamp(adjustedTarget,
                                        media::video_config::jitter::kMinBufferUs,
                                        media::video_config::jitter::kMaxBufferUs);
            renderClock_.updateBufferTarget(adjustedTarget);

            // Update jitter metrics
            frameQueue_->metrics().jitterUs.store(frameQueue_->jitterUs(), std::memory_order_relaxed);
            frameQueue_->metrics().bufferTargetUs.store(frameQueue_->bufferTargetUs(), std::memory_order_relaxed);

            // Pop and decode
            auto popResult = frameQueue_->tryPopReadyFrame(frame, renderClock_, now);
            if (popResult != media::PopResult::Nothing) {
                auto* dec = getOrCreateDecoder();
                if (dec) {
                    if (dec->submitFrame(frame.data.data(), frame.data.size(),
                                          frame.ptsUs, frame.isKeyFrame)) {
                        frameQueue_->metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                        frameQueue_->metrics().currentFps.store(
                            fpsCounter_.tick(media::nowUs()), std::memory_order_relaxed);
                        if (dec->decodedWidth() > 0) {
                            frameQueue_->metrics().width.store(dec->decodedWidth(), std::memory_order_relaxed);
                            frameQueue_->metrics().height.store(dec->decodedHeight(), std::memory_order_relaxed);
                            media::VideoSurfaceRegistry::instance().setDecodedResolution(
                                dec->decodedWidth(), dec->decodedHeight());
                        }
                        syncCoordinator_.onVideoRender(frame.ptsUs, media::nowUs());
                    } else {
                        frameQueue_->metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                didWork = true;
            }

            // Release output buffers to render decoded frames
            {
                std::lock_guard<std::mutex> lk(decoderMtx_);
                if (decoder_) {
                    decoder_->releaseOutputBuffers();
                }
            }
        }

        if (!didWork) {
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
        }
    }
}

}  // namespace videomodule
