// Aggregates metrics from all pipeline subsystems and serializes to JSI.
// Extracted from PipelineOrchestrator to isolate the ~200 lines of JSI
// property serialization from pipeline control logic.
#pragma once

#include <jsi/jsi.h>
#include <atomic>
#include <cstdint>
#include "MediaLog.h"
#include "MediaTime.h"
#include "StallRecoveryController.h"
#include "IngestRingBuffer.h"
#include "IngestThread.h"
#include "ClipIndex.h"
#include "demux/WebmDemuxer.h"
#include "video/VideoFrameQueue.h"
#include "common/AVSyncCoordinator.h"

// Forward: MediaSession is platform-specific (included by caller before this header)

namespace mediamodule {

namespace jsi = facebook::jsi;

// Non-owning pointers to all metric sources. All must outlive the collector.
// Session is media::MediaSession (platform-specific concrete type). Previously
// a template parameter; collapsed since there is only one instantiation per
// compile unit.
struct MetricsContext {
    media::MediaSession& session;
    media::demux::WebmDemuxer& demuxer;
    media::ClipIndex& clipIndex;
    media::StallRecoveryController& stallRecovery;
    media::VideoFrameQueue* videoQueue;
    media::AVSyncCoordinator* syncCoordinator;
    media::IngestRingBuffer* ringBuffer;
    media::IngestThread* ingestThread;
    std::atomic<uint64_t>& ingestBytesDropped;
    std::atomic<bool>& ingestDetached;
};

inline jsi::Value collectMetrics(jsi::Runtime& rt, const MetricsContext& ctx) {
    media::SessionMetrics metrics;
    ctx.session.getMetrics(metrics);

    jsi::Object result(rt);

    // Mirrors getPlaybackState() so JS sees a consistent state alongside the metrics snapshot.
    {
        using PB = media::PlaybackState;
        PB pbState = PB::Idle;
        if (ctx.session.isRunning()) {
            if (ctx.session.isPaused()) {
                pbState = PB::Paused;
            } else {
                auto stallState = ctx.stallRecovery.state();
                if (stallState == media::StallState::Failed) pbState = PB::Failed;
                else if (stallState == media::StallState::Stalled || stallState == media::StallState::Recovering) pbState = PB::Stalled;
                else if (metrics.audioStreamState == static_cast<uint8_t>(media::StreamState::Buffering) ||
                         metrics.audioStreamState == static_cast<uint8_t>(media::StreamState::Underrun)) pbState = PB::Buffering;
                else pbState = PB::Playing;
            }
        }
        result.setProperty(rt, "playbackState", static_cast<int>(pbState));
    }

    // Health
    jsi::Object health(rt);
    health.setProperty(rt, "running", ctx.session.isRunning());
    health.setProperty(rt, "responsive", metrics.decodeThreadResponsive);
    health.setProperty(rt, "detached", metrics.decodeThreadDetached);
    health.setProperty(rt, "watchdogTripped", metrics.watchdogTripped);
    health.setProperty(rt, "watchdogTripCount", static_cast<int>(metrics.watchdogTripCount));
    health.setProperty(rt, "timeSinceHeartbeatMs", static_cast<double>(metrics.timeSinceHeartbeatUs / 1000));
    result.setProperty(rt, "health", std::move(health));

    // Quality
    jsi::Object quality(rt);
    quality.setProperty(rt, "framesReceived", static_cast<double>(metrics.audioFramesReceived));
    quality.setProperty(rt, "underruns", static_cast<double>(metrics.underruns));
    quality.setProperty(rt, "silenceCallbacks", static_cast<double>(metrics.silenceCallbacks));
    quality.setProperty(rt, "framesDropped", static_cast<double>(metrics.framesDropped));
    quality.setProperty(rt, "oversizedFrameDrops", static_cast<double>(metrics.oversizedFrameDrops));
    quality.setProperty(rt, "bufferFullDrops", static_cast<double>(metrics.bufferFullDrops));
    quality.setProperty(rt, "encodedPoolExhaustionDrops", static_cast<double>(metrics.encodedPoolExhaustionDrops));
    quality.setProperty(rt, "encodedPushFailDrops", static_cast<double>(metrics.encodedPushFailDrops));
    quality.setProperty(rt, "decodedPushFailDrops", static_cast<double>(metrics.decodedPushFailDrops));
    quality.setProperty(rt, "framesDrained", static_cast<double>(metrics.framesDrained));
    quality.setProperty(rt, "decodeErrors", static_cast<double>(metrics.decodeErrors));
    quality.setProperty(rt, "decoderResets", static_cast<double>(metrics.decoderResets));
    quality.setProperty(rt, "ptsDiscontinuities", static_cast<double>(metrics.ptsDiscontinuities));
    quality.setProperty(rt, "fastPathSwitches", static_cast<double>(metrics.fastPathSwitches));
    quality.setProperty(rt, "catchupDeadZoneSnaps", static_cast<double>(metrics.catchupDeadZoneSnaps));
    quality.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(metrics.maxInterFrameGapUs) / 1000.0);
    quality.setProperty(rt, "gapsOver50ms", static_cast<double>(metrics.gapsOver50ms));
    quality.setProperty(rt, "gapsOver100ms", static_cast<double>(metrics.gapsOver100ms));
    quality.setProperty(rt, "gapsOver500ms", static_cast<double>(metrics.gapsOver500ms));
    quality.setProperty(rt, "plcFrames", static_cast<double>(metrics.plcFrames));
    quality.setProperty(rt, "fecFrames", static_cast<double>(metrics.fecFrames));
    quality.setProperty(rt, "silenceSkipFrames", static_cast<double>(metrics.silenceSkipFrames));
    quality.setProperty(rt, "peakConsecutivePLC", static_cast<int>(metrics.peakConsecutivePLC));
    quality.setProperty(rt, "currentConsecutivePLC", static_cast<int>(metrics.currentConsecutivePLC));
    quality.setProperty(rt, "audioDecodeLatencyUs", static_cast<double>(metrics.audioDecodeLatencyUs));
    quality.setProperty(rt, "audioLastDecodeError", static_cast<int>(metrics.audioLastDecodeError));
    result.setProperty(rt, "quality", std::move(quality));

    // Session
    jsi::Object session(rt);
    session.setProperty(rt, "uptimeMs", static_cast<double>(metrics.uptimeUs / 1000));
    session.setProperty(rt, "samplesOutput", static_cast<double>(metrics.totalSamplesOutput));
    session.setProperty(rt, "playbackRate", static_cast<double>(ctx.session.playbackRate()));
    session.setProperty(rt, "streamStatus", static_cast<int>(ctx.session.streamStatus()));
    result.setProperty(rt, "session", std::move(session));

    // Latency
    jsi::Object latency(rt);
    latency.setProperty(rt, "mode", media::latencyModeToString(metrics.grantedLatencyMode));
    latency.setProperty(rt, "isLowestLatency", metrics.grantedLatencyMode == media::LatencyMode::LowLatency);
    latency.setProperty(rt, "endToEndUs", static_cast<double>(metrics.bufferedDurationUs + metrics.audioOutputLatencyUs));
    result.setProperty(rt, "latency", std::move(latency));

    // Pools
    jsi::Object pools(rt);
    pools.setProperty(rt, "decodedUnderPressure", metrics.decodedPoolUnderPressure);
    pools.setProperty(rt, "encodedUnderPressure", metrics.encodedPoolUnderPressure);
    result.setProperty(rt, "pools", std::move(pools));

    // Audio output
    jsi::Object audioOutput(rt);
    audioOutput.setProperty(rt, "restartCount", static_cast<int>(metrics.audioRestartCount));
    audioOutput.setProperty(rt, "lastError", metrics.audioLastError);
    audioOutput.setProperty(rt, "actualSampleRate", metrics.actualSampleRate);
    audioOutput.setProperty(rt, "latencyUs", static_cast<double>(metrics.audioOutputLatencyUs));
    audioOutput.setProperty(rt, "callbackJitterUs", static_cast<double>(metrics.callbackJitterUs));
    audioOutput.setProperty(rt, "running", metrics.audioOutputRunning);
    audioOutput.setProperty(rt, "interrupted", metrics.interrupted);
    audioOutput.setProperty(rt, "sampleRateValid", metrics.sampleRateValid);
    result.setProperty(rt, "audioOutput", std::move(audioOutput));

    // Bluetooth
    jsi::Object bluetooth(rt);
    bluetooth.setProperty(rt, "route", media::audioRouteToString(metrics.currentRoute));
    bluetooth.setProperty(rt, "isA2dp", metrics.currentRoute == media::AudioRoute::BluetoothA2dp);
    bluetooth.setProperty(rt, "routeChangeCount", static_cast<double>(metrics.routeChangeCount));
    result.setProperty(rt, "bluetooth", std::move(bluetooth));

    // Jitter
    jsi::Object jitterObj(rt);
    jitterObj.setProperty(rt, "jitterUs", static_cast<double>(metrics.jitterUs));
    jitterObj.setProperty(rt, "jitterTrendUs", static_cast<double>(metrics.jitterTrendUs));
    jitterObj.setProperty(rt, "bufferTargetUs", static_cast<double>(metrics.bufferTargetUs));
    jitterObj.setProperty(rt, "arrivalConfidence", static_cast<double>(metrics.arrivalConfidence));
    jitterObj.setProperty(rt, "speculativeMode", jsi::Value(metrics.speculativeMode));
    result.setProperty(rt, "jitter", std::move(jitterObj));

    // Drift
    jsi::Object drift(rt);
    drift.setProperty(rt, "driftPpm", metrics.driftPpm);
    drift.setProperty(rt, "active", metrics.driftCompensationActive);
    drift.setProperty(rt, "currentRatio", static_cast<double>(metrics.driftCurrentRatio));
    drift.setProperty(rt, "catchupRatio", static_cast<double>(metrics.catchupCurrentRatio));
    result.setProperty(rt, "drift", std::move(drift));

    // Pipeline
    jsi::Object pipeline(rt);
    pipeline.setProperty(rt, "audioStreamState", static_cast<int>(metrics.audioStreamState));
    pipeline.setProperty(rt, "currentGain", static_cast<double>(metrics.currentGain));
    pipeline.setProperty(rt, "muted", metrics.muted);
    pipeline.setProperty(rt, "bufferedDurationUs", static_cast<double>(metrics.bufferedDurationUs));
    pipeline.setProperty(rt, "decodedDurationUs", static_cast<double>(metrics.decodedDurationUs));
    result.setProperty(rt, "pipeline", std::move(pipeline));

    // Levels
    jsi::Object levels(rt);
    levels.setProperty(rt, "peakLevel", static_cast<double>(metrics.peakLevel));
    levels.setProperty(rt, "rmsLevel", static_cast<double>(metrics.rmsLevel));
    levels.setProperty(rt, "peakDbfs", static_cast<double>(metrics.peakDbfs));
    levels.setProperty(rt, "rmsDbfs", static_cast<double>(metrics.rmsDbfs));
    levels.setProperty(rt, "clipCount", static_cast<double>(metrics.clipCount));
    result.setProperty(rt, "levels", std::move(levels));

    // Stall
    jsi::Object stall(rt);
    auto sm = ctx.stallRecovery.metrics();
    stall.setProperty(rt, "state", stallStateToString(sm.currentState));
    stall.setProperty(rt, "stallCount", static_cast<int>(sm.stallCount));
    stall.setProperty(rt, "recoveryCount", static_cast<int>(sm.recoveryCount));
    stall.setProperty(rt, "keyFrameRequests", static_cast<int>(sm.keyFrameRequests));
    stall.setProperty(rt, "totalStallMs", static_cast<double>(sm.totalStallDurationUs) / 1000.0);
    stall.setProperty(rt, "lastStallMs", static_cast<double>(sm.lastStallDurationUs) / 1000.0);
    stall.setProperty(rt, "longestStallMs", static_cast<double>(sm.longestStallUs) / 1000.0);
    stall.setProperty(rt, "lastRecoveryMs", static_cast<double>(sm.lastRecoveryDurationUs) / 1000.0);
    stall.setProperty(rt, "longestRecoveryMs", static_cast<double>(sm.longestRecoveryUs) / 1000.0);
    result.setProperty(rt, "stall", std::move(stall));

    // Demux
    jsi::Object demux(rt);
    auto dm = ctx.demuxer.demuxerMetrics();
    demux.setProperty(rt, "totalBytesFed", static_cast<double>(dm.totalBytesFed));
    demux.setProperty(rt, "feedDataCalls", static_cast<double>(dm.feedDataCalls));
    demux.setProperty(rt, "audioPacketsEmitted", static_cast<double>(dm.audioPacketsEmitted));
    demux.setProperty(rt, "videoPacketsEmitted", static_cast<double>(dm.videoPacketsEmitted));
    demux.setProperty(rt, "overflowCount", static_cast<double>(dm.overflowCount));
    demux.setProperty(rt, "partialDropCount", static_cast<double>(dm.partialDropCount));
    demux.setProperty(rt, "oversizedFrameDrops", static_cast<double>(dm.oversizedFrameDrops));
    demux.setProperty(rt, "packetCapDrops", static_cast<double>(dm.packetCapDrops));
    demux.setProperty(rt, "appendBackpressureDrops", static_cast<double>(dm.appendBackpressureDrops));
    demux.setProperty(rt, "blockStallCount", static_cast<double>(dm.blockStallCount));
    demux.setProperty(rt, "parseErrorCount", static_cast<double>(dm.parseErrorCount));
    demux.setProperty(rt, "cumulativeParseErrorCount", static_cast<double>(dm.cumulativeParseErrorCount));
    demux.setProperty(rt, "sessionResetCount", static_cast<double>(dm.sessionResetCount));
    demux.setProperty(rt, "timeInErrorMs", static_cast<double>(dm.timeInErrorMs));
    demux.setProperty(rt, "feedJitterUs", static_cast<double>(dm.feedJitterUs));
    demux.setProperty(rt, "feedDataLatencyUs", static_cast<double>(dm.feedDataLatencyUs));
    demux.setProperty(rt, "bufferBytes", static_cast<double>(dm.bufferBytes));
    demux.setProperty(rt, "parseState", static_cast<int>(dm.parseState));
    demux.setProperty(rt, "streamHeaderAvailable", jsi::Value(dm.streamHeaderAvailable));
    demux.setProperty(rt, "ingestBytesDropped", static_cast<double>(ctx.ingestBytesDropped.load(std::memory_order_relaxed)));
    demux.setProperty(rt, "ingestThreadDetached", jsi::Value(ctx.ingestDetached.load(std::memory_order_relaxed)));
    if (ctx.ringBuffer) {
        demux.setProperty(rt, "ingestRingUsed", static_cast<double>(ctx.ringBuffer->liveBytes()));
        demux.setProperty(rt, "ingestRingCapacity", static_cast<double>(ctx.ringBuffer->capacity()));
        demux.setProperty(rt, "ingestRingWriteRejects", static_cast<double>(ctx.ringBuffer->writeRejects()));
        demux.setProperty(rt, "ingestRingCompactionStalls", static_cast<double>(ctx.ringBuffer->compactionStalls()));
    } else {
        demux.setProperty(rt, "ingestRingUsed", 0.0);
        demux.setProperty(rt, "ingestRingCapacity", 0.0);
        demux.setProperty(rt, "ingestRingWriteRejects", 0.0);
        demux.setProperty(rt, "ingestRingCompactionStalls", 0.0);
    }
    if (ctx.ingestThread) {
        demux.setProperty(rt, "ingestThreadResponsive", jsi::Value(ctx.ingestThread->isResponsive()));
        demux.setProperty(rt, "timeSinceIngestHeartbeatMs",
                          static_cast<double>(ctx.ingestThread->timeSinceLastHeartbeatUs() / 1000));
    } else {
        demux.setProperty(rt, "ingestThreadResponsive", jsi::Value(true));
        demux.setProperty(rt, "timeSinceIngestHeartbeatMs", 0.0);
    }
    result.setProperty(rt, "demux", std::move(demux));

    // Video
    jsi::Object video(rt);
    if (ctx.videoQueue) {
        const auto& vm = ctx.videoQueue->metrics();
        video.setProperty(rt, "framesReceived", static_cast<double>(vm.framesReceived.load(std::memory_order_relaxed)));
        video.setProperty(rt, "framesDecoded", static_cast<double>(vm.framesDecoded.load(std::memory_order_relaxed)));
        video.setProperty(rt, "framesDropped", static_cast<double>(vm.framesDropped.load(std::memory_order_relaxed)));
        video.setProperty(rt, "keyFrameGatedDrops", static_cast<double>(vm.keyFrameGatedDrops.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decodeErrors", static_cast<double>(vm.decodeErrors.load(std::memory_order_relaxed)));
        // Decay currentFps to zero when the decode thread hasn't ticked the FpsCounter
        // for >1s — without this, a full freeze (HW decoder hung, surface lost mid-stream)
        // leaves the metric pinned at 30, blinding triage tree §4 (sustained frame loss).
        {
            int reportedFps = vm.currentFps.load(std::memory_order_relaxed);
            int64_t lastDecode = vm.lastDecodeTimeUs.load(std::memory_order_relaxed);
            if (lastDecode > 0 && (media::nowUs() - lastDecode) > 1'000'000) {
                reportedFps = 0;
            }
            video.setProperty(rt, "currentFps", static_cast<double>(reportedFps));
        }
        video.setProperty(rt, "width", static_cast<int>(vm.width.load(std::memory_order_relaxed)));
        video.setProperty(rt, "height", static_cast<int>(vm.height.load(std::memory_order_relaxed)));
        video.setProperty(rt, "jitterUs", static_cast<double>(vm.jitterUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "bufferTargetUs", static_cast<double>(vm.bufferTargetUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "lateFrames", static_cast<double>(vm.lateFrames.load(std::memory_order_relaxed)));
        video.setProperty(rt, "skippedFrames", static_cast<double>(vm.skippedFrames.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decoderResets", static_cast<double>(vm.decoderResets.load(std::memory_order_relaxed)));
        video.setProperty(rt, "surfaceLostCount", static_cast<double>(vm.surfaceLostCount.load(std::memory_order_relaxed)));
        video.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(vm.maxInterFrameGapUs.load(std::memory_order_relaxed)) / 1000.0);
        video.setProperty(rt, "gapsOver50ms", static_cast<double>(vm.gapsOver50ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "gapsOver100ms", static_cast<double>(vm.gapsOver100ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "gapsOver500ms", static_cast<double>(vm.gapsOver500ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "driftPpm", static_cast<double>(vm.driftPpm.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decodeLatencyUs", static_cast<double>(vm.decodeLatencyUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "lastDecodeTimeUs", static_cast<double>(vm.lastDecodeTimeUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "lastDecodeError", static_cast<int>(vm.lastDecodeError.load(std::memory_order_relaxed)));
        video.setProperty(rt, "queueDepth", static_cast<int>(ctx.videoQueue->pendingFrames()));
        video.setProperty(rt, "needsKeyFrame", ctx.videoQueue->isAwaitingKeyFrame());
        video.setProperty(rt, "keyFrameRequests", static_cast<double>(vm.keyFrameRequests.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decoderState", static_cast<int>(vm.decoderState.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decodeThreadResponsive", vm.decodeThreadResponsive.load(std::memory_order_relaxed));
        int64_t vhb = vm.lastHeartbeatUs.load(std::memory_order_relaxed);
        video.setProperty(rt, "timeSinceVideoHeartbeatMs", vhb > 0 ? static_cast<double>(media::nowUs() - vhb) / 1000.0 : 0.0);
    } else {
        video.setProperty(rt, "framesReceived", 0.0); video.setProperty(rt, "framesDecoded", 0.0);
        video.setProperty(rt, "framesDropped", 0.0); video.setProperty(rt, "keyFrameGatedDrops", 0.0); video.setProperty(rt, "decodeErrors", 0.0);
        video.setProperty(rt, "currentFps", 0.0); video.setProperty(rt, "width", 0);
        video.setProperty(rt, "height", 0); video.setProperty(rt, "jitterUs", 0.0);
        video.setProperty(rt, "bufferTargetUs", 0.0); video.setProperty(rt, "lateFrames", 0.0);
        video.setProperty(rt, "skippedFrames", 0.0); video.setProperty(rt, "decoderResets", 0.0);
        video.setProperty(rt, "surfaceLostCount", 0.0); video.setProperty(rt, "maxInterFrameGapMs", 0.0);
        video.setProperty(rt, "gapsOver50ms", 0.0); video.setProperty(rt, "gapsOver100ms", 0.0);
        video.setProperty(rt, "gapsOver500ms", 0.0); video.setProperty(rt, "driftPpm", 0.0);
        video.setProperty(rt, "decodeLatencyUs", 0.0); video.setProperty(rt, "lastDecodeTimeUs", 0.0);
        video.setProperty(rt, "lastDecodeError", 0); video.setProperty(rt, "queueDepth", 0); video.setProperty(rt, "needsKeyFrame", false); video.setProperty(rt, "keyFrameRequests", 0.0); video.setProperty(rt, "decoderState", 0);
        video.setProperty(rt, "decodeThreadResponsive", false); video.setProperty(rt, "timeSinceVideoHeartbeatMs", 0.0);
    }
    video.setProperty(rt, "avSyncOffsetUs", ctx.syncCoordinator
        ? static_cast<double>(ctx.syncCoordinator->currentOffsetUs()) : 0.0);
    video.setProperty(rt, "peakAbsAvSyncOffsetUs", ctx.syncCoordinator
        ? static_cast<double>(ctx.syncCoordinator->peakAbsOffsetUs()) : 0.0);
    video.setProperty(rt, "avSyncExceedCount", ctx.syncCoordinator
        ? static_cast<double>(ctx.syncCoordinator->syncExceedCount()) : 0.0);
    result.setProperty(rt, "video", std::move(video));

    // Clip
    jsi::Object clip(rt);
    auto cm = ctx.clipIndex.clipMetrics();
    clip.setProperty(rt, "enabled", cm.enabled);
    clip.setProperty(rt, "bufferCapacity", static_cast<double>(cm.bufferCapacity));
    clip.setProperty(rt, "bytesUsed", static_cast<double>(cm.bytesUsed));
    clip.setProperty(rt, "clusterCount", static_cast<double>(cm.clusterCount));
    clip.setProperty(rt, "availableSeconds", static_cast<double>(cm.availableSeconds));
    result.setProperty(rt, "clip", std::move(clip));

    return result;
}

}  // namespace mediamodule
