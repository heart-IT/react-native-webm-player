/**
 * Playback-control enums (player state, catchup behavior, stream status hint).
 */

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
