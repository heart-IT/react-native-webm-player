import type { HybridObject } from 'react-native-nitro-modules'

export enum WebmPlaybackState {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Failed = 3,
}

export enum WebmHealthStatus {
  Buffering = 0,
  Playing = 1,
  /** The broadcaster ended the stream. The player stays stopped. */
  Ended = 2,
  /** Repeated rejoin attempts produced no data. The player stays stopped. */
  Failed = 3,
}

export interface WebmHealthEvent {
  status: WebmHealthStatus
  /** Human-readable cause, for logs and triage. Not a stable identifier. */
  detail: string
}

export interface WebmPlayerMetrics {
  /** Muxed WebM bytes received from the broadcast worklet. */
  bytesReceived: number
  audioPacketsDecoded: number
  videoPacketsDecoded: number
  videoFramesDropped: number
  videoWidth: number
  videoHeight: number
  /**
   * The joined stream's own timeline, in seconds. It restarts at every join,
   * rejoins included; the packet counts above keep counting across them.
   */
  currentTimeSeconds: number
}

/**
 * Receive-only WebM broadcast player.
 *
 * JS hands over a broadcast key and nothing else. Native starts a Bare worklet
 * that joins the swarm, remuxes the stream to WebM and streams the bytes to the
 * player over the worklet's IPC; the platform decodes and presents. There is no
 * timing authority and no byte path on the JS side.
 *
 * iOS demuxes with libwebm and presents through AVSampleBufferRenderSynchronizer;
 * Android hands the same bytes to ExoPlayer. Declaring both platforms here makes
 * nitrogen enforce that neither side silently drops a member.
 */
export interface WebmPlayer extends HybridObject<{
  ios: 'swift'
  android: 'kotlin'
}> {
  /**
   * Join the broadcast at `key` and play from the live edge. `key` is the
   * Hypercore key as the broadcaster printed it (z32 or hex); it is passed to
   * Corestore unchanged.
   *
   * Playback keeps running in the background. Any loss of the stream (network,
   * falling behind the live edge, an audio interruption ending) is handled by
   * rejoining at the live edge, not by resuming a buffer.
   */
  play(key: string): void
  stop(): void

  readonly playbackState: WebmPlaybackState
  muted: boolean

  getMetrics(): WebmPlayerMetrics

  /**
   * Subscribe to playback health transitions. Fires only on change, so a
   * stalled stream does not spam the callback.
   */
  setHealthCallback(callback: (event: WebmHealthEvent) => void): void
}
