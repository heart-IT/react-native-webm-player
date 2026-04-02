/**
 * @holepunchto/webm-player — Native WebM broadcast player for React Native.
 *
 * Exports: MediaPipeline (stream controller), VideoView (render component),
 * installWebmPlayer() (JSI setup), and all supporting types/enums.
 *
 * @module @holepunchto/webm-player
 */
import React from 'react'
import { NativeModules, Platform, requireNativeComponent, type ViewStyle } from 'react-native'

// --- Types ---

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
    /** Longest run of consecutive PLC frames. */
    peakConsecutivePLC: number
  }
  /** VP9 video decode and sync metrics. */
  video: {
    /** Current A/V sync offset in microseconds (positive = video ahead of audio). */
    avSyncOffsetUs: number
    /** VP9 frames received from demuxer. */
    framesReceived: number
    /** VP9 frames successfully decoded by hardware decoder. */
    framesDecoded: number
    /** Video frames dropped (overflow or corruption). */
    framesDropped: number
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
    /** Total buffered audio in microseconds (encoded + decoded). */
    bufferedDurationUs: number
    /** Decoded audio ready for playback in microseconds. */
    decodedDurationUs: number
  }
  /** Session-level counters. */
  session: {
    /** Milliseconds since start(). */
    uptimeMs: number
    /** Total audio samples written to hardware. */
    samplesOutput: number
    /** Current playback rate (0.5-2.0, 1.0 = normal). */
    playbackRate: number
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
  }
  /** Audio jitter estimation. */
  jitter: {
    /** Estimated network jitter in microseconds. */
    jitterUs: number
    /** Current adaptive jitter buffer target in microseconds. */
    bufferTargetUs: number
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
    /** Current demuxer internal buffer size in bytes. */
    bufferBytes: number
    /** Demuxer parse state (0=WaitingForEBML, 1=WaitingForSegment, 2=ParsingTracks, 3=Streaming). */
    parseState: number
    /** Partial blocks dropped (trailing bytes lost on buffer boundary). */
    partialDropCount: number
  }
}

/**
 * Audio output routes for setAudioRoute()
 * Values must match native AudioRoute enum
 */
export enum AudioRoute {
  Unknown = 0,
  Earpiece = 1,
  Speaker = 2,
  WiredHeadset = 3,
  BluetoothSco = 4,
  BluetoothA2dp = 5,
  UsbDevice = 6
}

/**
 * Playback state for UI display.
 */
export enum PlaybackState {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Paused = 3,
  Stalled = 4,
  Failed = 5
}

/**
 * Audio focus states (Android only — iOS handled by AVAudioSession).
 */
export enum AudioFocusState {
  Gained = 0,
  Lost = 1,
  LostTransient = 2,
  LostTransientCanDuck = 3
}

export type AudioFocusCallback = (state: AudioFocusState) => void

/**
 * Describes a specific audio device with its route type, name, and platform ID.
 */
export interface AudioDeviceInfo {
  route: AudioRoute
  deviceName: string
  deviceId: string
}

export interface AudioRouteChangeEvent {
  route: AudioRoute
  availableDevices: AudioDeviceInfo[]
}

export type AudioRouteCallback = (event: AudioRouteChangeEvent) => void

/**
 * Video decoder lifecycle state for triage diagnostics.
 * Values must match native VideoDecoderState enum.
 */
export enum VideoDecoderState {
  NotCreated = 0,
  WaitingSurface = 1,
  BackingOff = 2,
  Active = 3,
  Failed = 4
}

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

/**
 * Catch-up policy for when player falls behind live.
 * Values must match native CatchupPolicy enum.
 */
export enum CatchupPolicy {
  PlayThrough = 0,
  Accelerate = 1,
  DropToLive = 2
}

/**
 * Stream status context set by the integrator.
 * Enriches health events with reason for data stalls.
 * Values must match native StreamStatus enum.
 */
export enum StreamStatus {
  Live = 0,
  Buffering = 1,
  Ended = 2,
  NoPeers = 3
}

export interface HealthEvent {
  status: StreamHealth
  detail: string
  streamStatus: StreamStatus
  metrics: {
    underruns: number
    decodeErrors: number
    framesDropped: number
    avSyncOffsetUs: number
    bufferTargetUs: number
    gapsOver100ms: number
    gapsOver500ms: number
    ptsDiscontinuities: number
    decoderResets: number
    videoFramesReceived: number
    videoFramesDecoded: number
    videoFramesDropped: number
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

/** A segment of transcribed speech with timing. */
export interface TranscriptSegment {
  /** Transcribed text. */
  text: string
  /** Segment start in microseconds (pipeline clock). */
  startUs: number
  /** Segment end in microseconds (pipeline clock). */
  endUs: number
  /** True if this is a finalized segment (not a partial/in-progress result). */
  isFinal: boolean
  /** Segment duration in seconds. Use with captureClip() to extract this moment. */
  durationSeconds: number
  /** Seconds ago relative to current playback position. Use with seekTo(-agoSeconds). */
  agoSeconds: number
}

/** Callback fired when a new transcript segment is available. */
export type TranscriptCallback = (segment: TranscriptSegment) => void

/** Props for the native TranscriptView overlay component. */
export interface TranscriptViewProps {
  /** Show or hide live captions. @default false */
  enabled?: boolean
  /** React Native view style. */
  style?: ViewStyle
}

/** Props for the native VideoView component. */
export interface VideoViewProps {
  /** Mirror the video horizontally (useful for self-view camera preview). @default false */
  mirror?: boolean
  /** Scale mode: 0 = fit (letterbox, shows full frame with bars), 1 = fill (crop to fill view). @default 1 */
  scaleMode?: 0 | 1
  /** React Native view style. */
  style?: ViewStyle
}

// --- JSI Interface ---

interface WebmPlayerModuleType {
  install(): boolean
}

interface MediaPipelineJSI {
  start(): boolean
  stop(): boolean
  isRunning(): boolean
  feedData(buffer: ArrayBuffer | Uint8Array): boolean
  setMuted(muted: boolean): boolean
  setGain(gain: number): boolean
  getMetrics(): PlayerMetrics
  setAudioRoute(route: AudioRoute, deviceId?: string): boolean
  getAvailableAudioRoutes(): AudioRoute[]
  getAvailableAudioDevices(): AudioDeviceInfo[]
  getCurrentAudioRoute(): AudioRoute
  setAudioRouteCallback(callback: AudioRouteCallback | null): boolean
  setHealthCallback(callback: HealthCallback | null): boolean
  getTrackInfo(): TrackInfo | null
  resetStream(): boolean
  requestKeyFrame(): boolean
  warmUp(): boolean
  setKeyFrameNeededCallback(callback: KeyFrameNeededCallback | null): boolean
  pause(): boolean
  resume(): boolean
  isPaused(): boolean
  getCurrentTimeUs(): number
  getPlaybackState(): PlaybackState
  setBufferTarget(audioMs: number, videoMs: number): boolean
  setAudioFocusCallback(callback: AudioFocusCallback | null): boolean
  setCatchupPolicy(policy: CatchupPolicy): boolean
  setStreamStatus(status: StreamStatus): boolean
  setPlaybackRate(rate: number): boolean
  setClipBufferDuration(seconds: number): boolean
  captureClip(lastNSeconds: number): Promise<string>
  seekTo(offsetSeconds: number): boolean
  getBufferRangeSeconds(): number
  setTranscriptionEnabled(enabled: boolean, modelPath?: string): boolean
  setTranslationEnabled(enabled: boolean): boolean
  setTranscriptCallback(callback: TranscriptCallback | null): boolean
  getTranscriptHistory(): TranscriptSegment[]
}

declare global {
  // eslint-disable-next-line no-var
  var __MediaPipeline: MediaPipelineJSI | undefined
}

const WebmPlayerNativeModule = NativeModules.WebmPlayer as WebmPlayerModuleType | undefined

let jsiInstalled = false

function ensureJSIInstalled(): void {
  if (jsiInstalled && global.__MediaPipeline) return

  if (Platform.OS !== 'android' && Platform.OS !== 'ios') {
    throw new Error('WebmPlayer is only supported on Android and iOS')
  }

  if (!WebmPlayerNativeModule) {
    throw new Error('WebmPlayer native module not found. Make sure the library is properly linked.')
  }

  const success = WebmPlayerNativeModule.install()
  if (!success) {
    throw new Error('Failed to install WebmPlayer JSI modules')
  }

  jsiInstalled = true
}

function getMediaPipeline(): MediaPipelineJSI {
  ensureJSIInstalled()
  const m = global.__MediaPipeline
  if (!m) throw new Error('MediaPipeline JSI module not available')
  return m
}

// --- Public API ---

/**
 * Single broadcast stream player.
 * Feed muxed WebM data via feedData() — native demuxes and plays audio + video.
 * start() initializes both audio and video pipelines.
 */
export const MediaPipeline = {
  /** Start audio and video playback pipelines. Call feedData() after this. */
  start: (): boolean => getMediaPipeline().start(),
  /** Stop playback and release all native resources (decoders, audio output, buffers). */
  stop: (): boolean => getMediaPipeline().stop(),
  /** True if pipelines are active (between start() and stop()). */
  isRunning: (): boolean => getMediaPipeline().isRunning(),
  /**
   * Feed muxed WebM bytes. Native demuxes into audio + video packets internally.
   * First call must include the EBML header. Returns false on parse error — continue feeding.
   * Never blocks. If data arrives faster than decode, excess frames are dropped.
   */
  feedData: (buffer: ArrayBuffer | Uint8Array): boolean => getMediaPipeline().feedData(buffer),
  /** Mute or unmute audio output. */
  setMuted: (muted: boolean): boolean => getMediaPipeline().setMuted(muted),
  /** Set audio gain. @param gain 0.0 (silent) to 2.0 (2x amplification). */
  setGain: (gain: number): boolean => getMediaPipeline().setGain(gain),
  /** Get a full playback health snapshot. See PlayerMetrics for field descriptions. */
  getMetrics: (): PlayerMetrics => getMediaPipeline().getMetrics(),
  /** Switch audio output to a specific route and optional device ID. */
  setAudioRoute: (route: AudioRoute, deviceId?: string): boolean =>
    getMediaPipeline().setAudioRoute(route, deviceId),
  /** List available audio route types (Speaker, Bluetooth, etc.). */
  getAvailableAudioRoutes: (): AudioRoute[] => getMediaPipeline().getAvailableAudioRoutes(),
  /** List audio devices with names and platform IDs. */
  getAvailableAudioDevices: (): AudioDeviceInfo[] => getMediaPipeline().getAvailableAudioDevices(),
  /** Get the currently active audio output route. */
  getCurrentAudioRoute: (): AudioRoute => getMediaPipeline().getCurrentAudioRoute(),
  /** Register for audio route change events (BT connect/disconnect, headset plug). Pass null to unregister. */
  setAudioRouteCallback: (callback: AudioRouteCallback | null): boolean =>
    getMediaPipeline().setAudioRouteCallback(callback),
  /** Register for health state transitions (Buffering, Healthy, Degraded, Stalled, Failed). Pass null to unregister. */
  setHealthCallback: (callback: HealthCallback | null): boolean =>
    getMediaPipeline().setHealthCallback(callback),
  /** Get codec IDs and video dimensions from demuxed tracks. Returns null before tracks are parsed. */
  getTrackInfo: (): TrackInfo | null => getMediaPipeline().getTrackInfo(),
  /** Reset demuxer and decode pipelines for a new stream. Does not clear the clip buffer. */
  resetStream: (): boolean => getMediaPipeline().resetStream(),
  /** Request video keyframe recovery. Clears the video queue and re-enables keyframe gating. */
  requestKeyFrame: (): boolean => getMediaPipeline().requestKeyFrame(),
  /** Pre-start audio output with silence for zero-latency first frame. Call before start(). */
  warmUp: (): boolean => getMediaPipeline().warmUp(),
  /** Register callback invoked when native needs a keyframe from the source. Pass null to unregister. */
  setKeyFrameNeededCallback: (callback: KeyFrameNeededCallback | null): boolean =>
    getMediaPipeline().setKeyFrameNeededCallback(callback),
  /** Pause playback (holds last video frame, silences audio). */
  pause: (): boolean => getMediaPipeline().pause(),
  /** Resume playback from pause. */
  resume: (): boolean => getMediaPipeline().resume(),
  /** True if playback is paused. */
  isPaused: (): boolean => getMediaPipeline().isPaused(),
  /** Current playback position in microseconds. */
  getCurrentTimeUs: (): number => getMediaPipeline().getCurrentTimeUs(),
  /** Current playback state (Idle, Buffering, Playing, Paused, Stalled, Failed). */
  getPlaybackState: (): PlaybackState => getMediaPipeline().getPlaybackState(),
  /** Override adaptive jitter buffer targets. @param audioMs Audio buffer target in milliseconds. @param videoMs Video buffer target in milliseconds. */
  setBufferTarget: (audioMs: number, videoMs: number): boolean =>
    getMediaPipeline().setBufferTarget(audioMs, videoMs),
  /** Register for Android audio focus events (other apps, phone calls). iOS handles interruptions automatically. Pass null to unregister. */
  setAudioFocusCallback: (callback: AudioFocusCallback | null): boolean =>
    getMediaPipeline().setAudioFocusCallback(callback),
  /** Set behavior when player falls behind live edge (PlayThrough, Accelerate, or DropToLive). */
  setCatchupPolicy: (policy: CatchupPolicy): boolean => getMediaPipeline().setCatchupPolicy(policy),
  /** Tell the health watchdog why data stopped (Live, Buffering, Ended, NoPeers). */
  setStreamStatus: (status: StreamStatus): boolean => getMediaPipeline().setStreamStatus(status),
  /** Set playback speed. @param rate 0.5 to 2.0 (1.0 = normal speed). */
  setPlaybackRate: (rate: number): boolean => getMediaPipeline().setPlaybackRate(rate),
  /** Enable the native ring buffer for clip capture and DVR. @param seconds Rolling buffer duration in seconds. */
  setClipBufferDuration: (seconds: number): boolean =>
    getMediaPipeline().setClipBufferDuration(seconds),
  /** Capture the last N seconds as a standalone .webm file. Returns the local file path. Requires setClipBufferDuration() first. */
  captureClip: (lastNSeconds: number): Promise<string> =>
    getMediaPipeline().captureClip(lastNSeconds),
  /** Seek within the buffered ring buffer. @param offsetSeconds Negative = rewind from live, 0 = return to live edge. */
  seekTo: (offsetSeconds: number): boolean => getMediaPipeline().seekTo(offsetSeconds),
  /** Get how many seconds of rewind are available in the ring buffer. */
  getBufferRangeSeconds: (): number => getMediaPipeline().getBufferRangeSeconds(),

  /** Enable or disable on-device speech transcription. Pass model path when enabling. */
  setTranscriptionEnabled: (enabled: boolean, modelPath?: string): boolean => getMediaPipeline().setTranscriptionEnabled(enabled, modelPath),

  /** Translate to English instead of transcribing in source language. Zero additional cost. */
  setTranslationEnabled: (enabled: boolean): boolean => getMediaPipeline().setTranslationEnabled(enabled),

  /** Set callback for live transcript segments. Pass null to unsubscribe. */
  setTranscriptCallback: (callback: TranscriptCallback | null): boolean => getMediaPipeline().setTranscriptCallback(callback),

  /** Get history of all transcript segments since transcription was enabled. */
  getTranscriptHistory: (): TranscriptSegment[] => getMediaPipeline().getTranscriptHistory()
}

// --- VideoView Native Component ---

const NativeVideoView = requireNativeComponent<VideoViewProps>('WebmVideoView')

/**
 * Native video view for rendering decoded VP9 frames from the broadcast stream.
 * Only a single VideoView instance is supported at a time.
 */
export function VideoView(props: VideoViewProps): React.ReactElement {
  return React.createElement(NativeVideoView, props)
}

// --- TranscriptView Native Component ---

const NativeTranscriptView = requireNativeComponent<TranscriptViewProps>('WebmTranscriptView')

/**
 * Native overlay view for live speech transcription captions.
 * Renders text directly from the native transcript engine — no JS in the hot path.
 * Place inside or alongside VideoView.
 */
export function TranscriptView(props: TranscriptViewProps): React.ReactElement {
  return React.createElement(NativeTranscriptView, props)
}

/**
 * Eagerly install the WebmPlayer JSI modules.
 * Throws if the native module is missing or installation fails.
 */
export function installWebmPlayer(): void {
  ensureJSIInstalled()
}
