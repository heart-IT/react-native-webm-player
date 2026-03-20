#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"
#include "video/VideoConfig.h"
#include "video/VideoSyncController.h"
#include "video/FpsCounter.h"
#include "common/AVSyncCoordinator.h"
#include "common/TimedJoin.h"
#include "MediaCodecDecoder.h"

namespace videomodule {

// Single-stream video decode thread for broadcast playback.
class VideoDecodeThread {
public:
    VideoDecodeThread() = default;
    ~VideoDecodeThread() { stop(); }

    VideoDecodeThread(const VideoDecodeThread&) = delete;
    VideoDecodeThread& operator=(const VideoDecodeThread&) = delete;

    bool start(media::VideoFrameQueue* queue);
    void stop();
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    media::AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }

    void resetRenderClock() noexcept {
        renderClock_.reset();
        lastAnchorTimeUs_ = 0;
    }

    void wake() noexcept {
        wakeVersion_.fetch_add(1, std::memory_order_release);
        wakeVersion_.notify_one();
    }

    void destroyDecoder();

private:
    void run();
    MediaCodecDecoder* getOrCreateDecoder();

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
    media::VideoFrameQueue* frameQueue_ = nullptr;

    std::unique_ptr<MediaCodecDecoder> decoder_;
    std::mutex decoderMtx_;

    media::VideoRenderClock renderClock_;
    media::FpsCounter fpsCounter_;
    int64_t lastAnchorTimeUs_ = 0;
    media::AVSyncCoordinator syncCoordinator_;

    // Reset flag: set by JS thread via reset callback, serviced by decode thread
    std::atomic<bool> pendingClockReset_{false};

    // Consecutive error tracking for automatic decoder recovery
    static constexpr int kMaxConsecutiveErrors = 5;
    int consecutiveErrors_ = 0;

    // Detach safety: if timedJoin detaches the thread, skip resource destruction
    std::atomic<bool> wasDetached_{false};
};

}  // namespace videomodule
