import type { HybridObject } from 'react-native-nitro-modules'

export enum WebmPlaybackState {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Paused = 3,
  Failed = 4,
}

export interface WebmPlayerMetrics {
  bytesFedTotal: number
  audioPacketsDecoded: number
  videoPacketsDecoded: number
  audioUnderruns: number
  videoFramesDropped: number
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
}
