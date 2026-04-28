// Automatic feed stall detection and recovery for production broadcast playback.
//
// Detects when no new data arrives for a configurable threshold, proactively
// fires keyframe requests, and manages graceful degradation/recovery transitions.
// All state is lock-free atomic — safe to query from any thread.
//
// Threading: onDataReceived() called from JS thread (feedData path).
// evaluate() called from decode thread (health watchdog cycle).
// All read accessors safe from any thread (relaxed atomics).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include "MediaTime.h"
#include "MediaLog.h"
#include "MediaConfig.h"

namespace media {

namespace config::stall {
    // Time without new data before declaring a stall
    constexpr int64_t kStallThresholdUs = 500'000;        // 500ms

    // Minimum interval between keyframe requests during sustained stall
    constexpr int64_t kKeyFrameRequestIntervalUs = 2'000'000;  // 2s

    // After recovery, require this much buffer before resuming playback.
    // Prevents immediate re-stall if data arrives in a thin trickle.
    constexpr int64_t kRecoveryBufferThresholdUs = 60'000;  // 3 frames (60ms)

    // Maximum stall duration before declaring the stream failed
    constexpr int64_t kMaxStallDurationUs = 30'000'000;  // 30s

    // Maximum time to spend in Recovering state before declaring the stream
    // failed. Prevents the controller from sticking in Recovering when data
    // resumes as a thin trickle below decode rate (buffer never crosses
    // kRecoveryBufferThresholdUs).
    constexpr int64_t kMaxRecoveryDurationUs = 30'000'000;  // 30s
}

enum class StallState : uint8_t {
    Healthy = 0,      // Data flowing normally
    Detecting = 1,    // Gap detected, monitoring (< kStallThresholdUs)
    Stalled = 2,      // Confirmed stall, keyframe requested
    Recovering = 3,   // Data resumed, rebuffering
    Failed = 4        // Stall exceeded max duration
};

inline const char* stallStateToString(StallState s) noexcept {
    switch (s) {
        case StallState::Healthy:    return "healthy";
        case StallState::Detecting:  return "detecting";
        case StallState::Stalled:    return "stalled";
        case StallState::Recovering: return "recovering";
        case StallState::Failed:     return "failed";
    }
    return "unknown";
}

struct StallMetrics {
    uint32_t stallCount = 0;
    uint32_t recoveryCount = 0;
    uint32_t keyFrameRequests = 0;
    int64_t totalStallDurationUs = 0;
    int64_t lastStallDurationUs = 0;
    int64_t longestStallUs = 0;
    int64_t lastRecoveryDurationUs = 0;
    int64_t longestRecoveryUs = 0;
    StallState currentState = StallState::Healthy;
};

class StallRecoveryController {
public:
    using KeyFrameRequestFn = std::function<void()>;
    using VideoKeyFrameRequestFn = std::function<void()>;

    void setKeyFrameRequestFn(KeyFrameRequestFn fn) noexcept {
        std::lock_guard<std::mutex> lk(fnMtx_);
        keyFrameRequestFn_ = std::move(fn);
    }

    void setVideoKeyFrameRequestFn(VideoKeyFrameRequestFn fn) noexcept {
        std::lock_guard<std::mutex> lk(fnMtx_);
        videoKeyFrameRequestFn_ = std::move(fn);
    }

    // Called from feedData() on JS thread when new data arrives.
    void onDataReceived() noexcept {
        int64_t now = nowUs();
        lastDataTimeUs_.store(now, std::memory_order_relaxed);

        StallState current = state_.load(std::memory_order_acquire);

        if (current == StallState::Stalled) {
            // Data resumed after stall — transition to recovering
            recoveryStartUs_.store(now, std::memory_order_relaxed);
            state_.store(StallState::Recovering, std::memory_order_release);
            MEDIA_LOG_I("StallRecovery: data resumed, rebuffering");
        } else if (current == StallState::Failed) {
            // Even after failure, if data arrives, attempt recovery
            recoveryStartUs_.store(now, std::memory_order_relaxed);
            state_.store(StallState::Recovering, std::memory_order_release);
            MEDIA_LOG_I("StallRecovery: data resumed after failure, attempting recovery");
        } else if (current == StallState::Detecting) {
            // Data arrived before threshold — cancel detection
            state_.store(StallState::Healthy, std::memory_order_release);
        }
    }

    // Called when sufficient buffer has been rebuilt after a stall.
    // Typically called from the decode thread when buffer > threshold.
    void onBufferSufficient() noexcept {
        StallState current = state_.load(std::memory_order_acquire);
        if (current == StallState::Recovering) {
            int64_t stallDuration = recoveryStartUs_.load(std::memory_order_relaxed) -
                                   stallStartUs_.load(std::memory_order_relaxed);
            lastStallDurationUs_.store(stallDuration, std::memory_order_relaxed);
            totalStallDurationUs_.fetch_add(stallDuration, std::memory_order_relaxed);

            int64_t longest = longestStallUs_.load(std::memory_order_relaxed);
            while (stallDuration > longest &&
                   !longestStallUs_.compare_exchange_weak(
                       longest, stallDuration, std::memory_order_relaxed)) {}

            int64_t recoveryDuration = nowUs() - recoveryStartUs_.load(std::memory_order_relaxed);
            lastRecoveryDurationUs_.store(recoveryDuration, std::memory_order_relaxed);
            int64_t longestRecovery = longestRecoveryUs_.load(std::memory_order_relaxed);
            while (recoveryDuration > longestRecovery &&
                   !longestRecoveryUs_.compare_exchange_weak(
                       longestRecovery, recoveryDuration, std::memory_order_relaxed)) {}

            recoveryCount_.fetch_add(1, std::memory_order_relaxed);
            state_.store(StallState::Healthy, std::memory_order_release);
            MEDIA_LOG_I("StallRecovery: recovered (stall=%lldms, rebuffer=%lldms)",
                        static_cast<long long>(stallDuration / 1000),
                        static_cast<long long>(recoveryDuration / 1000));
        }
    }

    // Called periodically (e.g., from decode thread health cycle) to evaluate
    // stall state and fire keyframe requests as needed.
    void evaluate() noexcept {
        StallState current = state_.load(std::memory_order_acquire);
        if (current == StallState::Failed) return;

        int64_t now = nowUs();
        int64_t lastData = lastDataTimeUs_.load(std::memory_order_relaxed);
        if (lastData == 0) return;

        int64_t gap = now - lastData;

        switch (current) {
        case StallState::Healthy:
            if (gap > config::stall::kStallThresholdUs / 2) {
                state_.store(StallState::Detecting, std::memory_order_release);
            }
            break;

        case StallState::Detecting:
            if (gap > config::stall::kStallThresholdUs) {
                state_.store(StallState::Stalled, std::memory_order_release);
                stallStartUs_.store(lastData, std::memory_order_relaxed);
                stallCount_.fetch_add(1, std::memory_order_relaxed);
                MEDIA_LOG_W("StallRecovery: feed stalled (gap=%lldms)",
                            static_cast<long long>(gap / 1000));
                requestKeyFrame(now);
            }
            break;

        case StallState::Stalled: {
            // Check for max stall duration
            int64_t stallDuration = now - stallStartUs_.load(std::memory_order_relaxed);
            if (stallDuration > config::stall::kMaxStallDurationUs) {
                state_.store(StallState::Failed, std::memory_order_release);
                MEDIA_LOG_E("StallRecovery: max stall duration exceeded (%llds)",
                            static_cast<long long>(stallDuration / 1'000'000));
                break;
            }

            // Periodic keyframe re-requests during sustained stall
            int64_t sinceLastRequest = now - lastKeyFrameRequestUs_.load(std::memory_order_relaxed);
            if (sinceLastRequest >= config::stall::kKeyFrameRequestIntervalUs) {
                requestKeyFrame(now);
            }
            break;
        }

        case StallState::Recovering: {
            // onBufferSufficient() handles transition back to Healthy.
            // If recovery never crosses the buffer threshold (e.g. thin trickle
            // below decode rate), declare Failed so HealthWatchdog sees the
            // terminal state instead of leaving stallCount > recoveryCount
            // permanently.
            int64_t recoveryDuration = now - recoveryStartUs_.load(std::memory_order_relaxed);
            if (recoveryDuration > config::stall::kMaxRecoveryDurationUs) {
                state_.store(StallState::Failed, std::memory_order_release);
                MEDIA_LOG_E("StallRecovery: recovery timeout (%llds), declaring failed",
                            static_cast<long long>(recoveryDuration / 1'000'000));
            }
            break;
        }

        case StallState::Failed:
            break;
        }
    }

    [[nodiscard]] StallState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isStalled() const noexcept {
        StallState s = state();
        return s == StallState::Stalled || s == StallState::Failed;
    }

    [[nodiscard]] StallMetrics metrics() const noexcept {
        StallMetrics m{};
        m.stallCount = stallCount_.load(std::memory_order_relaxed);
        m.recoveryCount = recoveryCount_.load(std::memory_order_relaxed);
        m.keyFrameRequests = keyFrameRequestCount_.load(std::memory_order_relaxed);
        m.totalStallDurationUs = totalStallDurationUs_.load(std::memory_order_relaxed);
        m.lastStallDurationUs = lastStallDurationUs_.load(std::memory_order_relaxed);
        m.longestStallUs = longestStallUs_.load(std::memory_order_relaxed);
        m.lastRecoveryDurationUs = lastRecoveryDurationUs_.load(std::memory_order_relaxed);
        m.longestRecoveryUs = longestRecoveryUs_.load(std::memory_order_relaxed);
        m.currentState = state_.load(std::memory_order_relaxed);
        return m;
    }

    void reset() noexcept {
        state_.store(StallState::Healthy, std::memory_order_relaxed);
        lastDataTimeUs_.store(0, std::memory_order_relaxed);
        stallCount_.store(0, std::memory_order_relaxed);
        recoveryCount_.store(0, std::memory_order_relaxed);
        keyFrameRequestCount_.store(0, std::memory_order_relaxed);
        totalStallDurationUs_.store(0, std::memory_order_relaxed);
        lastStallDurationUs_.store(0, std::memory_order_relaxed);
        longestStallUs_.store(0, std::memory_order_relaxed);
        lastRecoveryDurationUs_.store(0, std::memory_order_relaxed);
        longestRecoveryUs_.store(0, std::memory_order_relaxed);
        stallStartUs_.store(0, std::memory_order_relaxed);
        recoveryStartUs_.store(0, std::memory_order_relaxed);
        lastKeyFrameRequestUs_.store(0, std::memory_order_relaxed);
    }

private:
    void requestKeyFrame(int64_t now) noexcept {
        lastKeyFrameRequestUs_.store(now, std::memory_order_relaxed);
        keyFrameRequestCount_.fetch_add(1, std::memory_order_relaxed);

        // Copy callbacks under lock, invoke outside to prevent deadlock
        // if a callback re-enters StallRecoveryController.
        KeyFrameRequestFn videoFn;
        KeyFrameRequestFn audioFn;
        {
            std::lock_guard<std::mutex> lk(fnMtx_);
            videoFn = videoKeyFrameRequestFn_;
            audioFn = keyFrameRequestFn_;
        }
        if (videoFn) videoFn();
        if (audioFn) audioFn();

        MEDIA_LOG_I("StallRecovery: keyframe requested (#%u)",
                    keyFrameRequestCount_.load(std::memory_order_relaxed));
    }

    std::atomic<StallState> state_{StallState::Healthy};
    std::atomic<int64_t> lastDataTimeUs_{0};

    // Stall tracking (cross-thread: JS writes onDataReceived, decode reads evaluate)
    std::atomic<int64_t> stallStartUs_{0};
    std::atomic<int64_t> recoveryStartUs_{0};
    std::atomic<int64_t> lastKeyFrameRequestUs_{0};

    // Metrics (monotonic counters)
    std::atomic<uint32_t> stallCount_{0};
    std::atomic<uint32_t> recoveryCount_{0};
    std::atomic<uint32_t> keyFrameRequestCount_{0};
    std::atomic<int64_t> totalStallDurationUs_{0};
    std::atomic<int64_t> lastStallDurationUs_{0};
    std::atomic<int64_t> longestStallUs_{0};
    std::atomic<int64_t> lastRecoveryDurationUs_{0};
    std::atomic<int64_t> longestRecoveryUs_{0};

    std::mutex fnMtx_;
    KeyFrameRequestFn keyFrameRequestFn_;
    VideoKeyFrameRequestFn videoKeyFrameRequestFn_;
};

}  // namespace media
