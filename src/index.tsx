import React from 'react'
import { NativeModules, Platform, requireNativeComponent, type ViewStyle } from 'react-native'

// --- Types ---

export interface PlayerMetrics {
  health: {
    running: boolean
    responsive: boolean
    detached: boolean
    watchdogTripped: boolean
    watchdogTripCount: number
    timeSinceHeartbeatMs: number
  }
  quality: {
    framesReceived: number
    underruns: number
    framesDropped: number
    framesDrained: number
    decodeErrors: number
    decoderResets: number
    ptsDiscontinuities: number
    fastPathSwitches: number
    catchupDeadZoneSnaps: number
    maxInterFrameGapMs: number
    gapsOver50ms: number
    gapsOver100ms: number
    gapsOver500ms: number
  }
  video: {
    avSyncOffsetUs: number
    framesReceived: number
    framesDecoded: number
    framesDropped: number
    decodeErrors: number
    currentFps: number
    width: number
    height: number
    jitterUs: number
    bufferTargetUs: number
    lateFrames: number
    skippedFrames: number
    decoderResets: number
    surfaceLostCount: number
    maxInterFrameGapMs: number
    gapsOver50ms: number
    gapsOver100ms: number
    gapsOver500ms: number
    driftPpm: number
    queueDepth: number
    needsKeyFrame: boolean
  }
  pipeline: {
    audioStreamState: number
    currentGain: number
    bufferedDurationUs: number
    decodedDurationUs: number
  }
  session: {
    uptimeMs: number
    samplesOutput: number
    playbackRate: number
  }
  latency: {
    mode: string
    isLowestLatency: boolean
  }
  pools: {
    decodedUnderPressure: boolean
    encodedUnderPressure: boolean
  }
  audioOutput: {
    restartCount: number
    lastError: number
    actualSampleRate: number
    latencyUs: number
  }
  bluetooth: {
    route: string
    isA2dp: boolean
  }
  jitter: {
    jitterUs: number
    bufferTargetUs: number
  }
  drift: {
    driftPpm: number
    active: boolean
    currentRatio: number
    catchupRatio: number
  }
  demux: {
    totalBytesFed: number
    feedDataCalls: number
    audioPacketsEmitted: number
    videoPacketsEmitted: number
    overflowCount: number
    parseState: number
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

export interface VideoViewProps {
  mirror?: boolean
  /** 0 = fit (letterbox), 1 = fill (crop). @default 1 */
  scaleMode?: 0 | 1
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
  start: (): boolean => getMediaPipeline().start(),
  stop: (): boolean => getMediaPipeline().stop(),
  isRunning: (): boolean => getMediaPipeline().isRunning(),
  feedData: (buffer: ArrayBuffer | Uint8Array): boolean => getMediaPipeline().feedData(buffer),
  setMuted: (muted: boolean): boolean => getMediaPipeline().setMuted(muted),
  setGain: (gain: number): boolean => getMediaPipeline().setGain(gain),
  getMetrics: (): PlayerMetrics => getMediaPipeline().getMetrics(),
  setAudioRoute: (route: AudioRoute, deviceId?: string): boolean =>
    getMediaPipeline().setAudioRoute(route, deviceId),
  getAvailableAudioRoutes: (): AudioRoute[] => getMediaPipeline().getAvailableAudioRoutes(),
  getAvailableAudioDevices: (): AudioDeviceInfo[] => getMediaPipeline().getAvailableAudioDevices(),
  getCurrentAudioRoute: (): AudioRoute => getMediaPipeline().getCurrentAudioRoute(),
  setAudioRouteCallback: (callback: AudioRouteCallback | null): boolean =>
    getMediaPipeline().setAudioRouteCallback(callback),
  setHealthCallback: (callback: HealthCallback | null): boolean =>
    getMediaPipeline().setHealthCallback(callback),
  getTrackInfo: (): TrackInfo | null => getMediaPipeline().getTrackInfo(),
  resetStream: (): boolean => getMediaPipeline().resetStream(),
  requestKeyFrame: (): boolean => getMediaPipeline().requestKeyFrame(),
  warmUp: (): boolean => getMediaPipeline().warmUp(),
  setKeyFrameNeededCallback: (callback: KeyFrameNeededCallback | null): boolean =>
    getMediaPipeline().setKeyFrameNeededCallback(callback),
  pause: (): boolean => getMediaPipeline().pause(),
  resume: (): boolean => getMediaPipeline().resume(),
  isPaused: (): boolean => getMediaPipeline().isPaused(),
  getCurrentTimeUs: (): number => getMediaPipeline().getCurrentTimeUs(),
  getPlaybackState: (): PlaybackState => getMediaPipeline().getPlaybackState(),
  setBufferTarget: (audioMs: number, videoMs: number): boolean =>
    getMediaPipeline().setBufferTarget(audioMs, videoMs),
  setAudioFocusCallback: (callback: AudioFocusCallback | null): boolean =>
    getMediaPipeline().setAudioFocusCallback(callback),
  setCatchupPolicy: (policy: CatchupPolicy): boolean => getMediaPipeline().setCatchupPolicy(policy),
  setStreamStatus: (status: StreamStatus): boolean => getMediaPipeline().setStreamStatus(status),
  setPlaybackRate: (rate: number): boolean => getMediaPipeline().setPlaybackRate(rate),
  setClipBufferDuration: (seconds: number): boolean =>
    getMediaPipeline().setClipBufferDuration(seconds),
  captureClip: (lastNSeconds: number): Promise<string> =>
    getMediaPipeline().captureClip(lastNSeconds),
  seekTo: (offsetSeconds: number): boolean => getMediaPipeline().seekTo(offsetSeconds),
  getBufferRangeSeconds: (): number => getMediaPipeline().getBufferRangeSeconds()
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

/**
 * Eagerly install the WebmPlayer JSI modules.
 * Throws if the native module is missing or installation fails.
 */
export function installWebmPlayer(): void {
  ensureJSIInstalled()
}
