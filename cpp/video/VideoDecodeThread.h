#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include <algorithm>
#include "video/VideoDecoder.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"
#include "video/VideoConfig.h"
#include "video/VideoSyncController.h"
#include "video/FpsCounter.h"
#include "common/AVSyncCoordinator.h"
#include "common/TimedJoin.h"
#include "common/MediaLog.h"
#include "common/MediaTime.h"
#include "common/ThreadAffinity.h"

namespace media {

// Shared single-stream video decode thread for broadcast playback.
// Platform-agnostic: uses VideoDecoder interface for HW decode.
class VideoDecodeThread {
public:
    // Factory creates a platform-specific decoder when a surface is available.
    using DecoderFactory = std::function<std::unique_ptr<VideoDecoder>()>;

    VideoDecodeThread() = default;
    ~VideoDecodeThread() { stop(); }

    VideoDecodeThread(const VideoDecodeThread&) = delete;
    VideoDecodeThread& operator=(const VideoDecodeThread&) = delete;

    bool start(VideoFrameQueue* queue, DecoderFactory factory) {
        if (running_.load(std::memory_order_acquire)) return true;
        if (!queue || !factory) return false;

        frameQueue_ = queue;
        decoderFactory_ = std::move(factory);
        frameQueue_->setWakeCallback([this]() { wake(); });
        frameQueue_->setResetCallback([this]() {
            pendingClockReset_.store(true, std::memory_order_release);
            wake();
        });
        threadExited_.store(false, std::memory_order_relaxed);
        wakeVersion_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&VideoDecodeThread::run, this);
        return true;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        wake();
        if (frameQueue_) {
            frameQueue_->setWakeCallback(nullptr);
            frameQueue_->setResetCallback(nullptr);
        }
        timedJoin(thread_, threadExited_, config::thread::kThreadJoinTimeoutMs, "VideoDecodeThread");

        if (!threadExited_.load(std::memory_order_acquire)) {
            MEDIA_LOG_E("VideoDecodeThread: thread detached, skipping resource destruction");
            return;
        }

        std::lock_guard<std::mutex> lk(decoderMtx_);
        decoder_.reset();
        renderClock_.reset();
        fpsCounter_.reset();
        lastAnchorTimeUs_ = 0;
        consecutiveErrors_ = 0;
        decoderResetTimeUs_ = 0;
        decoderResetBackoffUs_ = kInitialDecoderResetBackoffUs;
        syncCoordinator_.reset();
        pendingClockReset_.store(false, std::memory_order_relaxed);
        frameQueue_ = nullptr;
        decoderFactory_ = nullptr;
    }

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }

    void wake() noexcept {
        wakeVersion_.fetch_add(1, std::memory_order_release);
        wakeVersion_.notify_one();
    }

private:
    // Caller must hold decoderMtx_
    VideoDecoder* getOrCreateDecoderLocked() {
        if (decoder_) return decoder_.get();

        auto& metrics = frameQueue_->metrics();

        // Backoff after error-driven decoder reset to prevent HAL thrashing
        if (decoderResetTimeUs_ > 0) {
            int64_t elapsed = nowUs() - decoderResetTimeUs_;
            if (elapsed < decoderResetBackoffUs_) {
                metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::BackingOff), std::memory_order_relaxed);
                return nullptr;
            }
            decoderResetTimeUs_ = 0;
        }

        if (!VideoSurfaceRegistry::instance().hasSurface()) {
            metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::WaitingSurface), std::memory_order_relaxed);
            return nullptr;
        }
        if (!decoderFactory_) return nullptr;

        auto dec = decoderFactory_();
        if (!dec) {
            metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::Failed), std::memory_order_relaxed);
            return nullptr;
        }

        decoder_ = std::move(dec);
        metrics.decoderState.store(static_cast<uint8_t>(VideoDecoderState::Active), std::memory_order_relaxed);
        return decoder_.get();
    }

    void run() {
        thread_affinity::setThreadName("VP9Decode");
        thread_affinity::configureCurrentThreadForDecode();

        EncodedVideoFrame frame;
        int consecutiveIdleCycles = 0;
        uint32_t lastWakeVersion = 0;

        while (running_.load(std::memory_order_acquire)) {
            bool didWork = false;

            if (pendingClockReset_.exchange(false, std::memory_order_acquire)) {
                renderClock_.reset();
                lastAnchorTimeUs_ = 0;
                syncCoordinator_.reset();
            }

            if (!VideoSurfaceRegistry::instance().hasSurface()) {
                std::lock_guard<std::mutex> lk(decoderMtx_);
                decoder_.reset();
                renderClock_.reset();
                lastAnchorTimeUs_ = 0;
                frameQueue_->requestKeyFrame();
            } else {
                int64_t now = nowUs();

                // Anchor render clock on first frame
                if (!renderClock_.isAnchored()) {
                    int64_t nextPts;
                    if (frameQueue_->peekNextPts(nextPts)) {
                        int64_t adjustedTarget = frameQueue_->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                        adjustedTarget = std::clamp(adjustedTarget,
                                                    video_config::jitter::kMinBufferUs,
                                                    video_config::jitter::kMaxBufferUs);
                        renderClock_.anchor(nextPts, now, adjustedTarget);
                        lastAnchorTimeUs_ = now;
                    }
                } else if (lastAnchorTimeUs_ > 0 &&
                           (now - lastAnchorTimeUs_) >= video_config::kReanchorIntervalUs) {
                    renderClock_.reset();
                    lastAnchorTimeUs_ = 0;
                }

                // Update buffer target with A/V sync correction
                int64_t adjustedTarget = frameQueue_->bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                adjustedTarget = std::clamp(adjustedTarget,
                                            video_config::jitter::kMinBufferUs,
                                            video_config::jitter::kMaxBufferUs);
                renderClock_.updateBufferTarget(adjustedTarget);
                renderClock_.setPlaybackRate(frameQueue_->playbackRate());

                // Update jitter metrics
                frameQueue_->metrics().jitterUs.store(frameQueue_->jitterUs(), std::memory_order_relaxed);
                frameQueue_->metrics().bufferTargetUs.store(frameQueue_->bufferTargetUs(), std::memory_order_relaxed);

                // Pop and decode
                auto popResult = frameQueue_->tryPopReadyFrame(frame, renderClock_, now);
                if (popResult != PopResult::Nothing) {
                    // Hold lock for entire submit+postDecode cycle so the
                    // decoder pointer stays valid through both operations
                    std::lock_guard<std::mutex> lk(decoderMtx_);
                    auto* dec = getOrCreateDecoderLocked();
                    if (dec) {
                        auto decodeStart = nowUs();
                        if (dec->submitFrame(frame.data.data(), frame.data.size(),
                                              frame.ptsUs, frame.isKeyFrame)) {
                            auto decodeDuration = nowUs() - decodeStart;
                            auto prev = frameQueue_->metrics().decodeLatencyUs.load(std::memory_order_relaxed);
                            auto smoothed = prev == 0 ? decodeDuration : prev + (decodeDuration - prev) / 8;
                            frameQueue_->metrics().decodeLatencyUs.store(smoothed, std::memory_order_relaxed);
                            frameQueue_->metrics().lastDecodeTimeUs.store(nowUs(), std::memory_order_relaxed);
                            consecutiveErrors_ = 0;
                            decoderResetBackoffUs_ = kInitialDecoderResetBackoffUs;
                            frameQueue_->metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                            frameQueue_->metrics().currentFps.store(
                                fpsCounter_.tick(nowUs()), std::memory_order_relaxed);
                            if (dec->decodedWidth() > 0) {
                                frameQueue_->metrics().width.store(dec->decodedWidth(), std::memory_order_relaxed);
                                frameQueue_->metrics().height.store(dec->decodedHeight(), std::memory_order_relaxed);
                                VideoSurfaceRegistry::instance().setDecodedResolution(
                                    dec->decodedWidth(), dec->decodedHeight());
                            }
                            syncCoordinator_.onVideoRender(frame.ptsUs, nowUs());
                        } else {
                            frameQueue_->metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                            frameQueue_->metrics().lastDecodeError.store(dec->lastError(), std::memory_order_relaxed);
                            if (++consecutiveErrors_ >= kMaxConsecutiveErrors) {
                                MEDIA_LOG_E("VideoDecodeThread: %d consecutive errors, resetting decoder (backoff=%lldms)",
                                            consecutiveErrors_, static_cast<long long>(decoderResetBackoffUs_ / 1000));
                                decoder_.reset();
                                renderClock_.reset();
                                lastAnchorTimeUs_ = 0;
                                decoderResetTimeUs_ = nowUs();
                                decoderResetBackoffUs_ = std::min(decoderResetBackoffUs_ * 2, kMaxDecoderResetBackoffUs);
                                frameQueue_->requestKeyFrame();
                                frameQueue_->metrics().decoderResets.fetch_add(1, std::memory_order_relaxed);
                                consecutiveErrors_ = 0;
                            }
                        }
                        // Release output buffers while still holding lock
                        if (decoder_) decoder_->postDecode();
                    }
                    didWork = true;
                }
            }

            if (!didWork) {
                consecutiveIdleCycles++;
                int64_t sleepUs;
                if (consecutiveIdleCycles < 4) {
                    sleepUs = video_config::kDecodeIdleSleepUs;
                } else {
                    // Cap at shift 3 (max 16ms) to maintain 2:1 margin over 30fps frame interval
                    int backoffShift = std::min(consecutiveIdleCycles - 4, 3);
                    sleepUs = video_config::kDecodeIdleSleepUs << backoffShift;
                }
                waitForWakeOrTimeout(lastWakeVersion, sleepUs);
                lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
            } else {
                consecutiveIdleCycles = 0;
                lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
            }
        }

        threadExited_.store(true, std::memory_order_release);
    }

    void waitForWakeOrTimeout(uint32_t lastVersion, int64_t timeoutUs) noexcept {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(timeoutUs);
        while (std::chrono::steady_clock::now() < deadline) {
            uint32_t current = wakeVersion_.load(std::memory_order_acquire);
            if (current != lastVersion || !running_.load(std::memory_order_relaxed)) return;
            wakeVersion_.wait(current, std::memory_order_acquire);
        }
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> threadExited_{false};
    std::atomic<uint32_t> wakeVersion_{0};
    VideoFrameQueue* frameQueue_ = nullptr;
    DecoderFactory decoderFactory_;

    std::unique_ptr<VideoDecoder> decoder_;
    std::mutex decoderMtx_;

    VideoRenderClock renderClock_;
    FpsCounter fpsCounter_;
    int64_t lastAnchorTimeUs_ = 0;
    AVSyncCoordinator syncCoordinator_;

    std::atomic<bool> pendingClockReset_{false};

    static constexpr int kMaxConsecutiveErrors = 5;
    static constexpr int64_t kInitialDecoderResetBackoffUs = 200'000;   // 200ms
    static constexpr int64_t kMaxDecoderResetBackoffUs = 2'000'000;     // 2s
    int consecutiveErrors_ = 0;
    int64_t decoderResetTimeUs_ = 0;
    int64_t decoderResetBackoffUs_ = kInitialDecoderResetBackoffUs;
};

}  // namespace media
