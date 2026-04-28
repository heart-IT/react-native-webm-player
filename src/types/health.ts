/**
 * Stream health, watchdog events, track info, and the keyframe-needed callback.
 */
import type { StreamStatus } from './playback'
import type { VideoDecoderState } from './video'

/**
 * Stream health states reported by the health watchdog callback.
 * Values must match native StreamHealth enum.
 */
export enum StreamHealth {
  Healthy = 0,
  Buffering = 1,
  Degraded = 2,
  Stalled = 3,
  Failed = 4
}

export interface HealthEvent {
  status: StreamHealth
  detail: string
  streamStatus: StreamStatus
  metrics: {
    /** Audio Playing→Underrun edge transitions (state changes, not events). */
    underruns: number
    /** Audio callbacks that produced zero samples — per-callback event counter. */
    silenceCallbacks: number
    /** Audio frames discarded by fast-drain (latency catchup). */
    framesDrained: number
    /** Mixer fast-path ↔ resampler-mode transitions. */
    fastPathSwitches: number
    decodeErrors: number
    framesDropped: number
    /** EWMA-smoothed instantaneous A/V sync offset (microseconds). */
    avSyncOffsetUs: number
    /** Cumulative absolute peak A/V offset across the session. */
    peakAbsAvSyncOffsetUs: number
    /** Count of A/V samples that exceeded the 45ms emergency threshold. */
    avSyncExceedCount: number
    bufferTargetUs: number
    gapsOver50ms: number
    gapsOver100ms: number
    gapsOver500ms: number
    ingestRingWriteRejects: number
    ptsDiscontinuities: number
    decoderResets: number
    videoFramesReceived: number
    videoFramesDecoded: number
    videoFramesDropped: number
    /** Decayed FPS — zero when the decoder hasn't ticked for >1s (freeze gauge). */
    currentFps: number
    needsKeyFrame: boolean
    videoDecodeErrors: number
    videoDecoderResets: number
    videoDecoderState: VideoDecoderState
    ingestThreadDetached: boolean
    /** Milliseconds since the last ingest-thread heartbeat (0 if mid-restart / unknown). */
    timeSinceIngestHeartbeatMs: number
    videoDecodeThreadDetached: boolean
    /** Milliseconds since the last video-decode-thread heartbeat. */
    timeSinceVideoHeartbeatMs: number
    audioFramesReceived: number
    parseErrorCount: number
    timeInErrorMs: number
    demuxerWedged: boolean
  }
}

export type HealthCallback = (event: HealthEvent) => void

export interface TrackInfo {
  audioCodecId: string
  videoCodecId: string
  videoWidth: number
  videoHeight: number
}

export type KeyFrameNeededCallback = () => void
