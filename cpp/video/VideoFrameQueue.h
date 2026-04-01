#pragma once

#include <mutex>
#include <deque>
#include <atomic>
#include <functional>
#include "VideoTypes.h"
#include "VideoConfig.h"
#include "VideoSyncController.h"
#include "common/MediaConfig.h"
#include "common/MediaTime.h"

namespace media {

enum class PopResult : uint8_t { Nothing, Popped, PoppedWithSkip };

// Single-stream video frame queue for broadcast playback.
// Receives encoded VP9 frames from JS thread, provides them to decode thread.
// Thread safety: pushEncodedFrame from JS thread, pop/peek from decode thread.
// All access serialized by internal mutex.
//
// Known limitation: uses std::deque<EncodedVideoFrame> with std::vector<uint8_t>
// per frame, causing heap allocations on the video decode path. Acceptable for
// v1.0 because video decode is not RT-priority. recycleBuf_ mitigates the
// common case by reusing the last popped frame's buffer.
class VideoFrameQueue {
public:
    using WakeCallback = std::function<void()>;
    using ResetCallback = std::function<void()>;

    VideoFrameQueue() {
        recycleBuf_.reserve(video_config::kMaxEncodedFrameSize);
    }

    void setWakeCallback(WakeCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        wakeCallback_ = std::move(cb);
    }

    // Called by the decode thread so resetStream() can reset the render clock
    void setResetCallback(ResetCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        resetCallback_ = std::move(cb);
    }

    void reset() noexcept {
        ResetCallback cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            encodedQueue_.clear();
            needsKeyFrame_ = true;
            lastArrivalUs_ = 0;
            metrics_.reset();
            jitter_.reset();
            cb = resetCallback_;
        }
        if (cb) cb();
    }

    bool pushEncodedFrame(const uint8_t* data, size_t size,
                          int64_t ptsUs, bool isKeyFrame) noexcept {
        if (!data || size == 0) return false;
        if (size > video_config::kMaxEncodedFrameSize) return false;

        std::lock_guard<std::mutex> lk(mtx_);

        if (needsKeyFrame_ && !isKeyFrame) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (isKeyFrame) {
            needsKeyFrame_ = false;
            metrics_.needsKeyFrame.store(false, std::memory_order_relaxed);
        }

        if (encodedQueue_.size() >= video_config::kDecodeQueueDepth) {
            if (catchupPolicy_.load(std::memory_order_relaxed) == CatchupPolicy::DropToLive) {
                skipToLive();
            } else {
                bool droppedKeyFrame = encodedQueue_.front().isKeyFrame;
                encodedQueue_.pop_front();
                metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                // Dropping a keyframe invalidates all subsequent P-frames
                if (droppedKeyFrame) {
                    encodedQueue_.clear();
                    needsKeyFrame_ = true;
                    metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
                }
            }
        }

        int64_t arrivalUs = nowUs();

        // Track inter-frame delivery gap
        if (lastArrivalUs_ > 0) {
            int64_t gapUs = arrivalUs - lastArrivalUs_;
            int64_t curMax = metrics_.maxInterFrameGapUs.load(std::memory_order_relaxed);
            while (gapUs > curMax &&
                   !metrics_.maxInterFrameGapUs.compare_exchange_weak(
                       curMax, gapUs, std::memory_order_relaxed)) {}
            if (gapUs > 500000) {
                metrics_.gapsOver500ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            } else if (gapUs > 100000) {
                metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            } else if (gapUs > 50000) {
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            }
        }
        lastArrivalUs_ = arrivalUs;

        jitter_.onFrame(ptsUs, arrivalUs, isKeyFrame);
        metrics_.driftPpm.store(jitter_.longTermDriftPpm(), std::memory_order_relaxed);

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

        // Wake decode thread for immediate processing (avoid backoff latency)
        if (wakeCallback_) wakeCallback_();
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
    PopResult tryPopReadyFrame(EncodedVideoFrame& out, const VideoRenderClock& clock,
                               int64_t nowUs) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (encodedQueue_.empty()) return PopResult::Nothing;

        int64_t nextPts = encodedQueue_.front().ptsUs;
        if (!clock.isReadyToRender(nextPts, nowUs)) return PopResult::Nothing;

        recycleBuf_ = std::move(out.data);
        out = std::move(encodedQueue_.front());
        encodedQueue_.pop_front();

        // Track late frames
        int64_t scheduledRender = clock.scheduledRenderTime(out.ptsUs);
        if (scheduledRender > 0 && (nowUs - scheduledRender) > video_config::kLateFrameThresholdUs) {
            metrics_.lateFrames.fetch_add(1, std::memory_order_relaxed);
        }

        // Skip stale frames — replace with fresher frame
        bool skipped = false;
        while (!encodedQueue_.empty()) {
            int64_t nextScheduled = clock.scheduledRenderTime(encodedQueue_.front().ptsUs);
            if (nextScheduled <= 0 || (nowUs - nextScheduled) <= video_config::kLateFrameThresholdUs) break;
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
        metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
        encodedQueue_.clear();
    }

    void setBufferTargetOverride(int64_t us) noexcept {
        jitter_.setBufferTargetOverride(us);
    }

    void setNetworkJitterFloorUs(int64_t us) noexcept {
        jitter_.setNetworkJitterFloorUs(us);
    }

    void setCatchupPolicy(CatchupPolicy p) noexcept {
        catchupPolicy_.store(p, std::memory_order_relaxed);
    }

    void setPlaybackRate(float rate) noexcept {
        playbackRate_.store(
            std::clamp(rate, config::playbackrate::kMinRate, config::playbackrate::kMaxRate),
            std::memory_order_relaxed);
    }

    [[nodiscard]] float playbackRate() const noexcept {
        return playbackRate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isAwaitingKeyFrame() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return needsKeyFrame_;
    }

    VideoStreamMetrics& metrics() noexcept { return metrics_; }
    const VideoStreamMetrics& metrics() const noexcept { return metrics_; }

private:
    // Clear queue for DropToLive policy. Remaining non-keyframes would bypass
    // keyframe gating in pop paths, causing video corruption. Caller must hold mtx_.
    void skipToLive() noexcept {
        size_t dropped = encodedQueue_.size();
        encodedQueue_.clear();
        if (dropped > 0) {
            metrics_.framesDropped.fetch_add(dropped, std::memory_order_relaxed);
            needsKeyFrame_ = true;
            metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
        }
    }

    mutable std::mutex mtx_;
    std::deque<EncodedVideoFrame> encodedQueue_;
    bool needsKeyFrame_ = true;
    WakeCallback wakeCallback_;
    ResetCallback resetCallback_;
    int64_t lastArrivalUs_ = 0;
    VideoStreamMetrics metrics_;
    VideoJitterEstimator jitter_;
    std::vector<uint8_t> recycleBuf_;
    std::atomic<CatchupPolicy> catchupPolicy_{CatchupPolicy::PlayThrough};
    std::atomic<float> playbackRate_{config::playbackrate::kDefaultRate};
};

}  // namespace media
