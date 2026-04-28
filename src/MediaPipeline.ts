/**
 * Public MediaPipeline surface — the JS-side controller for the single
 * broadcast stream. Every method delegates to the native JSI module obtained
 * via `getMediaPipeline()`.
 *
 * Numeric arguments are guarded here with `Number.isFinite` so NaN/Infinity
 * never reach the JSI boundary. See docs/ARCHITECTURE.md for the rationale.
 */
import { getMediaPipeline } from './installer'
import type {
  AudioDeviceInfo,
  AudioFocusCallback,
  AudioRoute,
  AudioRouteCallback,
  CatchupPolicy,
  HealthCallback,
  KeyFrameNeededCallback,
  PlaybackState,
  PlayerMetrics,
  StreamStatus,
  TrackInfo,
  TranscriptCallback,
  TranscriptSegment
} from './types'

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
  setGain: (gain: number): boolean => {
    if (!Number.isFinite(gain)) return false
    return getMediaPipeline().setGain(gain)
  },
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
  setBufferTarget: (audioMs: number, videoMs: number): boolean => {
    if (!Number.isFinite(audioMs) || !Number.isFinite(videoMs)) return false
    return getMediaPipeline().setBufferTarget(audioMs, videoMs)
  },
  /** Register for Android audio focus events (other apps, phone calls). iOS handles interruptions automatically. Pass null to unregister. */
  setAudioFocusCallback: (callback: AudioFocusCallback | null): boolean =>
    getMediaPipeline().setAudioFocusCallback(callback),
  /** Set behavior when player falls behind live edge (PlayThrough, Accelerate, or DropToLive). */
  setCatchupPolicy: (policy: CatchupPolicy): boolean => getMediaPipeline().setCatchupPolicy(policy),
  /** Tell the health watchdog why data stopped (Live, Buffering, Ended, NoPeers). */
  setStreamStatus: (status: StreamStatus): boolean => getMediaPipeline().setStreamStatus(status),
  /** Set playback speed. @param rate 0.5 to 2.0 (1.0 = normal speed). */
  setPlaybackRate: (rate: number): boolean => {
    if (!Number.isFinite(rate)) return false
    return getMediaPipeline().setPlaybackRate(rate)
  },
  /** Enable the native ring buffer for clip capture and DVR. @param seconds Rolling buffer duration in seconds. */
  setClipBufferDuration: (seconds: number): boolean => {
    if (!Number.isFinite(seconds)) return false
    return getMediaPipeline().setClipBufferDuration(seconds)
  },
  /** Capture the last N seconds as a standalone .webm file. Returns the local file path. Requires setClipBufferDuration() first. */
  captureClip: (lastNSeconds: number): Promise<string> =>
    getMediaPipeline().captureClip(lastNSeconds),
  /** Seek within the buffered ring buffer. @param offsetSeconds Negative = rewind from live, 0 = return to live edge. */
  seekTo: (offsetSeconds: number): boolean => getMediaPipeline().seekTo(offsetSeconds),
  /** Get how many seconds of rewind are available in the ring buffer. */
  getBufferRangeSeconds: (): number => getMediaPipeline().getBufferRangeSeconds(),

  /** Enable or disable on-device speech transcription. Pass model path when enabling. */
  setTranscriptionEnabled: (enabled: boolean, modelPath?: string): boolean =>
    getMediaPipeline().setTranscriptionEnabled(enabled, modelPath),

  /** Translate to English instead of transcribing in source language. Zero additional cost. */
  setTranslationEnabled: (enabled: boolean): boolean =>
    getMediaPipeline().setTranslationEnabled(enabled),

  /** Set callback for live transcript segments. Pass null to unsubscribe. */
  setTranscriptCallback: (callback: TranscriptCallback | null): boolean =>
    getMediaPipeline().setTranscriptCallback(callback),

  /** Get history of all transcript segments since transcription was enabled. */
  getTranscriptHistory: (): TranscriptSegment[] => getMediaPipeline().getTranscriptHistory()
}
