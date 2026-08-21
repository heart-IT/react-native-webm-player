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
  /**
   * The platform halted playback for an audio interruption (phone call) or
   * audio-focus loss. Playback resumes automatically when the system allows
   * it; if the system requires user action instead, `isPaused` becomes true
   * and a `resume()` is needed.
   */
  Interrupted = 4,
}

export interface WebmHealthEvent {
  status: WebmHealthStatus
  /** Human-readable cause, for logs and triage. Not a stable identifier. */
  detail: string
}

export interface WebmPlayerMetrics {
  bytesFedTotal: number
  /**
   * Muxed bytes rejected because the ring was full. Rejections are whole-chunk;
   * a climbing value means the feeder is outrunning the demuxer and must
   * re-send what was rejected (see feedData).
   */
  bytesDropped: number
  /**
   * Ring occupancy, 0.0–1.0. The feeder's backpressure signal: back off as
   * this approaches 1.0 rather than waiting for feedData to start rejecting.
   */
  ringFillRatio: number
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
   * Feed muxed WebM bytes. The buffer is copied before this returns; the
   * caller may reuse it immediately.
   *
   * The stream must begin with a complete WebM header (EBML + Segment +
   * Tracks). A feeder joining a broadcast mid-stream must prepend the stream's
   * header before the first cluster it forwards.
   *
   * Returns false when the bytes were NOT accepted. Writes are all-or-nothing:
   * a rejected chunk was dropped whole, and because the bytes feed a container
   * parser, the feeder MUST re-send the same chunk before any newer data —
   * feeding the next chunk instead corrupts the stream from that point on.
   * False means either the ring is full (producer outrunning the demuxer —
   * back off and retry) or the player is not running (start() not called, or
   * stop() already called — stop feeding).
   */
  feedData(data: ArrayBuffer): boolean
  /**
   * No more bytes are coming. Playback drains what is buffered and reports
   * health `Ended`. To play again after this, call resetStream() and feed a
   * fresh stream, or stop() and start().
   */
  setEndOfStream(): void
  /**
   * Abandon the current stream: buffered bytes are discarded and the parser
   * rebuilt. The next feedData() must begin a new stream with its own complete
   * WebM header. Use on broadcast reconnect.
   */
  resetStream(): void

  muted: boolean
  /**
   * 0.0–1.0. Values above 1.0 are clamped: neither platform's renderer
   * amplifies.
   */
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
