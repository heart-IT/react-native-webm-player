// Implementation of MediaSessionBase::metrics() — assembles a SessionMetrics
// snapshot from every subsystem (audio channel, decode thread, mixer, jitter
// estimator, audio output bridge, audio session manager).
//
// Include-only-from: MediaSessionBase.h. Defined out-of-class as a template
// member; included after the class body.
#pragma once

namespace media {

template<typename PlatformTraits>
[[nodiscard]] SessionMetrics MediaSessionBase<PlatformTraits>::metrics() noexcept {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    SessionMetrics m{};
    m.uptimeUs = uptimeUs();

    const Bridge* audio = audioOutput_.get();

    m.totalSamplesOutput = audio ? static_cast<uint64_t>(audio->framesWritten()) : 0;

    m.decodeThreadResponsive = decodeThread_.isResponsive();
    m.decodeThreadDetached = decodeThread_.wasDetached();
    m.watchdogTripped = decodeThread_.wasWatchdogTripped();
    m.watchdogTripCount = decodeThread_.watchdogTripCount();
    m.timeSinceHeartbeatUs = decodeThread_.timeSinceLastHeartbeatUs();

    m.grantedLatencyMode = audio ? audio->grantedLatencyMode() : LatencyMode::Unknown;

    if (audio_.isActive()) {
        const auto& sm = audio_.metrics();
        m.audioFramesReceived = sm.framesReceived.load(std::memory_order_relaxed);
        m.framesDropped = sm.framesDropped.load(std::memory_order_relaxed);
        m.oversizedFrameDrops = sm.oversizedFrameDrops.load(std::memory_order_relaxed);
        m.bufferFullDrops = sm.bufferFullDrops.load(std::memory_order_relaxed);
        m.encodedPoolExhaustionDrops = sm.encodedPoolExhaustionDrops.load(std::memory_order_relaxed);
        m.encodedPushFailDrops = sm.encodedPushFailDrops.load(std::memory_order_relaxed);
        m.decodedPushFailDrops = sm.decodedPushFailDrops.load(std::memory_order_relaxed);
        m.framesDrained = sm.framesDrained.load(std::memory_order_relaxed);
        m.decodeErrors = sm.decodeErrors.load(std::memory_order_relaxed);
        m.decoderResets = sm.decoderResets.load(std::memory_order_relaxed);
        m.underruns = sm.underruns.load(std::memory_order_relaxed);
        m.silenceCallbacks = sm.silenceCallbacks.load(std::memory_order_relaxed);
        m.ptsDiscontinuities = sm.ptsDiscontinuities.load(std::memory_order_relaxed);
        m.plcFrames = sm.plcFrames.load(std::memory_order_relaxed);
        m.fecFrames = sm.fecFrames.load(std::memory_order_relaxed);
        m.silenceSkipFrames = sm.silenceSkipFrames.load(std::memory_order_relaxed);
        m.peakConsecutivePLC = sm.peakConsecutivePLC.load(std::memory_order_relaxed);
        m.currentConsecutivePLC = audio_.currentConsecutivePLC();
        m.muted = muted_;

        m.fastPathSwitches = sm.fastPathSwitches.load(std::memory_order_relaxed);
        m.catchupDeadZoneSnaps = audio_.catchupDeadZoneSnaps();

        m.maxInterFrameGapUs = sm.maxInterFrameGapUs.load(std::memory_order_relaxed);
        m.gapsOver50ms = sm.gapsOver50ms.load(std::memory_order_relaxed);
        m.gapsOver100ms = sm.gapsOver100ms.load(std::memory_order_relaxed);
        m.gapsOver500ms = sm.gapsOver500ms.load(std::memory_order_relaxed);

        m.audioDecodeLatencyUs = sm.decodeLatencyUs.load(std::memory_order_relaxed);
        m.audioLastDecodeError = sm.lastDecodeError.load(std::memory_order_relaxed);

        m.arrivalConfidence = jitter_.arrivalConfidence();
        m.speculativeMode = m.arrivalConfidence >= media::config::speculative::kConfidenceThreshold;

        m.driftPpm = audio_.driftPpm();
        m.driftCompensationActive = audio_.isDriftCompensationActive();
        m.driftCurrentRatio = audio_.driftCurrentRatio();
        m.catchupCurrentRatio = audio_.catchupCurrentRatio();

        m.jitterUs = jitter_.jitterUs();
        m.jitterTrendUs = jitter_.jitterTrendUs();
        m.bufferTargetUs = jitter_.bufferTargetUs();

        m.decodedPoolUnderPressure = audio_.isDecodedPoolUnderPressure();
        m.encodedPoolUnderPressure = audio_.isEncodedPoolUnderPressure();

        m.audioStreamState = static_cast<uint8_t>(audio_.state());
        m.currentGain = audio_.targetGain();
        m.bufferedDurationUs = audio_.bufferedDurationUs();
        m.decodedDurationUs = audio_.decodedDurationUs();
    }

    const auto& meter = mixer_.levelMeter();
    m.peakLevel = meter.peakLevel();
    m.rmsLevel = meter.rmsLevel();
    m.peakDbfs = AudioLevelMeter::toDbfs(m.peakLevel);
    m.rmsDbfs = AudioLevelMeter::toDbfs(m.rmsLevel);
    m.clipCount = meter.clipCount();

    if (audio) {
        m.audioRestartCount = audio->restartCount();
        m.audioLastError = audio->lastError();
        m.actualSampleRate = audio->actualSampleRate();
        m.sampleRateValid = audio->isSampleRateValid();
        m.audioOutputLatencyUs = audio->totalLatencyUs();
        m.callbackJitterUs = audio->callbackJitterUs();
        m.audioOutputRunning = audio->isRunning();
    }
    m.interrupted = !PlatformTraits::managerInstance().canPlayAudio();

    m.currentRoute = PlatformTraits::managerInstance().currentRoute();
    m.routeChangeCount = PlatformTraits::managerInstance().routeChangeCount();

    return m;
}

}  // namespace media
