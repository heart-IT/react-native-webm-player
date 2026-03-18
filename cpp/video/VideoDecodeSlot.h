#pragma once

#include <mutex>
#include <deque>
#include <atomic>
#include "VideoTypes.h"
#include "VideoConfig.h"
#include "VideoSyncController.h"
#include "common/MediaTime.h"

namespace media {

enum class PopResult : uint8_t { Nothing, Popped, PoppedWithSkip };

class VideoDecodeSlot {
public:
    void bind(int participantId) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        boundParticipant_.store(participantId, std::memory_order_release);
        encodedQueue_.clear();
        needsKeyFrame_ = true;
        metrics_.reset();
        jitter_.reset();
    }

    void unbind() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        boundParticipant_.store(-1, std::memory_order_release);
        encodedQueue_.clear();
        needsKeyFrame_ = true;
    }

    bool isBound() const noexcept {
        return boundParticipant_.load(std::memory_order_acquire) >= 0;
    }

    int boundParticipant() const noexcept {
        return boundParticipant_.load(std::memory_order_acquire);
    }

    bool pushEncodedFrame(const uint8_t* data, size_t size,
                          int64_t ptsUs, bool isKeyFrame) noexcept {
        if (!data || size == 0) return false;
        if (size > video_config::kMaxEncodedFrameSize) return false;

        std::lock_guard<std::mutex> lk(mtx_);
        if (boundParticipant_.load(std::memory_order_relaxed) < 0) return false;

        if (needsKeyFrame_ && !isKeyFrame) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (isKeyFrame) needsKeyFrame_ = false;

        if (encodedQueue_.size() >= video_config::kDecodeQueueDepth) {
            encodedQueue_.pop_front();
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
        }

        int64_t arrivalUs = nowUs();
        jitter_.onFrame(ptsUs, arrivalUs, isKeyFrame);

        EncodedVideoFrame frame;
        if (!recycleBuf_.empty()) {
            frame.data = std::move(recycleBuf_);
            frame.data.clear();
        }
        frame.data.assign(data, data + size);
        frame.ptsUs = ptsUs;
        frame.isKeyFrame = isKeyFrame;
        encodedQueue_.push_back(std::move(frame));

        metrics_.framesReceived.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool popEncodedFrame(EncodedVideoFrame& out) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (encodedQueue_.empty()) return false;
        recycleBuf_ = std::move(out.data);
        out = std::move(encodedQueue_.front());
        encodedQueue_.pop_front();
        return true;
    }

    bool peekNextPts(int64_t& ptsUs) const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (encodedQueue_.empty()) return false;
        ptsUs = encodedQueue_.front().ptsUs;
        return true;
    }

    // Combined peek+readiness check+pop in a single lock acquisition.
    // Returns PopResult::Popped or PoppedWithSkip (late frame dropped) if a frame was popped.
    // Also writes the popped frame's PTS into ptsUsOut.
    PopResult tryPopReadyFrame(EncodedVideoFrame& out, const VideoRenderClock& clock,
                               int64_t nowUs) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (encodedQueue_.empty()) return PopResult::Nothing;

        int64_t nextPts = encodedQueue_.front().ptsUs;
        if (!clock.isReadyToRender(nextPts, nowUs)) return PopResult::Nothing;

        recycleBuf_ = std::move(out.data);
        out = std::move(encodedQueue_.front());
        encodedQueue_.pop_front();

        // Track late frames: frame was ready but arrived past its scheduled render time
        int64_t scheduledRender = clock.scheduledRenderTime(out.ptsUs);
        if (scheduledRender > 0 && (nowUs - scheduledRender) > video_config::kLateFrameThresholdUs) {
            metrics_.lateFrames.fetch_add(1, std::memory_order_relaxed);
        }

        // Skip ALL late frames in one call (not just one)
        bool skipped = false;
        while (!encodedQueue_.empty() &&
               clock.isReadyToRender(encodedQueue_.front().ptsUs, nowUs)) {
            metrics_.skippedFrames.fetch_add(1, std::memory_order_relaxed);
            recycleBuf_ = std::move(out.data);
            out = std::move(encodedQueue_.front());
            encodedQueue_.pop_front();
            skipped = true;
        }

        return skipped ? PopResult::PoppedWithSkip : PopResult::Popped;
    }

    [[nodiscard]] int64_t bufferTargetUs() const noexcept {
        return jitter_.bufferTargetUs();
    }

    [[nodiscard]] int64_t jitterUs() const noexcept {
        return jitter_.jitterUs();
    }

    size_t pendingFrames() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return encodedQueue_.size();
    }

    void requestKeyFrame() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        needsKeyFrame_ = true;
        encodedQueue_.clear();
    }

    bool needsKeyFrame() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return needsKeyFrame_;
    }

    VideoStreamMetrics& metrics() noexcept { return metrics_; }
    const VideoStreamMetrics& metrics() const noexcept { return metrics_; }

private:
    mutable std::mutex mtx_;
    std::atomic<int> boundParticipant_{-1};
    std::deque<EncodedVideoFrame> encodedQueue_;
    bool needsKeyFrame_ = true;
    VideoStreamMetrics metrics_;
    VideoJitterEstimator jitter_;
    std::vector<uint8_t> recycleBuf_;
};

}  // namespace media
