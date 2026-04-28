// Aggregated metrics snapshot returned by MediaSessionBase::metrics().
// Plain-data struct — every field is set once per snapshot from the audio
// channel, decode thread, mixer, jitter estimator, and platform audio bridge.
#pragma once

#include <cstdint>
#include "AudioRouteTypes.h"
#include "MediaConfig.h"
#include "../playback/MediaTypes.h"

namespace media {

struct SessionMetrics {
    uint64_t totalSamplesOutput = 0;
    int64_t uptimeUs = 0;

    bool decodeThreadResponsive = true;
    bool decodeThreadDetached = false;
    bool watchdogTripped = false;
    uint32_t watchdogTripCount = 0;
    int64_t timeSinceHeartbeatUs = 0;

    // Latency mode tracking
    LatencyMode grantedLatencyMode = LatencyMode::Unknown;

    // Audio output health metrics
    uint32_t audioRestartCount = 0;
    int audioLastError = 0;
    int32_t actualSampleRate = 0;
    bool sampleRateValid = true;
    int64_t audioOutputLatencyUs = 0;
    int64_t callbackJitterUs = 0;
    bool audioOutputRunning = false;
    bool interrupted = false;

    // Pool pressure indicators
    bool decodedPoolUnderPressure = false;
    bool encodedPoolUnderPressure = false;

    // Bluetooth route tracking
    AudioRoute currentRoute = AudioRoute::Unknown;
    uint64_t routeChangeCount = 0;

    // Audio channel metrics
    uint64_t audioFramesReceived = 0;
    uint64_t framesDropped = 0;
    uint64_t oversizedFrameDrops = 0;
    uint64_t bufferFullDrops = 0;
    uint64_t encodedPoolExhaustionDrops = 0;
    uint64_t encodedPushFailDrops = 0;
    uint64_t decodedPushFailDrops = 0;
    uint64_t framesDrained = 0;
    uint64_t decodeErrors = 0;
    uint64_t decoderResets = 0;
    uint64_t underruns = 0;             // Playing→Underrun edge transitions (state changes)
    uint64_t silenceCallbacks = 0;      // per-callback events of zero-sample output
    uint64_t ptsDiscontinuities = 0;
    uint64_t plcFrames = 0;
    uint64_t fecFrames = 0;
    uint64_t silenceSkipFrames = 0;
    uint32_t peakConsecutivePLC = 0;
    uint32_t currentConsecutivePLC = 0;
    bool muted = false;

    // Clock drift compensation metrics
    int32_t driftPpm = 0;
    bool driftCompensationActive = false;
    float driftCurrentRatio = 1.0f;
    float catchupCurrentRatio = 1.0f;

    // Bluetooth crackling diagnostics
    uint64_t fastPathSwitches = 0;
    uint64_t catchupDeadZoneSnaps = 0;

    // Inter-frame delivery gap diagnostics
    int64_t maxInterFrameGapUs = 0;
    uint64_t gapsOver50ms = 0;
    uint64_t gapsOver100ms = 0;
    uint64_t gapsOver500ms = 0;

    // Jitter buffer metrics
    int64_t jitterUs = 0;
    int64_t jitterTrendUs = 0;
    int64_t bufferTargetUs = 0;

    // Audio decode performance
    int64_t audioDecodeLatencyUs = 0;
    int32_t audioLastDecodeError = 0;

    // Speculative playback
    float arrivalConfidence = 0.0f;
    bool speculativeMode = false;

    // Triage metrics: pipeline state observability
    uint8_t audioStreamState = 0;
    float currentGain = 1.0f;
    int64_t bufferedDurationUs = 0;
    int64_t decodedDurationUs = 0;

    // Audio level metering (post-gain, from audio callback)
    float peakLevel = 0.0f;
    float rmsLevel = 0.0f;
    float peakDbfs = -100.0f;
    float rmsDbfs = -100.0f;
    uint64_t clipCount = 0;
};

}  // namespace media
