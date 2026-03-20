// Health state machine evaluated on decode thread. Fires callback on state transitions.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include "MediaTime.h"
#include "MediaLog.h"

namespace media {

enum class StreamHealth : uint8_t {
    Healthy = 0,
    Buffering = 1,
    Degraded = 2,
    Stalled = 3,
    Failed = 4
};

inline const char* streamHealthToString(StreamHealth h) noexcept {
    switch (h) {
        case StreamHealth::Healthy:   return "healthy";
        case StreamHealth::Buffering: return "buffering";
        case StreamHealth::Degraded:  return "degraded";
        case StreamHealth::Stalled:   return "stalled";
        case StreamHealth::Failed:    return "failed";
    }
    return "unknown";
}

struct HealthSnapshot {
    uint64_t underruns = 0;
    uint64_t decodeErrors = 0;
    uint64_t framesDropped = 0;
    uint64_t ptsDiscontinuities = 0;
    int64_t uptimeUs = 0;
    bool decodeThreadDetached = false;
    bool audioOutputRunning = true;
    bool paused = false;
    uint64_t gapsOver500ms = 0;
    int64_t avSyncOffsetUs = 0;
    int64_t bufferTargetUs = 0;
    uint64_t gapsOver100ms = 0;
};

struct HealthEvent {
    StreamHealth status;
    const char* detail;
    HealthSnapshot metrics;
};

using HealthCallback = std::function<void(const HealthEvent&)>;
using HealthMetricsReader = std::function<HealthSnapshot()>;

namespace config::health {
    constexpr int64_t kMinEventIntervalUs = 500'000;           // 500ms rate limit
    constexpr int64_t kEvalWindowUs = 5'000'000;               // 5s evaluation window
    constexpr uint64_t kDegradedUnderrunThreshold = 5;         // underruns in window
    constexpr uint64_t kDegradedDecodeErrorThreshold = 3;      // errors in window
    constexpr uint64_t kStalledGapThreshold = 1;               // any 500ms+ gap
    constexpr uint64_t kFailedResetThreshold = 3;              // resets in window
}

// Thread safety: evaluate() must only be called from the decode thread.
// The callback may dispatch to another thread (e.g., JS via CallInvoker).
class HealthWatchdog {
public:
    HealthWatchdog(HealthMetricsReader readMetrics, HealthCallback callback) noexcept
        : readMetrics_(std::move(readMetrics))
        , callback_(std::move(callback)) {}

    HealthWatchdog(const HealthWatchdog&) = delete;
    HealthWatchdog& operator=(const HealthWatchdog&) = delete;

    void evaluate() noexcept {
        int64_t now = nowUs();

        // Rate limit
        if (now - lastEventTimeUs_ < config::health::kMinEventIntervalUs) return;

        HealthSnapshot snap = readMetrics_();

        // When paused, suppress health evaluation — don't fire transitions
        if (snap.paused) return;

        // Reset window counters periodically
        if (now - windowStartUs_ >= config::health::kEvalWindowUs) {
            windowStartUs_ = now;
            windowBaseSnapshot_ = snap;
        }

        // Compute deltas within the current window
        uint64_t underrunDelta = snap.underruns - windowBaseSnapshot_.underruns;
        uint64_t errorDelta = snap.decodeErrors - windowBaseSnapshot_.decodeErrors;
        uint64_t gapDelta = snap.gapsOver500ms - windowBaseSnapshot_.gapsOver500ms;
        uint64_t dropDelta = snap.framesDropped - windowBaseSnapshot_.framesDropped;

        // Classify health
        StreamHealth newHealth = classify(snap, underrunDelta, errorDelta, gapDelta, dropDelta);

        // Only fire on transition
        if (newHealth == currentHealth_) return;

        StreamHealth oldHealth = currentHealth_;
        const char* detail = transitionDetail(oldHealth, newHealth,
                                               underrunDelta, errorDelta, gapDelta);

        currentHealth_ = newHealth;
        lastPublishedHealth_.store(newHealth, std::memory_order_relaxed);
        lastEventTimeUs_ = now;

        MEDIA_LOG_I("HealthWatchdog: %s → %s (%s)",
                    streamHealthToString(oldHealth),
                    streamHealthToString(newHealth),
                    detail);

        if (callback_) {
            callback_(HealthEvent{newHealth, detail, snap});
        }
    }

    [[nodiscard]] StreamHealth currentHealth() const noexcept {
        return currentHealth_;
    }

    [[nodiscard]] StreamHealth publishedHealth() const noexcept {
        return lastPublishedHealth_.load(std::memory_order_relaxed);
    }

private:
    static StreamHealth classify(const HealthSnapshot& snap,
                                  uint64_t underrunDelta,
                                  uint64_t errorDelta,
                                  uint64_t gapDelta,
                                  uint64_t dropDelta) noexcept {
        // Failed: unrecoverable conditions
        if (snap.decodeThreadDetached) return StreamHealth::Failed;
        if (!snap.audioOutputRunning) return StreamHealth::Failed;

        // Stalled: no data flowing
        if (gapDelta >= config::health::kStalledGapThreshold)
            return StreamHealth::Stalled;

        // Degraded: playing but with significant quality loss
        if (underrunDelta >= config::health::kDegradedUnderrunThreshold)
            return StreamHealth::Degraded;
        if (errorDelta >= config::health::kDegradedDecodeErrorThreshold)
            return StreamHealth::Degraded;

        // Buffering: occasional issues, recovering
        if (underrunDelta > 0) return StreamHealth::Buffering;

        return StreamHealth::Healthy;
    }

    static const char* transitionDetail(StreamHealth from, StreamHealth to,
                                         uint64_t underrunDelta,
                                         uint64_t errorDelta,
                                         uint64_t gapDelta) noexcept {
        if (to == StreamHealth::Failed) return "audio output failed or decode thread detached";
        if (to == StreamHealth::Stalled) return "feed stalled: gap > 500ms";
        if (to == StreamHealth::Degraded) {
            if (underrunDelta >= config::health::kDegradedUnderrunThreshold)
                return "degraded: sustained underruns";
            if (errorDelta >= config::health::kDegradedDecodeErrorThreshold)
                return "degraded: sustained decode errors";
            return "degraded: quality loss";
        }
        if (to == StreamHealth::Buffering) return "rebuffering: audio underrun";
        if (to == StreamHealth::Healthy) return "playback recovered";
        return "unknown transition";
    }

    HealthMetricsReader readMetrics_;
    HealthCallback callback_;

    StreamHealth currentHealth_{StreamHealth::Buffering};
    std::atomic<StreamHealth> lastPublishedHealth_{StreamHealth::Buffering};
    int64_t lastEventTimeUs_{0};
    int64_t windowStartUs_{0};
    HealthSnapshot windowBaseSnapshot_{};
};

}  // namespace media
