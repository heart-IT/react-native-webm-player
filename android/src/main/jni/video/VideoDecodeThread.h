#pragma once

#include <thread>
#include <atomic>
#include <array>
#include <mutex>
#include <unordered_map>
#include "video/VideoDecodeSlot.h"
#include "video/VideoSurfaceRegistry.h"
#include "video/VideoConfig.h"
#include "video/VideoSyncController.h"
#include "video/FpsCounter.h"
#include "common/AVSyncCoordinator.h"
#include "MediaCodecDecoder.h"

namespace videomodule {

class VideoDecodeThread {
public:
    VideoDecodeThread() = default;
    ~VideoDecodeThread() { stop(); }

    VideoDecodeThread(const VideoDecodeThread&) = delete;
    VideoDecodeThread& operator=(const VideoDecodeThread&) = delete;

    bool start(std::array<media::VideoDecodeSlot, media::video_config::kMaxVideoSlots>* slots);
    void stop();
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    media::AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }

    void resetRenderClock(int slotIdx) noexcept {
        if (slotIdx >= 0 && slotIdx < media::video_config::kMaxVideoSlots) {
            renderClocks_[static_cast<size_t>(slotIdx)].reset();
            lastAnchorTimeUs_[static_cast<size_t>(slotIdx)] = 0;
        }
    }

    void destroyDecoder(int slotIdx);

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::array<media::VideoDecodeSlot, media::video_config::kMaxVideoSlots>* slots_ = nullptr;

    // Per-slot decoder instances
    std::array<std::unique_ptr<MediaCodecDecoder>, media::video_config::kMaxVideoSlots> decoders_;
    std::mutex decoderMtx_;

    MediaCodecDecoder* getOrCreateDecoder(int slotIdx, int participantId);

    std::array<media::VideoRenderClock, media::video_config::kMaxVideoSlots> renderClocks_;
    std::array<media::FpsCounter, media::video_config::kMaxVideoSlots> fpsCounters_;
    std::array<int64_t, media::video_config::kMaxVideoSlots> lastAnchorTimeUs_{};
    media::AVSyncCoordinator syncCoordinator_;
};

}  // namespace videomodule
