#pragma once

#include <mutex>
#include <array>
#include <atomic>
#include <functional>
#include "VideoTypes.h"
#include "VideoConfig.h"
#include "VideoSyncController.h"
#include "common/IngestRingBuffer.h"
#include "common/MediaConfig.h"
#include "common/MediaTime.h"

namespace media {

enum class PopResult : uint8_t { Nothing, Popped, PoppedWithSkip };

// Single-stream video frame queue for broadcast playback.
// Receives encoded VP9 frame descriptors (absOffset into IngestRingBuffer)
// from ingest thread, provides them to decode thread.
// Thread safety: pushEncodedFrame from ingest thread, pop/peek from decode thread.
// All access serialized by internal mutex.
// No per-frame heap allocation — frames are ring-buffer-backed descriptors.
class VideoFrameQueue {
public:
    using WakeCallback = std::function<void()>;
    using ResetCallback = std::function<void()>;
    using KeyFrameRequestCallback = std::function<void()>;

    VideoFrameQueue() = default;

    void setRingBuffer(IngestRingBuffer* ring) noexcept {
        ring_.store(ring, std::memory_order_release);
    }
    [[nodiscard]] IngestRingBuffer* ringBuffer() const noexcept {
        return ring_.load(std::memory_order_acquire);
    }

    // Callbacks are wrapped in shared_ptr so the "deferred" copy we make while holding
    // the push-path mutex is always a pointer bump — never a potential std::function
    // heap-alloc (libc++/libstdc++ SBO is implementation-defined, ~2-3 pointers).
    void setWakeCallback(WakeCallback cb) noexcept {
        auto shared = cb ? std::make_shared<WakeCallback>(std::move(cb)) : nullptr;
        std::lock_guard<std::mutex> lk(mtx_);
        wakeCallback_ = std::move(shared);
    }

    // Called by the decode thread so resetStream() can reset the render clock
    void setResetCallback(ResetCallback cb) noexcept {
        auto shared = cb ? std::make_shared<ResetCallback>(std::move(cb)) : nullptr;
        std::lock_guard<std::mutex> lk(mtx_);
        resetCallback_ = std::move(shared);
    }

    void setKeyFrameRequestCallback(KeyFrameRequestCallback cb) noexcept {
        auto shared = cb ? std::make_shared<KeyFrameRequestCallback>(std::move(cb)) : nullptr;
        std::lock_guard<std::mutex> lk(mtx_);
        keyFrameRequestCallback_ = std::move(shared);
    }

    void reset() noexcept {
        std::shared_ptr<ResetCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            encodedQueue_.clear();
            refreshOldestSnapshot_();
            needsKeyFrame_ = true;
            lastArrivalUs_ = 0;
            metrics_.reset();
            jitter_.reset();
            lastDecodedAbsOffset_.store(0, std::memory_order_relaxed);
            cb = resetCallback_;
        }
        if (cb && *cb) (*cb)();
    }

    // Fire the reset callback without draining queued frames — used on
    // pause→resume so the render clock re-anchors without dropping frames.
    void signalClockReset() noexcept {
        std::shared_ptr<ResetCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = resetCallback_;
        }
        if (cb && *cb) (*cb)();
    }

    bool pushEncodedFrame(long long absOffset, size_t size,
                          int64_t ptsUs, bool isKeyFrame) noexcept {
        if (size == 0) return false;
        if (size > video_config::kMaxEncodedFrameSize) return false;

        std::shared_ptr<WakeCallback> deferredWakeCb;
        std::shared_ptr<KeyFrameRequestCallback> deferredKeyFrameCb;
        bool skipEnqueue = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);

            if (needsKeyFrame_ && !isKeyFrame) {
                metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                metrics_.keyFrameGatedDrops.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (isKeyFrame) {
                needsKeyFrame_ = false;
                metrics_.needsKeyFrame.store(false, std::memory_order_relaxed);
            }

            if (encodedQueue_.size() >= video_config::kDecodeQueueDepth) {
                if (catchupPolicy_.load(std::memory_order_relaxed) == CatchupPolicy::DropToLive) {
                    if (skipToLive()) {
                        metrics_.keyFrameRequests.fetch_add(1, std::memory_order_relaxed);
                        deferredKeyFrameCb = keyFrameRequestCallback_;
                    }
                } else {
                    bool droppedKeyFrame = encodedQueue_.front().isKeyFrame;
                    encodedQueue_.pop_front();
                    metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                    if (droppedKeyFrame) {
                        encodedQueue_.clear();
                        needsKeyFrame_ = true;
                        metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
                        metrics_.keyFrameRequests.fetch_add(1, std::memory_order_relaxed);
                        deferredKeyFrameCb = keyFrameRequestCallback_;
                    }
                    refreshOldestSnapshot_();
                }
            }

            // Re-check after drop-and-clear: if a keyframe drop flipped needsKeyFrame_
            // or skipToLive() cleared the queue and set needsKeyFrame_, skip the
            // enqueue so the queue never contains a P-frame without its reference.
            // Still fire the deferred keyframe callback (outside the lock) so the
            // producer requests a fresh keyframe.
            skipEnqueue = (needsKeyFrame_ && !isKeyFrame);
            if (skipEnqueue) {
                metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                metrics_.keyFrameGatedDrops.fetch_add(1, std::memory_order_relaxed);
                refreshOldestSnapshot_();
            } else {

            int64_t arrivalUs = nowUs();

            // Track inter-frame delivery gap
            if (lastArrivalUs_ > 0) {
                int64_t gapUs = arrivalUs - lastArrivalUs_;
                int64_t curMax = metrics_.maxInterFrameGapUs.load(std::memory_order_relaxed);
                while (gapUs > curMax &&
                       !metrics_.maxInterFrameGapUs.compare_exchange_weak(
                           curMax, gapUs, std::memory_order_relaxed)) {}
                if (gapUs > media::config::gap::kBucket500msUs) {
                    metrics_.gapsOver500ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                } else if (gapUs > media::config::gap::kBucket100msUs) {
                    metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                } else if (gapUs > media::config::gap::kBucket50msUs) {
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                }
            }
            lastArrivalUs_ = arrivalUs;

            jitter_.onSample(ptsUs, arrivalUs, isKeyFrame);
            metrics_.driftPpm.store(jitter_.longTermDriftPpm(), std::memory_order_relaxed);

            EncodedVideoFrame frame;
            frame.absOffset = absOffset;
            frame.size = size;
            frame.ptsUs = ptsUs;
            frame.isKeyFrame = isKeyFrame;
            encodedQueue_.push_back(frame);
            refreshOldestSnapshot_();

            // A keyframe entering via the DropToLive branch observes
            // needsKeyFrame_=true (set by skipToLive()) after the top-of-function
            // clear; re-clear here so the *next* P-frame isn't wrongly gated.
            if (isKeyFrame && needsKeyFrame_) {
                needsKeyFrame_ = false;
                metrics_.needsKeyFrame.store(false, std::memory_order_relaxed);
            }

            metrics_.framesReceived.fetch_add(1, std::memory_order_relaxed);

            deferredWakeCb = wakeCallback_;
            }  // end `else { ... }` — successful enqueue path
        }
        // Fire callbacks outside the lock to prevent deadlock if they re-enter.
        // On the skipEnqueue path wakeCb is unset (no new frame) but the keyframe
        // request still propagates — producer must be told to send a keyframe.
        if (deferredWakeCb && *deferredWakeCb) (*deferredWakeCb)();
        if (deferredKeyFrameCb && *deferredKeyFrameCb) (*deferredKeyFrameCb)();
        return !skipEnqueue;
    }

    bool popEncodedFrame(EncodedVideoFrame& out) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (encodedQueue_.empty()) return false;
        out = encodedQueue_.front();
        encodedQueue_.pop_front();
        refreshOldestSnapshot_();
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

        out = encodedQueue_.front();
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
            out = encodedQueue_.front();
            encodedQueue_.pop_front();
            skipped = true;
        }

        refreshOldestSnapshot_();
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
        std::shared_ptr<KeyFrameRequestCallback> deferredCb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Dedup: only fire JS on the false→true transition. Sync (VP9Decoder)
            // and outer (VideoDecodeThread) error paths often back-to-back call
            // requestKeyFrame on the same packet; stall recovery re-requests
            // every 2s while needsKeyFrame_ stays true. JS only needs to know
            // once that a keyframe is needed — repeated nudges go through
            // VideoSurfaceRegistry::requestKeyFrame from those flows instead.
            bool wasAwaiting = needsKeyFrame_;
            needsKeyFrame_ = true;
            metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
            metrics_.keyFrameRequests.fetch_add(1, std::memory_order_relaxed);
            encodedQueue_.clear();
            refreshOldestSnapshot_();
            if (!wasAwaiting) deferredCb = keyFrameRequestCallback_;
        }
        if (deferredCb && *deferredCb) (*deferredCb)();
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

    void onFrameDecoded(long long absOffset) noexcept {
        lastDecodedAbsOffset_.store(absOffset, std::memory_order_relaxed);
    }

    [[nodiscard]] long long lastDecodedAbsOffset() const noexcept {
        return lastDecodedAbsOffset_.load(std::memory_order_relaxed);
    }

    // Lock-free: returns a recent snapshot of the oldest queued frame's absOffset
    // (0 if empty). Refreshed under mtx_ by every push/pop/clear path. Ingest thread
    // reads this every cycle — keeping it atomic avoids contending on mtx_ with the
    // video decode thread's pop path.
    [[nodiscard]] long long oldestQueuedAbsOffset() const noexcept {
        return oldestQueuedAbsOffset_.load(std::memory_order_acquire);
    }

    VideoStreamMetrics& metrics() noexcept { return metrics_; }
    const VideoStreamMetrics& metrics() const noexcept { return metrics_; }

private:
    // Refreshes the oldest-queued absOffset snapshot. Caller must hold mtx_.
    void refreshOldestSnapshot_() noexcept {
        oldestQueuedAbsOffset_.store(
            encodedQueue_.empty() ? 0 : encodedQueue_.front().absOffset,
            std::memory_order_release);
    }

    // Clear queue for DropToLive policy. Remaining non-keyframes would bypass
    // keyframe gating in pop paths, causing video corruption. Caller must hold mtx_.
    // Returns true if a keyframe request callback should be fired (outside the lock).
    bool skipToLive() noexcept {
        size_t dropped = encodedQueue_.size();
        encodedQueue_.clear();
        refreshOldestSnapshot_();
        if (dropped > 0) {
            metrics_.framesDropped.fetch_add(dropped, std::memory_order_relaxed);
            needsKeyFrame_ = true;
            metrics_.needsKeyFrame.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Fixed-capacity ring buffer for encoded frames. No heap allocation after construction.
    // Capacity is kDecodeQueueDepth + 1 to distinguish full from empty (wastes one slot).
    static constexpr size_t kRingCapacity = video_config::kDecodeQueueDepth + 1;
    struct FrameRing {
        std::array<EncodedVideoFrame, kRingCapacity> buf{};
        size_t head = 0;  // index of next pop
        size_t tail = 0;  // index of next push

        [[nodiscard]] size_t size() const noexcept {
            return (tail + kRingCapacity - head) % kRingCapacity;
        }
        [[nodiscard]] bool empty() const noexcept { return head == tail; }
        [[nodiscard]] bool full() const noexcept {
            return (tail + 1) % kRingCapacity == head;
        }
        EncodedVideoFrame& front() noexcept { return buf[head]; }
        const EncodedVideoFrame& front() const noexcept { return buf[head]; }
        void pop_front() noexcept { head = (head + 1) % kRingCapacity; }
        void push_back(const EncodedVideoFrame& f) noexcept {
            buf[tail] = f;
            tail = (tail + 1) % kRingCapacity;
        }
        void clear() noexcept { head = tail = 0; }
    };

    mutable std::mutex mtx_;
    FrameRing encodedQueue_;
    bool needsKeyFrame_ = true;
    std::shared_ptr<WakeCallback> wakeCallback_;
    std::shared_ptr<ResetCallback> resetCallback_;
    std::shared_ptr<KeyFrameRequestCallback> keyFrameRequestCallback_;
    int64_t lastArrivalUs_ = 0;
    VideoStreamMetrics metrics_;
    VideoJitterEstimator jitter_;
    std::atomic<IngestRingBuffer*> ring_{nullptr};
    std::atomic<long long> lastDecodedAbsOffset_{0};
    std::atomic<long long> oldestQueuedAbsOffset_{0};  // snapshot refreshed under mtx_
    std::atomic<CatchupPolicy> catchupPolicy_{CatchupPolicy::PlayThrough};
    std::atomic<float> playbackRate_{config::playbackrate::kDefaultRate};
};

}  // namespace media
