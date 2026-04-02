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

enum class StreamStatus : uint8_t {
    Live = 0,
    Buffering = 1,
    Ended = 2,
    NoPeers = 3
};

inline const char* streamStatusToString(StreamStatus s) noexcept {
    switch (s) {
        case StreamStatus::Live:      return "live";
        case StreamStatus::Buffering: return "buffering";
        case StreamStatus::Ended:     return "ended";
        case StreamStatus::NoPeers:   return "no_peers";
    }
    return "unknown";
}

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
    uint64_t decoderResets = 0;
    int64_t uptimeUs = 0;
    bool decodeThreadDetached = false;
    bool audioOutputRunning = true;
    bool paused = false;
    uint64_t gapsOver500ms = 0;
    int64_t avSyncOffsetUs = 0;
    int64_t bufferTargetUs = 0;
    uint64_t gapsOver100ms = 0;

    // Audio decode stall detection
    uint64_t audioFramesReceived = 0;
    uint64_t audioSamplesOutput = 0;

    // Buffer depth for early stall warning
    int64_t bufferedDurationUs = 0;
    int64_t decodedDurationUs = 0;

    // Video health
    uint64_t videoFramesReceived = 0;
    uint64_t videoFramesDecoded = 0;
    uint64_t videoFramesDropped = 0;

    // Pool pressure (< 25% available)
    bool decodedPoolUnderPressure = false;
    bool encodedPoolUnderPressure = false;

    // Stream context from integrator
    StreamStatus streamStatus = StreamStatus::Live;
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
    constexpr int64_t kAvSyncDegradedThresholdUs = 45000;     // ITU-R BT.1359: > 45ms sustained
    constexpr uint64_t kVideoDropRateDenom = 10;              // Degraded if drops > 1/10 of received
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

    void requestWindowReset() noexcept {
        windowResetRequested_.store(true, std::memory_order_release);
    }

    void evaluate() noexcept {
        int64_t now = nowUs();

        // Rate limit
        if (now - lastEventTimeUs_ < config::health::kMinEventIntervalUs) return;

        HealthSnapshot snap = readMetrics_();

        // Service deferred window reset (called from JS thread via resetStream)
        if (windowResetRequested_.load(std::memory_order_acquire)) {
            windowResetRequested_.store(false, std::memory_order_relaxed);
            windowStartUs_ = now;
            windowBaseSnapshot_ = snap;
            currentHealth_.store(StreamHealth::Buffering, std::memory_order_relaxed);
            lastPublishedHealth_.store(StreamHealth::Buffering, std::memory_order_relaxed);
        }

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
        uint64_t resetDelta = snap.decoderResets - windowBaseSnapshot_.decoderResets;
        uint64_t audioRecvDelta = snap.audioFramesReceived - windowBaseSnapshot_.audioFramesReceived;
        uint64_t audioOutputDelta = snap.audioSamplesOutput - windowBaseSnapshot_.audioSamplesOutput;
        uint64_t videoRecvDelta = snap.videoFramesReceived - windowBaseSnapshot_.videoFramesReceived;
        uint64_t videoDecDelta = snap.videoFramesDecoded - windowBaseSnapshot_.videoFramesDecoded;
        uint64_t videoDropDelta = snap.videoFramesDropped - windowBaseSnapshot_.videoFramesDropped;

        // Classify health
        StreamHealth newHealth = classify(snap, underrunDelta, errorDelta, gapDelta, dropDelta,
                                           resetDelta, audioRecvDelta, audioOutputDelta,
                                           videoRecvDelta, videoDecDelta, videoDropDelta);

        // Only fire on transition
        StreamHealth oldHealth = currentHealth_.load(std::memory_order_relaxed);
        if (newHealth == oldHealth) return;
        int64_t absOffset = snap.avSyncOffsetUs >= 0 ? snap.avSyncOffsetUs : -snap.avSyncOffsetUs;
        bool poolPressure = snap.decodedPoolUnderPressure && snap.encodedPoolUnderPressure;
        const char* detail = transitionDetail(oldHealth, newHealth,
                                               underrunDelta, errorDelta, gapDelta,
                                               resetDelta, absOffset, videoDropDelta,
                                               snap.streamStatus, poolPressure);

        currentHealth_.store(newHealth, std::memory_order_relaxed);
        lastPublishedHealth_.store(newHealth, std::memory_order_release);
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
        return currentHealth_.load(std::memory_order_acquire);
    }

    [[nodiscard]] StreamHealth publishedHealth() const noexcept {
        return lastPublishedHealth_.load(std::memory_order_acquire);
    }

    // Pure classification functions — public for testability, no state dependency.
    static StreamHealth classify(const HealthSnapshot& snap,
                                  uint64_t underrunDelta,
                                  uint64_t errorDelta,
                                  uint64_t gapDelta,
                                  uint64_t dropDelta,
                                  uint64_t resetDelta,
                                  uint64_t audioRecvDelta,
                                  uint64_t audioOutputDelta,
                                  uint64_t videoRecvDelta,
                                  uint64_t videoDecDelta,
                                  uint64_t videoDropDelta) noexcept {
        // Failed: unrecoverable conditions
        if (snap.decodeThreadDetached) return StreamHealth::Failed;
        if (!snap.audioOutputRunning) return StreamHealth::Failed;
        if (resetDelta >= config::health::kFailedResetThreshold)
            return StreamHealth::Failed;

        // Stalled: no data flowing
        if (gapDelta >= config::health::kStalledGapThreshold)
            return StreamHealth::Stalled;

        // Degraded: playing but with significant quality loss
        if (underrunDelta >= config::health::kDegradedUnderrunThreshold)
            return StreamHealth::Degraded;
        if (errorDelta >= config::health::kDegradedDecodeErrorThreshold)
            return StreamHealth::Degraded;
        // Audio receiving frames but not producing output — decoder is failing.
        // Require meaningful receive count to avoid false trigger during initial buffering.
        if (audioRecvDelta > 10 && audioOutputDelta == 0)
            return StreamHealth::Degraded;
        // Video receiving frames but not decoding any — decoder is failing.
        if (videoRecvDelta > 10 && videoDecDelta == 0)
            return StreamHealth::Degraded;
        // Sustained A/V desync beyond ITU-R BT.1359 perceptible threshold
        int64_t absOffset = snap.avSyncOffsetUs >= 0 ? snap.avSyncOffsetUs : -snap.avSyncOffsetUs;
        if (absOffset > config::health::kAvSyncDegradedThresholdUs)
            return StreamHealth::Degraded;
        // Audio dropping > 10% of received frames
        if (audioRecvDelta > 0 && dropDelta * config::health::kVideoDropRateDenom > audioRecvDelta)
            return StreamHealth::Degraded;
        // Video dropping > 10% of received frames
        if (videoRecvDelta > 0 && videoDropDelta * config::health::kVideoDropRateDenom > videoRecvDelta)
            return StreamHealth::Degraded;
        // Both frame pools under pressure — capacity limit, degradation imminent
        if (snap.decodedPoolUnderPressure && snap.encodedPoolUnderPressure)
            return StreamHealth::Degraded;

        // Buffering: occasional issues, recovering
        if (underrunDelta > 0) return StreamHealth::Buffering;

        return StreamHealth::Healthy;
    }

    static const char* transitionDetail(StreamHealth from, StreamHealth to,
                                         uint64_t underrunDelta,
                                         uint64_t errorDelta,
                                         uint64_t gapDelta,
                                         uint64_t resetDelta,
                                         int64_t absAvSyncOffset,
                                         uint64_t videoDropDelta,
                                         StreamStatus streamStatus = StreamStatus::Live,
                                         bool poolPressure = false) noexcept {
        if (to == StreamHealth::Failed) {
            if (resetDelta >= config::health::kFailedResetThreshold)
                return "failed: excessive decoder resets";
            return "audio output failed or decode thread detached";
        }
        if (to == StreamHealth::Stalled) {
            switch (streamStatus) {
                case StreamStatus::NoPeers:   return "stalled: no peers connected";
                case StreamStatus::Ended:     return "stalled: stream ended";
                case StreamStatus::Buffering: return "stalled: replication lag";
                default:                      return "feed stalled: gap > 500ms";
            }
        }
        if (to == StreamHealth::Degraded) {
            if (underrunDelta >= config::health::kDegradedUnderrunThreshold)
                return "degraded: sustained underruns";
            if (errorDelta >= config::health::kDegradedDecodeErrorThreshold)
                return "degraded: sustained decode errors";
            if (absAvSyncOffset > config::health::kAvSyncDegradedThresholdUs)
                return "degraded: A/V sync drift > 45ms";
            if (videoDropDelta > 0)
                return "degraded: high video frame drop rate";
            if (poolPressure)
                return "degraded: frame pool pressure";
            return "degraded: video decode stalled";
        }
        if (to == StreamHealth::Buffering) return "rebuffering: audio underrun";
        if (to == StreamHealth::Healthy) return "playback recovered";
        return "unknown transition";
    }

private:
    HealthMetricsReader readMetrics_;
    HealthCallback callback_;

    std::atomic<StreamHealth> currentHealth_{StreamHealth::Buffering};
    std::atomic<StreamHealth> lastPublishedHealth_{StreamHealth::Buffering};
    int64_t lastEventTimeUs_{0};
    int64_t windowStartUs_{0};
    HealthSnapshot windowBaseSnapshot_{};
    std::atomic<bool> windowResetRequested_{false};
};

}  // namespace media
