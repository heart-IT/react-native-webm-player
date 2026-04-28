// Ingest-thread integration point for WebM broadcast playback.
//
// Owns the demux loop: feeds the WebmDemuxer, dispatches packets to audio/video
// sinks, maintains ClipIndex, and propagates network jitter floor. Previously
// split across Sink<T>, DemuxStage, and BroadcastPipeline — collapsed into a
// single class since the graph has exactly one shape (one producer, one audio
// sink, one video sink) and the descriptor indirection was pure overhead.
//
// Threading: onNewData() runs on the ingest thread. All sinks run on that
// thread too. Outputs are the sinks' responsibility to make thread-safe.
#pragma once

#include <functional>
#include "ClipIndex.h"
#include "MediaLog.h"
#include "demux/WebmDemuxer.h"
#include "video/VideoSurfaceRegistry.h"

namespace media::pipeline {

struct PipelineResult {
    bool hadData = false;
    bool recoveryNeeded = false;
};

// Sinks receive a contiguous batch of raw demux packets for a single feedData()
// cycle. Pointers are valid only for the duration of the sink call.
using AudioSink = std::function<void(const demux::AudioPacket*, size_t)>;
using VideoSink = std::function<void(const demux::VideoPacket*, size_t)>;
using JitterFloorSink = std::function<void(int64_t)>;

struct PipelineConfig {
    demux::WebmDemuxer& demuxer;
    ClipIndex& clipIndex;
    AudioSink audioSink;
    VideoSink videoSink;
    JitterFloorSink jitterFloorSink;
};

class BroadcastPipeline {
public:
    explicit BroadcastPipeline(PipelineConfig config) noexcept
        : demuxer_(config.demuxer)
        , clipIndex_(config.clipIndex)
        , audioSink_(std::move(config.audioSink))
        , videoSink_(std::move(config.videoSink))
        , jitterFloorSink_(std::move(config.jitterFloorSink)) {}

    PipelineResult onNewData(size_t newBytes) noexcept {
        PipelineResult result{};
        if (newBytes == 0) return result;

        const auto& demuxResult = demuxer_.feedData(nullptr, newBytes);
        result.hadData = true;

        // Dispatch directly — no descriptor conversion.
        if (!demuxResult.audioPackets.empty() && audioSink_) {
            audioSink_(demuxResult.audioPackets.data(),
                       demuxResult.audioPackets.size());
        }
        if (!demuxResult.videoPackets.empty() && videoSink_) {
            videoSink_(demuxResult.videoPackets.data(),
                       demuxResult.videoPackets.size());
        }

        updateClipMetadata(demuxResult);
        propagateJitterFloor();

        if (!demuxResult.error.empty()) {
            VideoSurfaceRegistry::instance().requestKeyFrame();
            if (demuxer_.parseState() == demux::ParseState::Error) {
                if (demuxer_.hasStreamHeader()) {
                    MEDIA_LOG_W("BroadcastPipeline: demux error in Error state — triggering recovery");
                    result.recoveryNeeded = true;
                } else if (!wedgedNoHeaderLogged_) {
                    // Wedged: never parsed a stream header, so cached-header self-heal
                    // cannot run. JS must resetStream() with a fresh EBML-prefixed
                    // stream. Log once per wedge to avoid spam; the latch is cleared
                    // in onStreamReset().
                    MEDIA_LOG_E("BroadcastPipeline: demuxer wedged in Error without a cached "
                                "stream header — JS must resetStream() with an EBML-prefixed stream");
                    wedgedNoHeaderLogged_ = true;
                }
            } else {
                MEDIA_LOG_W("BroadcastPipeline: demux error");
            }
        }

        return result;
    }

    // Reset the stream-header latch so the next stream re-propagates to ClipIndex.
    // Called by the orchestrator alongside demuxer_.reset() / clipIndex_.reset().
    void onStreamReset() noexcept {
        streamHeaderLatched_ = false;
        wedgedNoHeaderLogged_ = false;
    }

private:
    void updateClipMetadata(const demux::DemuxResult& result) noexcept {
        if (!streamHeaderLatched_ && demuxer_.hasStreamHeader()) {
            auto hdr = demuxer_.streamHeader();
            clipIndex_.setStreamHeader(hdr.data(), hdr.size());
            streamHeaderLatched_ = true;
        }
        for (size_t i = 0; i < result.newClusterPositions.size(); ++i) {
            clipIndex_.onNewCluster(result.newClusterPositions[i]);
        }
        for (const auto& pkt : result.videoPackets) {
            if (pkt.isKeyFrame) clipIndex_.onKeyFrame(pkt.ptsUs);
            clipIndex_.onBlockPts(pkt.ptsUs);
        }
        for (const auto& pkt : result.audioPackets) {
            clipIndex_.onBlockPts(pkt.ptsUs);
        }
        clipIndex_.updateRetainPosition();
    }

    void propagateJitterFloor() noexcept {
        int64_t feedJitter = demuxer_.feedJitterUs();
        if (feedJitter > 0 && jitterFloorSink_) {
            int64_t floor = (feedJitter * 5) >> 1;
            jitterFloorSink_(floor);
        }
    }

    demux::WebmDemuxer& demuxer_;
    ClipIndex& clipIndex_;
    AudioSink audioSink_;
    VideoSink videoSink_;
    JitterFloorSink jitterFloorSink_;
    bool streamHeaderLatched_ = false;  // ingest-thread only; single-consumer
    bool wedgedNoHeaderLogged_ = false; // ingest-thread only; one-shot WARN latch
};

}  // namespace media::pipeline
