import type { HybridObject } from 'react-native-nitro-modules'

export enum WebmPlaybackState {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Paused = 3,
  Failed = 4,
}

/** Where audio is currently coming out. Read-only: for a media-playback
 * session the user chooses the output through system UI on both platforms. */
export enum WebmAudioRoute {
  Unknown = 0,
  Earpiece = 1,
  Speaker = 2,
  WiredHeadset = 3,
  Bluetooth = 4,
  Usb = 5,
}

export enum WebmHealthStatus {
  Buffering = 0,
  Playing = 1,
  Ended = 2,
  Failed = 3,
}

export interface WebmHealthEvent {
  status: WebmHealthStatus
  /** Human-readable cause, for logs and triage. Not a stable identifier. */
  detail: string
}

export interface WebmPlayerMetrics {
  bytesFedTotal: number
  audioPacketsDecoded: number
  videoPacketsDecoded: number
  /**
   * Renderer starvation events. Android only (ExoPlayer reports them); always 0
   * on iOS, where AVSampleBufferAudioRenderer does not expose starvation.
   */
  audioUnderruns: number
  videoFramesDropped: number
  /**
   * Audio frames rebuilt from Opus FEC, or concealed by PLC, after a gap.
   * iOS only; always 0 on Android, where ExoPlayer conceals internally without
   * reporting.
   */
  audioFramesRecovered: number
  videoWidth: number
  videoHeight: number
  currentTimeSeconds: number
  playbackRate: number
  muted: boolean
  gain: number
}

/**
 * Receive-only WebM broadcast player.
 *
 * JS forwards muxed WebM bytes; native demuxes, decodes and presents. Platform
 * frameworks own A/V sync — there is no timing authority on the JS side.
 *
 * iOS demuxes with libwebm and presents through AVSampleBufferRenderSynchronizer;
 * Android hands the same bytes to ExoPlayer. Declaring both platforms here makes
 * nitrogen enforce that neither side silently drops a member.
 */
export interface WebmPlayer extends HybridObject<{
  ios: 'swift'
  android: 'kotlin'
}> {
  start(): boolean
  stop(): boolean
  pause(): boolean
  resume(): boolean

  readonly isRunning: boolean
  readonly isPaused: boolean
  readonly playbackState: WebmPlaybackState

  /**
   * Feed muxed WebM bytes. Returns false when the ring is full, meaning the
   * producer is outrunning the demuxer.
   */
  feedData(data: ArrayBuffer): boolean
  setEndOfStream(): void
  resetStream(): void

  muted: boolean
  /** 0.0–2.0 */
  gain: number
  /** 0.5–2.0 */
  playbackRate: number

  getMetrics(): WebmPlayerMetrics

  /**
   * Subscribe to playback health transitions. Fires only on change, so a
   * stalled stream does not spam the callback.
   */
  setHealthCallback(callback: (event: WebmHealthEvent) => void): void

  /** Where audio is currently routed. */
  readonly currentAudioRoute: WebmAudioRoute
  /** Output routes the system currently reports as available. */
  getAvailableAudioRoutes(): WebmAudioRoute[]
  /** Fires when the system changes the output route. */
  setRouteChangeCallback(callback: (route: WebmAudioRoute) => void): void
}
