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

    void destroyDecoder();

private:
    void run();
    MediaCodecDecoder* getOrCreateDecoder();

    std::thread thread_;
    std::atomic<bool> running_{false};
    media::VideoFrameQueue* frameQueue_ = nullptr;

    std::unique_ptr<MediaCodecDecoder> decoder_;
    std::mutex decoderMtx_;

    media::VideoRenderClock renderClock_;
    media::FpsCounter fpsCounter_;
    int64_t lastAnchorTimeUs_ = 0;
    media::AVSyncCoordinator syncCoordinator_;
};

}  // namespace videomodule
