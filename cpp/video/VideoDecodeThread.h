// Single-stream video decode thread for broadcast playback.
// Delegates lifecycle to WorkerThread<T>.
#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include <algorithm>
#include <array>
#include <vector>
#include "video/VideoDecoder.h"
#include "video/VideoFrameQueue.h"
#include "common/IngestRingBuffer.h"
#include "video/VideoSurfaceRegistry.h"
#include "video/VideoConfig.h"
#include "video/VideoSyncController.h"
#include "video/FpsCounter.h"
#include "common/AVSyncCoordinator.h"
#include "common/WorkerThread.h"
#include "common/MediaLog.h"
#include "common/MediaTime.h"

namespace media {

// Processor: performs one video decode cycle.
class VideoDecodeProcessor {
public:
    using DecoderFactory = std::function<std::unique_ptr<VideoDecoder>()>;

    // Heap-allocated state that can be leaked on detach to prevent UAF.
    struct DecoderState {
        std::mutex mtx;
        std::unique_ptr<VideoDecoder> decoder;
        DecoderFactory decoderFactory;
        VideoRenderClock renderClock;
        FpsCounter fpsCounter;
        int64_t lastAnchorTimeUs = 0;
        int consecutiveErrors = 0;
        int64_t decoderResetTimeUs = 0;
        int64_t decoderResetBackoffUs = kInitialDecoderResetBackoffUs;
        bool surfaceLostKeyFrameRequested = false;
    };

    VideoDecodeProcessor()
        : state_(std::make_unique<DecoderState>()) {}

    void configure(VideoFrameQueue* queue, DecoderFactory factory) {
        frameQueue_.store(queue, std::memory_order_relaxed);
        state_->decoderFactory = std::move(factory);
    }

    [[nodiscard]] bool process() noexcept {
        auto* fq = frameQueue_.load(std::memory_order_acquire);
        if (!fq) return false;

        auto* s = state_.get();
        bool didWork = false;

        if (pendingClockReset_.exchange(false, std::memory_order_acquire)) {
            s->renderClock.reset();
            s->lastAnchorTimeUs = 0;
            syncCoordinator_.reset();
        }

        if (!VideoSurfaceRegistry::instance().hasSurface()) {
            std::lock_guard<std::mutex> lk(s->mtx);
            s->decoder.reset();
            s->renderClock.reset();
            s->lastAnchorTimeUs = 0;
            if (!s->surfaceLostKeyFrameRequested) {
                fq->requestKeyFrame();
                s->surfaceLostKeyFrameRequested = true;
            }
        } else {
            s->surfaceLostKeyFrameRequested = false;
            int64_t now = nowUs();

            // Anchor render clock on first frame
            if (!s->renderClock.isAnchored()) {
                int64_t nextPts;
                if (fq->peekNextPts(nextPts)) {
                    int64_t adjustedTarget = fq->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                    adjustedTarget = std::clamp(adjustedTarget,
                                                video_config::jitter::kMinBufferUs,
                                                video_config::jitter::kMaxBufferUs);
                    s->renderClock.anchor(nextPts, now, adjustedTarget);
                    s->lastAnchorTimeUs = now;
                }
            } else if (s->lastAnchorTimeUs > 0 &&
                       (now - s->lastAnchorTimeUs) >= video_config::kReanchorIntervalUs) {
                s->renderClock.reset();
                s->lastAnchorTimeUs = 0;
            }

            // Update buffer target with A/V sync correction
            int64_t adjustedTarget = fq->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
            adjustedTarget = std::clamp(adjustedTarget,
                                        video_config::jitter::kMinBufferUs,
                                        video_config::jitter::kMaxBufferUs);
            s->renderClock.updateBufferTarget(adjustedTarget);
            s->renderClock.setPlaybackRate(fq->playbackRate());

            fq->metrics().jitterUs.store(fq->jitterUs(), std::memory_order_relaxed);
            fq->metrics().bufferTargetUs.store(fq->bufferTargetUs(), std::memory_order_relaxed);

            // Reset render clock after extended gaps to prevent frame burst
            if (lastFramePopTimeUs_ > 0 && s->renderClock.isAnchored()) {
                int64_t gapUs = now - lastFramePopTimeUs_;
                if (gapUs > video_config::kGapReanchorThresholdUs) {
                    s->renderClock.reset();
                    s->lastAnchorTimeUs = 0;
                }
            }

            // Pop and decode
            EncodedVideoFrame frame;
            auto popResult = fq->tryPopReadyFrame(frame, s->renderClock, now);
            if (popResult != PopResult::Nothing) {
                auto* ring = fq->ringBuffer();
                const uint8_t* frameData = nullptr;
                if (ring) {
                    frameData = ring->dataAt(frame.absOffset, static_cast<long long>(frame.size));
                    if (!frameData) {
                        // Oversized frames are rejected upstream by WebmDemuxer's
                        // kMaxVideoFrameSize guard so the std::array bound holds.
                        if (frame.size <= videoLinearBuf_.size() &&
                            ring->readAt(frame.absOffset, static_cast<long>(frame.size), videoLinearBuf_.data()) == 0) {
                            frameData = videoLinearBuf_.data();
                        }
                    }
                }
                if (!frameData) {
                    fq->metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::lock_guard<std::mutex> lk(s->mtx);
                    auto* dec = getOrCreateDecoderLocked(s, fq);
                    if (dec) {
                        auto decodeStart = nowUs();
                        if (dec->submitFrame(frameData, frame.size,
                                              frame.ptsUs, frame.isKeyFrame)) {
                            auto decodeDuration = nowUs() - decodeStart;
                            auto prev = fq->metrics().decodeLatencyUs.load(std::memory_order_relaxed);
                            auto smoothed = prev == 0 ? decodeDuration : prev + (decodeDuration - prev) / 8;
                            fq->metrics().decodeLatencyUs.store(smoothed, std::memory_order_relaxed);
                            fq->metrics().lastDecodeTimeUs.store(nowUs(), std::memory_order_relaxed);
                            s->consecutiveErrors = 0;
                            s->decoderResetBackoffUs = kInitialDecoderResetBackoffUs;
                            fq->metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                            fq->metrics().currentFps.store(
                                s->fpsCounter.tick(nowUs()), std::memory_order_relaxed);
                            if (dec->decodedWidth() > 0) {
                                fq->metrics().width.store(dec->decodedWidth(), std::memory_order_relaxed);
                                fq->metrics().height.store(dec->decodedHeight(), std::memory_order_relaxed);
                                VideoSurfaceRegistry::instance().setDecodedResolution(
                                    dec->decodedWidth(), dec->decodedHeight());
                            }
                            fq->onFrameDecoded(frame.absOffset);
                            syncCoordinator_.onVideoRender(frame.ptsUs, nowUs(),
                                                           fq->playbackRate());
                        } else {
                            fq->metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                            fq->metrics().lastDecodeError.store(dec->lastError(), std::memory_order_relaxed);
                            if (++s->consecutiveErrors >= kMaxConsecutiveErrors) {
                                MEDIA_LOG_E("VideoDecodeThread: %d consecutive errors, resetting decoder (backoff=%lldms)",
                                            s->consecutiveErrors, static_cast<long long>(s->decoderResetBackoffUs / 1000));
                                s->decoder.reset();
                                s->renderClock.reset();
                                s->lastAnchorTimeUs = 0;
                                s->decoderResetTimeUs = nowUs();
                                s->decoderResetBackoffUs = std::min(s->decoderResetBackoffUs * 2, kMaxDecoderResetBackoffUs);
                                fq->requestKeyFrame();
                                fq->metrics().decoderResets.fetch_add(1, std::memory_order_relaxed);
                                s->consecutiveErrors = 0;
                            }
                        }
                        if (s->decoder) s->decoder->postDecode();
                    }
                }
                lastFramePopTimeUs_ = nowUs();
                didWork = true;
            }
        }

        // Update video-specific heartbeat metrics
        auto heartbeatNow = nowUs();
        fq->metrics().decodeThreadResponsive.store(true, std::memory_order_relaxed);
        fq->metrics().lastHeartbeatUs.store(heartbeatNow, std::memory_order_relaxed);

        // Decay stale smoothed offset so a transient spike does not lock the
        // buffer target when video has stopped rendering (fixes P1-7).
        syncCoordinator_.tickDecay(heartbeatNow);

        return didWork;
    }

    void onDetached() noexcept {
        frameQueue_.store(nullptr, std::memory_order_release);
        // Leak decoder state — the detached thread may still be mid-iteration.
        (void)state_.release();
        state_ = std::make_unique<DecoderState>();
        MEDIA_LOG_E("VideoDecodeProcessor: decoder state leaked to prevent UAF");
    }

    void onStopped() noexcept {
        auto* fq = frameQueue_.load(std::memory_order_acquire);
        if (fq) {
            fq->setWakeCallback(nullptr);
            fq->setResetCallback(nullptr);
        }
        std::lock_guard<std::mutex> lk(state_->mtx);
        state_->decoder.reset();
        state_->renderClock.reset();
        state_->fpsCounter.reset();
        state_->lastAnchorTimeUs = 0;
        state_->consecutiveErrors = 0;
        state_->decoderResetTimeUs = 0;
        state_->decoderResetBackoffUs = kInitialDecoderResetBackoffUs;
        state_->surfaceLostKeyFrameRequested = false;
        syncCoordinator_.reset();
        pendingClockReset_.store(false, std::memory_order_relaxed);
        frameQueue_.store(nullptr, std::memory_order_relaxed);
        state_->decoderFactory = nullptr;
    }

    AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }

    // Request the decode thread drop its render-clock anchor on its next cycle.
    // Used on pause→resume so the pause duration is not mis-accounted as elapsed
    // wall-clock (fixes P1-6 frame burst on resume).
    void requestClockReset() noexcept {
        pendingClockReset_.store(true, std::memory_order_release);
    }

    void setWakeCallback(VideoFrameQueue* queue, std::function<void()> wakeFn) {
        queue->setWakeCallback(std::move(wakeFn));
        queue->setResetCallback([this]() {
            pendingClockReset_.store(true, std::memory_order_release);
        });
    }

private:
    VideoDecoder* getOrCreateDecoderLocked(DecoderState* s, VideoFrameQueue* fq) {
        if (s->decoder) return s->decoder.get();

        auto& metrics = fq->metrics();

        if (s->decoderResetTimeUs > 0) {
            int64_t elapsed = nowUs() - s->decoderResetTimeUs;
            if (elapsed < s->decoderResetBackoffUs) {
                metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::BackingOff), std::memory_order_relaxed);
                return nullptr;
            }
            s->decoderResetTimeUs = 0;
        }

        if (!VideoSurfaceRegistry::instance().hasSurface()) {
            metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::WaitingSurface), std::memory_order_relaxed);
            return nullptr;
        }
        if (!s->decoderFactory) return nullptr;

        auto dec = s->decoderFactory();
        if (!dec) {
            metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::Failed), std::memory_order_relaxed);
            metrics.decodeErrors.fetch_add(1, std::memory_order_relaxed);
            fq->requestKeyFrame();
            return nullptr;
        }

        s->decoder = std::move(dec);
        metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::Active), std::memory_order_relaxed);
        return s->decoder.get();
    }

    std::unique_ptr<DecoderState> state_;
    AVSyncCoordinator syncCoordinator_;
    std::atomic<VideoFrameQueue*> frameQueue_{nullptr};
    std::atomic<bool> pendingClockReset_{false};
    // Fixed-size wrap-around fallback buffer. Guaranteed zero allocation on
    // the decode thread; upstream rejects any frame larger than this cap.
    std::array<uint8_t, video_config::kMaxEncodedFrameSize> videoLinearBuf_{};
    int64_t lastFramePopTimeUs_{0};

    static constexpr int kMaxConsecutiveErrors = media::video_config::decoder_reset::kMaxConsecutiveErrors;
    static constexpr int64_t kInitialDecoderResetBackoffUs = media::video_config::decoder_reset::kInitialBackoffUs;
    static constexpr int64_t kMaxDecoderResetBackoffUs = media::video_config::decoder_reset::kMaxBackoffUs;
};

// Facade: preserves the public API used by VideoPipelineModule.
class VideoDecodeThread {
public:
    using DecoderFactory = VideoDecodeProcessor::DecoderFactory;

    VideoDecodeThread()
        : worker_(processor_, {"VP9Decode",
                               video_config::kDecodeIdleSleepUs,
                               3, true, false}) {}

    ~VideoDecodeThread() { stop(); }

    VideoDecodeThread(const VideoDecodeThread&) = delete;
    VideoDecodeThread& operator=(const VideoDecodeThread&) = delete;

    [[nodiscard]] bool start(VideoFrameQueue* queue, DecoderFactory factory) {
        if (worker_.isRunning()) return true;
        if (!queue || !factory) return false;

        processor_.configure(queue, std::move(factory));
        processor_.setWakeCallback(queue, [this]() { worker_.wake(); });

        return worker_.start();
    }

    void stop() {
        if (!worker_.isRunning()) return;
        worker_.stop();
        if (!worker_.wasDetached()) {
            processor_.onStopped();
        }
        worker_.resetWatchdogState();
    }

    bool isRunning() const noexcept { return worker_.isRunning(); }
    void wake() noexcept { worker_.wake(); }

    [[nodiscard]] bool isResponsive() noexcept { return worker_.isResponsive(); }
    [[nodiscard]] bool wasWatchdogTripped() const noexcept { return worker_.wasWatchdogTripped(); }
    [[nodiscard]] uint32_t watchdogTripCount() const noexcept { return worker_.watchdogTripCount(); }
    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept { return worker_.timeSinceLastHeartbeatUs(); }

    AVSyncCoordinator& syncCoordinator() noexcept { return processor_.syncCoordinator(); }

    void requestClockReset() noexcept { processor_.requestClockReset(); }

private:
    VideoDecodeProcessor processor_;
    WorkerThread<VideoDecodeProcessor> worker_;
};

}  // namespace media
