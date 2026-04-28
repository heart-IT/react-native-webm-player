/**
 * PlayerMetrics — full playback health snapshot returned by
 * MediaPipeline.getMetrics().
 *
 * One snapshot type, deliberately kept in one file. Sub-fields cover audio,
 * video, demux, jitter, drift, etc. — but they're a single cohesive output
 * (one call → one object).
 */

/**
 * Full playback health snapshot returned by MediaPipeline.getMetrics().
 * All counters are cumulative since start() unless noted otherwise.
 */
export interface PlayerMetrics {
  /** Decode thread liveness. */
  health: {
    /** Pipeline is active (between start() and stop()). */
    running: boolean
    /** Decode thread is responding to heartbeat checks. */
    responsive: boolean
    /** Decode thread was force-detached — unrecoverable, requires stop()/start(). */
    detached: boolean
    /** Decode thread missed its heartbeat deadline at least once. */
    watchdogTripped: boolean
    /** Total number of watchdog trips since start(). */
    watchdogTripCount: number
    /** Milliseconds since last decode thread heartbeat. */
    timeSinceHeartbeatMs: number
  }
  /** Audio decode quality counters. */
  quality: {
    /** Audio frames received from demuxer. */
    framesReceived: number
    /** Audio callback had no decoded data — silence was output. */
    underruns: number
    /** Frames dropped due to buffer overflow or late arrival. */
    framesDropped: number
    /** Frames discarded by fast-drain to reduce burst backlog. */
    framesDrained: number
    /** Opus decode failures. */
    decodeErrors: number
    /** Opus decoder resets (PTS discontinuity or error accumulation). */
    decoderResets: number
    /** Timestamp jumps detected (>500ms forward or >200ms backward). */
    ptsDiscontinuities: number
    /** Transitions between direct-copy and resampled audio path. */
    fastPathSwitches: number
    /** Catchup ratio snapped to unity within dead-zone threshold. */
    catchupDeadZoneSnaps: number
    /** Largest gap (ms) between consecutive feedData() calls. */
    maxInterFrameGapMs: number
    /** Feed gaps exceeding 50ms. */
    gapsOver50ms: number
    /** Feed gaps exceeding 100ms. */
    gapsOver100ms: number
    /** Feed gaps exceeding 500ms — triggers stall detection. */
    gapsOver500ms: number
    /** PLC (packet loss concealment) frames generated. */
    plcFrames: number
    /** Frames recovered via Opus in-band FEC (better quality than PLC). */
    fecFrames: number
    /** Silence frames skipped for instant latency catchup. */
    silenceSkipFrames: number
    /** Longest run of consecutive PLC frames. */
    peakConsecutivePLC: number
    /** Current run of consecutive PLC frames (0 when decoding normally). */
    currentConsecutivePLC: number
    /** EWMA of Opus decode duration in microseconds. */
    audioDecodeLatencyUs: number
    /** Last non-zero Opus error code (negative OPUS_* value; 0 if none). */
    audioLastDecodeError: number
  }
  /** VP9 video decode and sync metrics. */
  video: {
    /** Current A/V sync offset in microseconds (positive = video ahead of audio). */
    avSyncOffsetUs: number
    /** Peak absolute A/V sync offset seen (high-water mark, in microseconds). */
    peakAbsAvSyncOffsetUs: number
    /** Count of raw video render offsets exceeding the 45ms threshold. */
    avSyncExceedCount: number
    /** VP9 frames received from demuxer. */
    framesReceived: number
    /** VP9 frames successfully decoded by hardware decoder. */
    framesDecoded: number
    /** Video frames dropped (overflow, corruption, or keyframe gating). */
    framesDropped: number
    /** Frames dropped specifically because a keyframe was needed (subset of framesDropped). */
    keyFrameGatedDrops: number
    /** VP9 decode failures. */
    decodeErrors: number
    /** Current render frame rate (FPS). */
    currentFps: number
    /** Decoded video width in pixels. */
    width: number
    /** Decoded video height in pixels. */
    height: number
    /** Video jitter estimate in microseconds. */
    jitterUs: number
    /** Video adaptive jitter buffer target in microseconds. */
    bufferTargetUs: number
    /** Frames that arrived past their render deadline. */
    lateFrames: number
    /** Frames skipped because a fresher frame was available. */
    skippedFrames: number
    /** VP9 decoder resets (error accumulation or session invalidation). */
    decoderResets: number
    /** Render surface detach events. */
    surfaceLostCount: number
    /** Largest gap (ms) between consecutive video frames received. */
    maxInterFrameGapMs: number
    /** Video feed gaps exceeding 50ms. */
    gapsOver50ms: number
    /** Video feed gaps exceeding 100ms. */
    gapsOver100ms: number
    /** Video feed gaps exceeding 500ms. */
    gapsOver500ms: number
    /** Video clock drift in parts-per-million. */
    driftPpm: number
    /** Last VP9 decode duration in microseconds. */
    decodeLatencyUs: number
    /** Timestamp of last successful decode in microseconds. */
    lastDecodeTimeUs: number
    /** Last VP9 decode error code (0 = OK). */
    lastDecodeError: number
    /** Current encoded video queue depth (0-8). */
    queueDepth: number
    /** True if decoder is waiting for a keyframe. */
    needsKeyFrame: boolean
    /** Total keyframes requested for recovery. */
    keyFrameRequests: number
    /** VideoDecoderState enum value (0=NotCreated through 4=Failed). */
    decoderState: number
    /** Video decode thread is responding to heartbeat checks. */
    decodeThreadResponsive: boolean
    /** Milliseconds since last video decode thread heartbeat. */
    timeSinceVideoHeartbeatMs: number
  }
  /** Audio pipeline state. */
  pipeline: {
    /** StreamState: 0=Inactive, 1=Buffering, 2=Playing, 3=Underrun, 4=Paused. */
    audioStreamState: number
    /** Currently applied audio gain (0.0-2.0). */
    currentGain: number
    /** Whether audio output is muted. */
    muted: boolean
    /** Total buffered audio in microseconds (encoded + decoded). */
    bufferedDurationUs: number
    /** Decoded audio ready for playback in microseconds. */
    decodedDurationUs: number
  }
  /** Current playback state (0=Idle, 1=Buffering, 2=Playing, 3=Paused, 4=Stalled, 5=Failed). */
  playbackState: number
  /** Session-level counters. */
  session: {
    /** Milliseconds since start(). */
    uptimeMs: number
    /** Total audio samples written to hardware. */
    samplesOutput: number
    /** Current playback rate (0.5-2.0, 1.0 = normal). */
    playbackRate: number
    /** Current stream status (0=Live, 1=Buffering, 2=Ended, 3=NoPeers). */
    streamStatus: number
  }
  /** Audio output latency info. */
  latency: {
    /** "low_latency" or "standard". */
    mode: string
    /** True if AAudio exclusive mode or RemoteIO low-latency was granted. */
    isLowestLatency: boolean
    /** Estimated end-to-end latency in microseconds (buffered audio + output latency). */
    endToEndUs: number
  }
  /** Frame pool pressure indicators. */
  pools: {
    /** Decoded frame pool has < 25% slots available. */
    decodedUnderPressure: boolean
    /** Encoded frame pool has < 25% slots available. */
    encodedUnderPressure: boolean
  }
  /** Platform audio output state. */
  audioOutput: {
    /** Audio output stream restart count. */
    restartCount: number
    /** Last platform audio error code (0 = OK). */
    lastError: number
    /** Hardware sample rate actually granted (may differ from 48kHz on Bluetooth). */
    actualSampleRate: number
    /** Measured audio output latency in microseconds. */
    latencyUs: number
    /** Audio callback timing jitter in microseconds. */
    callbackJitterUs: number
    /** Audio output stream is currently running. */
    running: boolean
    /** Audio session is interrupted (iOS — phone call, Siri, etc.). */
    interrupted: boolean
    /** Granted sample rate is usable (within resampler range). */
    sampleRateValid: boolean
  }
  /** Bluetooth routing state. */
  bluetooth: {
    /** Current audio route name. */
    route: string
    /** True if Bluetooth A2DP (high-quality stereo) is active. */
    isA2dp: boolean
    /** Monotonic count of confirmed route transitions since session start. */
    routeChangeCount: number
  }
  /** Audio jitter estimation. */
  jitter: {
    /** Estimated network jitter in microseconds. */
    jitterUs: number
    /** Jitter trend (first derivative) — positive means degrading network. */
    jitterTrendUs: number
    /** Current adaptive jitter buffer target in microseconds. */
    bufferTargetUs: number
    /** Arrival confidence score (0.0 = unknown, 1.0 = stable). */
    arrivalConfidence: number
    /** True when jitter estimator is in speculative mode (low confidence). */
    speculativeMode: boolean
  }
  /** Clock drift compensation state. */
  drift: {
    /** Sender/receiver clock drift in parts-per-million. */
    driftPpm: number
    /** Drift compensation resampler is active (drift > 50 PPM). */
    active: boolean
    /** Smoothed drift correction ratio (1.0 = no drift). */
    currentRatio: number
    /** Catchup speedup ratio for draining excess buffer (1.0 = no catchup). */
    catchupRatio: number
  }
  /** Audio level metering (computed on every audio callback, lock-free). */
  levels: {
    /** Linear peak level (0.0-1.0+, values > 1.0 indicate clipping). */
    peakLevel: number
    /** Linear RMS level. */
    rmsLevel: number
    /** Peak level in dBFS (-100 = silence, 0 = full scale). */
    peakDbfs: number
    /** RMS level in dBFS. */
    rmsDbfs: number
    /** Count of audio frames that exceeded 1.0 (digital clipping). */
    clipCount: number
  }
  /** Automatic stall detection and recovery state. */
  stall: {
    /** Current stall state: "healthy", "detecting", "stalled", "recovering", or "failed". */
    state: string
    /** Total feed stalls detected since start(). */
    stallCount: number
    /** Successful recoveries from stall. */
    recoveryCount: number
    /** Keyframes requested during stall recovery. */
    keyFrameRequests: number
    /** Cumulative stall duration in milliseconds. */
    totalStallMs: number
    /** Duration of the most recent stall in milliseconds. */
    lastStallMs: number
    /** Longest single stall in milliseconds. */
    longestStallMs: number
    /** Duration of most recent recovery (rebuffer time) in milliseconds. */
    lastRecoveryMs: number
    /** Longest single recovery in milliseconds. */
    longestRecoveryMs: number
  }
  /** Clip buffer / DVR state. */
  clip: {
    /** Clip ring buffer is active (setClipBufferDuration > 0). */
    enabled: boolean
    /** Total ring buffer capacity in bytes. */
    bufferCapacity: number
    /** Bytes currently stored in the ring buffer. */
    bytesUsed: number
    /** Number of WebM clusters retained in the buffer. */
    clusterCount: number
    /** Duration of buffered content in seconds (rewind range). */
    availableSeconds: number
  }
  /** WebM demuxer state. */
  demux: {
    /** Total bytes passed to the demuxer via feedData(). */
    totalBytesFed: number
    /** Number of feedData() calls. */
    feedDataCalls: number
    /** Opus audio packets extracted from the WebM stream. */
    audioPacketsEmitted: number
    /** VP9 video packets extracted from the WebM stream. */
    videoPacketsEmitted: number
    /** Demuxer buffer overflow events. */
    overflowCount: number
    /** Consecutive block parse failures triggering a stall error. */
    blockStallCount: number
    /** Total transient parse errors (header, segment, tracks, blocks). */
    parseErrorCount: number
    /** Network-layer feed arrival jitter in microseconds (EWMA). */
    feedJitterUs: number
    /** EWMA of feedData() parse duration in microseconds. */
    feedDataLatencyUs: number
    /** Current demuxer internal buffer size in bytes. */
    bufferBytes: number
    /** Demuxer parse state (0=WaitingForEBML, 1=WaitingForSegment, 2=ParsingTracks, 3=Streaming). */
    parseState: number
    /** Partial blocks dropped (aggregate of the three split counters below). */
    partialDropCount: number
    /** Video frames rejected for exceeding kMaxVideoFrameSize (512 KB). */
    oversizedFrameDrops: number
    /** Blocks dropped because the per-feedData audio/video packet cap was hit. */
    packetCapDrops: number
    /** Stream-mode append lost trailing bytes (ring/StreamReader near-full). */
    appendBackpressureDrops: number
    /** Bytes dropped due to ingest ring buffer backpressure. */
    ingestBytesDropped: number
    /** Bytes currently used in the ingest ring buffer. */
    ingestRingUsed: number
    /** Total capacity of the ingest ring buffer in bytes. */
    ingestRingCapacity: number
    /** write() calls rejected for insufficient space on the ingest ring. */
    ingestRingWriteRejects: number
    /** Milliseconds since demuxer entered ParseState::Error; 0 if not in Error. */
    timeInErrorMs: number
    /** Monotonic parse-error count across the session (survives resetStream/seekTo). */
    cumulativeParseErrorCount: number
    /** Number of times the demuxer has been reset in this session. */
    sessionResetCount: number
    /** True if the ingest thread heartbeat is current. */
    ingestThreadResponsive: boolean
    /** Milliseconds since last ingest thread heartbeat. */
    timeSinceIngestHeartbeatMs: number
    /** True if the ingest thread was force-detached after a join timeout (unrecoverable without stop/start). */
    ingestThreadDetached: boolean
  }
}
