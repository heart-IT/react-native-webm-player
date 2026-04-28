// Health state machine evaluated on decode thread. Fires callback on state transitions.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include "MediaConfig.h"
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
    uint64_t gapsOver50ms = 0;

    // Ring-buffer producer-side rejects: JS feedData() found the ring full and
    // dropped the chunk. Distinct from gaps (decode-side starvation).
    uint64_t ingestRingWriteRejects = 0;

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

    // Video decoder diagnostics
    bool needsKeyFrame = false;
    uint64_t videoDecodeErrors = 0;
    uint64_t videoDecoderResets = 0;
    uint8_t videoDecoderState = 0;

    // Video decode thread liveness (heartbeat stale > 30s)
    bool videoDecodeThreadDetached = false;

    // Ingest thread liveness
    bool ingestThreadDetached = false;

    // Demuxer health: triage cannot otherwise distinguish "feed stalled"
    // (no JS feedData calls) from "demuxer wedged" (data arriving but
    // nothing parses) without these. parseErrorCount is the cumulative
    // session-wide count; demuxerWedged latches when parseState == Error
    // AND no cached stream header is available (self-heal impossible);
    // timeInErrorMs is non-zero only while parseState == Error.
    uint64_t parseErrorCount = 0;
    int64_t timeInErrorMs = 0;
    uint8_t parseState = 0;
    bool demuxerWedged = false;

    // Stream context from integrator
    StreamStatus streamStatus = StreamStatus::Live;

    // Speculative playback: higher PLC is expected, relax underrun threshold
    bool speculativeMode = false;

    // Audio glitch event metrics (Triage Tree §1, §2). HealthEvent previously
    // exposed only the edge counter `underruns`, leaving sustained silence and
    // fast-path churn invisible to push subscribers — these close that gap.
    uint64_t silenceCallbacks = 0;
    uint64_t framesDrained = 0;
    uint64_t fastPathSwitches = 0;

    // A/V sync excursion (Triage Tree §5) — peakAbsAvSyncOffsetUs survives
    // EWMA smoothing back below 45ms; avSyncExceedCount is the hit count.
    int64_t peakAbsAvSyncOffsetUs = 0;
    uint64_t avSyncExceedCount = 0;

    // Thread liveness early-warning (Triage Tree §6). Detached booleans only
    // fire on the >30s timeout; these gauges show staleness on the way to
    // detachment.
    int64_t timeSinceIngestHeartbeatMs = 0;
    int64_t timeSinceVideoHeartbeatMs = 0;

    // Video freeze detection (Triage Tree §4). currentFps without decay leaves
    // a hung decoder pinned at 30; HealthSnapshot stores the decayed view.
    int currentFps = 0;
};

struct HealthEvent {
    StreamHealth status;
    std::string detail;
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
    constexpr uint64_t kFailedResetThreshold = 3;              // resets in window (audio or video)
    constexpr int64_t kAvSyncDegradedThresholdUs = config::avsync::kEmergencyThresholdUs;  // same 45ms threshold
    constexpr uint64_t kVideoDropRateDenom = 10;              // Degraded if drops > 1/10 of received
    constexpr uint8_t kVideoDecoderStateFailed = 4;           // VideoDecoderState::Failed (avoids video/ include)
    // Demuxer error budget per evaluation window. Below: Degraded; sustained
    // wedge (Error parseState + no header) → Stalled.
    constexpr uint64_t kDegradedParseErrorThreshold = 5;       // delta in 5s window
    constexpr int64_t kStalledTimeInErrorMs = 2'000;           // wedged for >2s
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

        // Initialize window on first evaluate so the first window starts from
        // actual pipeline start, not epoch 0
        if (windowStartUs_ == 0) {
            windowStartUs_ = now;
            windowBaseSnapshot_ = snap;
        }

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

        // Compute deltas within the current window. Clamp to zero — if any counter went
        // backwards (e.g. stream reset that zeroed metrics), the unsigned subtraction
        // would underflow into a huge delta and flip us to Failed.
        auto safeDelta = [](uint64_t cur, uint64_t base) -> uint64_t {
            return cur >= base ? cur - base : 0;
        };
        uint64_t underrunDelta = safeDelta(snap.underruns, windowBaseSnapshot_.underruns);
        uint64_t errorDelta = safeDelta(snap.decodeErrors, windowBaseSnapshot_.decodeErrors);
        uint64_t gapDelta = safeDelta(snap.gapsOver500ms, windowBaseSnapshot_.gapsOver500ms);
        uint64_t dropDelta = safeDelta(snap.framesDropped, windowBaseSnapshot_.framesDropped);
        uint64_t resetDelta = safeDelta(snap.decoderResets, windowBaseSnapshot_.decoderResets);
        uint64_t audioRecvDelta = safeDelta(snap.audioFramesReceived, windowBaseSnapshot_.audioFramesReceived);
        uint64_t audioOutputDelta = safeDelta(snap.audioSamplesOutput, windowBaseSnapshot_.audioSamplesOutput);
        uint64_t videoRecvDelta = safeDelta(snap.videoFramesReceived, windowBaseSnapshot_.videoFramesReceived);
        uint64_t videoDecDelta = safeDelta(snap.videoFramesDecoded, windowBaseSnapshot_.videoFramesDecoded);
        uint64_t videoDropDelta = safeDelta(snap.videoFramesDropped, windowBaseSnapshot_.videoFramesDropped);
        uint64_t videoResetDelta = safeDelta(snap.videoDecoderResets, windowBaseSnapshot_.videoDecoderResets);
        uint64_t parseErrorDelta = safeDelta(snap.parseErrorCount, windowBaseSnapshot_.parseErrorCount);

        // Classify health
        StreamHealth newHealth = classify(snap, underrunDelta, errorDelta, gapDelta, dropDelta,
                                           resetDelta, audioRecvDelta, audioOutputDelta,
                                           videoRecvDelta, videoDecDelta, videoDropDelta,
                                           videoResetDelta, parseErrorDelta);

        // Only fire on transition
        StreamHealth oldHealth = currentHealth_.load(std::memory_order_relaxed);
        if (newHealth == oldHealth) return;
        int64_t absOffset = snap.avSyncOffsetUs >= 0 ? snap.avSyncOffsetUs : -snap.avSyncOffsetUs;
        bool poolPressure = snap.decodedPoolUnderPressure && snap.encodedPoolUnderPressure;
        const char* detail = transitionDetail(oldHealth, newHealth,
                                               underrunDelta, errorDelta, gapDelta,
                                               resetDelta, absOffset, videoDropDelta,
                                               snap.streamStatus, poolPressure,
                                               videoResetDelta, parseErrorDelta,
                                               snap.demuxerWedged);

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
                                  uint64_t videoDropDelta,
                                  uint64_t videoResetDelta = 0,
                                  uint64_t parseErrorDelta = 0) noexcept {
        // Failed: unrecoverable conditions
        if (snap.decodeThreadDetached) return StreamHealth::Failed;
        if (snap.ingestThreadDetached) return StreamHealth::Failed;
        if (!snap.audioOutputRunning) return StreamHealth::Failed;
        if (snap.videoDecodeThreadDetached) return StreamHealth::Failed;
        if (snap.videoDecoderState == config::health::kVideoDecoderStateFailed)
            return StreamHealth::Failed;
        if (resetDelta >= config::health::kFailedResetThreshold)
            return StreamHealth::Failed;
        if (videoResetDelta >= config::health::kFailedResetThreshold)
            return StreamHealth::Failed;

        // Stalled: no data flowing or demuxer wedged for >2s
        if (gapDelta >= config::health::kStalledGapThreshold)
            return StreamHealth::Stalled;
        if (snap.demuxerWedged && snap.timeInErrorMs >= config::health::kStalledTimeInErrorMs)
            return StreamHealth::Stalled;

        // Degraded: playing but with significant quality loss.
        // Speculative mode intentionally tolerates more PLC — raise threshold.
        uint64_t underrunThreshold = snap.speculativeMode
            ? config::health::kDegradedUnderrunThreshold * 2
            : config::health::kDegradedUnderrunThreshold;
        if (underrunDelta >= underrunThreshold)
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
        // Sustained demuxer parse errors (e.g., malformed EBML, truncated cluster).
        // Stops short of Stalled because a transient burst may resolve via the
        // demuxer's self-heal path; sustained wedge is caught by the Stalled
        // branch above on timeInErrorMs.
        if (parseErrorDelta >= config::health::kDegradedParseErrorThreshold)
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
                                         bool poolPressure = false,
                                         uint64_t videoResetDelta = 0,
                                         uint64_t parseErrorDelta = 0,
                                         bool demuxerWedged = false) noexcept {
        if (to == StreamHealth::Failed) {
            if (resetDelta >= config::health::kFailedResetThreshold)
                return "failed: excessive audio decoder resets";
            if (videoResetDelta >= config::health::kFailedResetThreshold)
                return "failed: excessive video decoder resets";
            return "failed: audio output, decode thread, video decode thread, or ingest thread detached";
        }
        if (to == StreamHealth::Stalled) {
            if (demuxerWedged) return "stalled: demuxer wedged in error state — JS must resetStream() with a fresh EBML stream";
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
            if (parseErrorDelta >= config::health::kDegradedParseErrorThreshold)
                return "degraded: sustained demuxer parse errors";
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
