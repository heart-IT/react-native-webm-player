// Heavy-weight lifecycle implementations for PipelineOrchestrator: ingest-thread
// start/stop, demuxer self-heal, stream reset, and seek. Extracted from
// PipelineOrchestrator.h to keep the class declaration under the 400-line warn
// threshold. Each method is `inline` for ODR across iOS + Android translation
// units, mirroring the bindings/*.inl pattern.
//
// Threading: all methods called on the JS thread only.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::startIngestThread() noexcept {
    if (ingestThread_ && ingestThread_->isRunning()) return;

    size_t ringCapacity = clipIndex_.isEnabled()
        ? media::config::ingest::kRingBufferCapacityWithClip
        : media::config::ingest::kRingBufferCapacity;
    ingestRingBuffer_ = std::make_shared<media::IngestRingBuffer>(ringCapacity);
    demuxer_.setRingBuffer(ingestRingBuffer_.get());
    clipIndex_.setRingBuffer(ingestRingBuffer_.get());
    if (videoQueue_) videoQueue_->setRingBuffer(ingestRingBuffer_.get());

    auto weak = weakOwner_;

    pipeline_ = std::make_unique<media::pipeline::BroadcastPipeline>(
        media::pipeline::PipelineConfig{
            .demuxer = demuxer_,
            .clipIndex = clipIndex_,
            // Sinks run from the IngestThread which is joined in teardown
            // before orchestrator destruction — the bound `owner` is a
            // belt-and-braces lifetime guard consistent with async sites.
            .audioSink = [this, weak](const media::demux::AudioPacket* pkts, size_t count) {
                auto owner = weak.lock();
                if (!owner) return;
                for (size_t i = 0; i < count; ++i) {
                    (void)session_.pushAudioFrame(pkts[i].absOffset, pkts[i].size,
                                                  pkts[i].ptsUs, pkts[i].durationUs);
                }
            },
            .videoSink = [this, weak](const media::demux::VideoPacket* pkts, size_t count) {
                auto owner = weak.lock();
                if (!owner || !videoQueue_) return;
                for (size_t i = 0; i < count; ++i) {
                    videoQueue_->pushEncodedFrame(pkts[i].absOffset, pkts[i].size,
                                                  pkts[i].ptsUs, pkts[i].isKeyFrame);
                }
            },
            .jitterFloorSink = [this, weak](int64_t floor) {
                auto owner = weak.lock();
                if (!owner) return;
                session_.setNetworkJitterFloorUs(floor);
                if (videoQueue_) videoQueue_->setNetworkJitterFloorUs(floor);
            }
        });

    media::IngestThread::Deps deps{
        .ringBuffer = *ingestRingBuffer_,
        .pipeline = *pipeline_,
        .retainPosUpdater = [this, weak]() {
            auto owner = weak.lock();
            if (!owner) return;
            auto* ring = ingestRingBuffer_.get();
            if (!ring) return;
            long long audioFloor = session_.retentionFloorAbsOffset();
            long long videoFloor = videoQueue_
                ? videoQueue_->oldestQueuedAbsOffset() : 0;
            long long minFloor = 0;
            if (audioFloor > 0 && videoFloor > 0) minFloor = std::min(audioFloor, videoFloor);
            else if (audioFloor > 0) minFloor = audioFloor;
            else if (videoFloor > 0) minFloor = videoFloor;
            if (minFloor > 0) {
                long long base = ring->baseOffset();
                size_t wp = ring->currentWritePos();
                long long local = minFloor - base;
                if (local > 0 && static_cast<size_t>(local) <= wp) {
                    ring->setDecodeRetainPos(static_cast<size_t>(local));
                }
            }
        }
    };

    ingestThread_ = std::make_unique<media::IngestThread>(std::move(deps));
    session_.setDecodeRingBuffer(ingestRingBuffer_.get());
    if (!ingestThread_->start()) {
        MEDIA_LOG_E("PipelineOrchestrator: failed to start ingest thread");
        session_.setDecodeRingBuffer(nullptr);
        ingestThread_.reset();
        pipeline_.reset();
        ingestRingBuffer_.reset();
    }
}

inline bool PipelineOrchestrator::stopIngestThread() noexcept {
    if (!ingestThread_) return true;
    ingestThread_->stop();
    if (ingestThread_->wasDetached()) {
        (void)ingestThread_.release();
        // Move the shared_ptr into detachedIngestRings_ so the runaway thread
        // (which holds a raw IngestRingBuffer& through its IngestProcessor)
        // sees a live storage region forever. Mirrors the prior
        // unique_ptr::release() leak; ensureRunning() refuses re-init below.
        if (ingestRingBuffer_) detachedIngestRings_.push_back(std::move(ingestRingBuffer_));
        (void)pipeline_.release();
        ingestDetached_ = true;
        MEDIA_LOG_E("PipelineOrchestrator: ingest thread detached, leaking resources");
        return false;
    }
    session_.setDecodeRingBuffer(nullptr);
    demuxer_.setRingBuffer(nullptr);
    clipIndex_.setRingBuffer(nullptr);
    if (videoQueue_) videoQueue_->setRingBuffer(nullptr);
    ingestThread_.reset();
    pipeline_.reset();
    return true;
}

// Called from feedData() on the JS thread. Not under lifecycleMtx_ because
// feedData() must stay lock-free for throughput. Safe: JSI guarantees
// single-threaded JS calls, so stop() and feedData() cannot overlap.
inline void PipelineOrchestrator::performDemuxerRecovery() noexcept {
    if (!ingestThread_ || !ingestRingBuffer_) return;
    auto hdr = demuxer_.streamHeader();
    if (hdr.empty()) {
        MEDIA_LOG_W("performDemuxerRecovery: no cached stream header");
        return;
    }
    MEDIA_LOG_W("performDemuxerRecovery: self-healing with cached header (%zu bytes)", hdr.size());

    const bool paused = ingestThread_->pause();
    if (!paused) {
        MEDIA_LOG_W("performDemuxerRecovery: ingest did not pause; reset will proceed with release-ordered stores — "
                    "readers may see transient decode errors but no UAF");
    }

    session_.resetStreamState();
    if (videoQueue_) videoQueue_->reset();
    if (syncCoordinator_) syncCoordinator_->reset();
    session_.cancelPendingEncodedClear();

    ingestRingBuffer_->reset();
    (void)ingestRingBuffer_->write(hdr.data(), hdr.size());
    clipIndex_.reset();
    demuxer_.reset();
    if (pipeline_) pipeline_->onStreamReset();

    ingestThread_->syncPosition();
    ingestThread_->resume();
}

inline jsi::Value PipelineOrchestrator::resetStream(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (ingestThread_ && !ingestThread_->pause()) {
        MEDIA_LOG_W("resetStream: ingest did not pause; ring reset proceeding with release-ordered stores");
    }
    session_.resetStreamState();
    session_.resetHealthWindow();
    if (videoQueue_) videoQueue_->reset();
    if (syncCoordinator_) syncCoordinator_->reset();
    stallRecovery_.reset();
    if (ingestRingBuffer_) ingestRingBuffer_->reset();
    demuxer_.reset();
    clipIndex_.reset();
    if (pipeline_) pipeline_->onStreamReset();
    if (ingestThread_) {
        ingestThread_->syncPosition();
        ingestThread_->resume();
    }
    return jsi::Value(true);
}

inline jsi::Value PipelineOrchestrator::seekTo(double offsetSeconds) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!session_.isRunning()) return jsi::Value(false);
    if (!clipIndex_.isEnabled()) return jsi::Value(false);

    if (ingestThread_ && !ingestThread_->pause()) {
        MEDIA_LOG_W("seekTo: ingest did not pause; ring reset proceeding with release-ordered stores");
    }

    if (offsetSeconds >= 0) {
        session_.resetStreamState();
        session_.resetHealthWindow();
        if (videoQueue_) videoQueue_->reset();
        if (syncCoordinator_) syncCoordinator_->reset();
        if (ingestRingBuffer_) ingestRingBuffer_->reset();
        demuxer_.reset();
        clipIndex_.reset();
        if (pipeline_) pipeline_->onStreamReset();
        media::VideoSurfaceRegistry::instance().requestKeyFrame();
        if (ingestThread_) {
            ingestThread_->syncPosition();
            ingestThread_->resume();
        }
        return jsi::Value(true);
    }

    int64_t targetPts = session_.currentTimeUs() + static_cast<int64_t>(offsetSeconds * 1'000'000.0);
    auto clipResult = clipIndex_.extractFromPts(targetPts);
    if (!clipResult.error.empty()) {
        MEDIA_LOG_W("seekTo: %s", clipResult.error.c_str());
        if (ingestThread_) ingestThread_->resume();
        return jsi::Value(false);
    }

    session_.resetStreamState();
    session_.resetHealthWindow();
    if (videoQueue_) videoQueue_->reset();
    if (syncCoordinator_) syncCoordinator_->reset();
    session_.cancelPendingEncodedClear();

    if (ingestRingBuffer_) ingestRingBuffer_->reset();
    demuxer_.reset();

    constexpr size_t kChunkSize = 65536;
    const uint8_t* seekData = clipResult.data.data();
    size_t len = clipResult.data.size();

    if (ingestRingBuffer_ && len > ingestRingBuffer_->capacity()) {
        MEDIA_LOG_W("seekTo: clip data (%zu) exceeds ring capacity (%zu)",
                    len, ingestRingBuffer_->capacity());
        if (ingestThread_) ingestThread_->resume();
        return jsi::Value(false);
    }

    while (len > 0) {
        size_t chunk = std::min(len, kChunkSize);
        if (ingestRingBuffer_ && !ingestRingBuffer_->write(seekData, chunk)) {
            MEDIA_LOG_W("seekTo: ring buffer full during re-feed");
            break;
        }
        // pipeline_ is constructed in startIngestThread() before ingestThread_->start().
        // seekTo() early-returns when !isRunning(), so reaching here implies pipeline_ is non-null.
        pipeline_->onNewData(chunk);
        seekData += chunk;
        len -= chunk;
    }

    if (ingestRingBuffer_ && ingestRingBuffer_->currentWritePos() > 0) {
        ingestRingBuffer_->setDecodeRetainPos(1);
    }

    clipIndex_.reset();
    demuxer_.reset();
    if (pipeline_) pipeline_->onStreamReset();
    media::VideoSurfaceRegistry::instance().requestKeyFrame();
    if (ingestThread_) {
        ingestThread_->syncPosition();
        ingestThread_->resume();
    }
    MEDIA_LOG_I("seekTo: completed seek to %.1fs", offsetSeconds);
    return jsi::Value(true);
}

}  // namespace mediamodule
