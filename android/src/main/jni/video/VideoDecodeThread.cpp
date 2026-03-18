#include "VideoDecodeThread.h"
#include "common/MediaLog.h"
#include "common/MediaTime.h"
#include "common/ThreadAffinity.h"
#include <android/native_window_jni.h>

namespace videomodule {

bool VideoDecodeThread::start(
    std::array<media::VideoDecodeSlot, media::video_config::kMaxVideoSlots>* slots) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (!slots) return false;

    slots_ = slots;
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
    for (auto& decoder : decoders_) {
        decoder.reset();
    }
    for (auto& clock : renderClocks_) {
        clock.reset();
    }
    for (auto& fps : fpsCounters_) {
        fps.reset();
    }
    lastAnchorTimeUs_.fill(0);
    syncCoordinator_.reset();
    slots_ = nullptr;
}

MediaCodecDecoder* VideoDecodeThread::getOrCreateDecoder(int slotIdx, int participantId) {
    std::lock_guard<std::mutex> lk(decoderMtx_);
    auto idx = static_cast<size_t>(slotIdx);

    if (decoders_[idx]) return decoders_[idx].get();

    auto* surface = static_cast<ANativeWindow*>(
        media::VideoSurfaceRegistry::instance().acquireSurface(participantId));
    if (!surface) return nullptr;

    auto decoder = std::make_unique<MediaCodecDecoder>();
    if (!decoder->initialize(surface)) {
        ANativeWindow_release(surface);
        MEDIA_LOG_E("VideoDecodeThread: failed to init decoder for slot %d", slotIdx);
        return nullptr;
    }

    decoders_[idx] = std::move(decoder);
    return decoders_[idx].get();
}

void VideoDecodeThread::destroyDecoder(int slotIdx) {
    std::lock_guard<std::mutex> lk(decoderMtx_);
    decoders_[static_cast<size_t>(slotIdx)].reset();
}

void VideoDecodeThread::run() {
    media::thread_affinity::configureCurrentThreadForDecode();

    media::EncodedVideoFrame frame;

    while (running_.load(std::memory_order_acquire)) {
        bool didWork = false;

        for (size_t i = 0; i < media::video_config::kMaxVideoSlots; ++i) {
            auto& slot = (*slots_)[i];
            if (!slot.isBound()) {
                {
                    std::lock_guard<std::mutex> lk(decoderMtx_);
                    if (decoders_[i]) decoders_[i].reset();
                }
                continue;
            }

            int participantId = slot.boundParticipant();

            if (!media::VideoSurfaceRegistry::instance().hasSurface(participantId)) {
                std::lock_guard<std::mutex> lk(decoderMtx_);
                if (decoders_[i]) decoders_[i].reset();
                continue;
            }

            auto& clock = renderClocks_[i];
            int64_t now = media::nowUs();

            // Anchor render clock on first frame
            if (!clock.isAnchored()) {
                int64_t nextPts;
                if (slot.peekNextPts(nextPts)) {
                    int64_t adjustedTarget = slot.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                    adjustedTarget = std::clamp(adjustedTarget,
                                                media::video_config::jitter::kMinBufferUs,
                                                media::video_config::jitter::kMaxBufferUs);
                    clock.anchor(nextPts, now, adjustedTarget);
                    lastAnchorTimeUs_[i] = now;
                }
            } else if (lastAnchorTimeUs_[i] > 0 &&
                       (now - lastAnchorTimeUs_[i]) >= media::video_config::kReanchorIntervalUs) {
                // Periodic re-anchor to compensate for clock drift
                clock.reset();
                lastAnchorTimeUs_[i] = 0;
            }

            // Update buffer target with A/V sync correction
            int64_t adjustedTarget = slot.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
            adjustedTarget = std::clamp(adjustedTarget,
                                        media::video_config::jitter::kMinBufferUs,
                                        media::video_config::jitter::kMaxBufferUs);
            clock.updateBufferTarget(adjustedTarget);

            // Update per-slot jitter metrics
            slot.metrics().jitterUs.store(slot.jitterUs(), std::memory_order_relaxed);
            slot.metrics().bufferTargetUs.store(slot.bufferTargetUs(), std::memory_order_relaxed);

            // Single-lock pop with readiness check and late-frame skip
            auto popResult = slot.tryPopReadyFrame(frame, clock, now);
            if (popResult != media::PopResult::Nothing) {
                auto* decoder = getOrCreateDecoder(static_cast<int>(i), participantId);
                if (decoder) {
                    if (decoder->submitFrame(frame.data.data(), frame.data.size(),
                                              frame.ptsUs, frame.isKeyFrame)) {
                        slot.metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                        slot.metrics().currentFps.store(
                            fpsCounters_[i].tick(media::nowUs()), std::memory_order_relaxed);
                        if (decoder->decodedWidth() > 0) {
                            slot.metrics().width.store(decoder->decodedWidth(), std::memory_order_relaxed);
                            slot.metrics().height.store(decoder->decodedHeight(), std::memory_order_relaxed);
                            media::VideoSurfaceRegistry::instance().setDecodedResolution(
                                participantId, decoder->decodedWidth(), decoder->decodedHeight());
                        }
                        syncCoordinator_.onVideoRender(frame.ptsUs, media::nowUs());
                    } else {
                        slot.metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                didWork = true;
            }

            // Release output buffers to render decoded frames
            {
                std::lock_guard<std::mutex> lk(decoderMtx_);
                if (decoders_[i]) {
                    decoders_[i]->releaseOutputBuffers();
                }
            }
        }

        if (!didWork) {
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
        }
    }
}

}  // namespace videomodule
