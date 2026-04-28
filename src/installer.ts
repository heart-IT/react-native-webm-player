/**
 * JSI installation surface.
 *
 * Owns the internal MediaPipelineJSI contract (the native object pinned to
 * `global.__MediaPipeline` with each method as a pre-bound Function property),
 * the install latch, and the public `installWebmPlayer()` entry point.
 *
 * Internal to the package — `getMediaPipeline()` and `MediaPipelineJSI` are
 * imported by `MediaPipeline.ts` but are not re-exported from `index.tsx`.
 */
import { Platform } from 'react-native'
import NativeWebmPlayer from './specs/NativeWebmPlayer'
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
 * Shape of the native object installed at `global.__MediaPipeline`. Mirrors
 * the public MediaPipeline surface but has no JS-side argument validation —
 * `MediaPipeline.ts` is the validation layer.
 */
export interface MediaPipelineJSI {
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

let jsiInstalled = false

function ensureJSIInstalled(): void {
  if (jsiInstalled && global.__MediaPipeline) return

  if (Platform.OS !== 'android' && Platform.OS !== 'ios') {
    throw new Error('WebmPlayer is only supported on Android and iOS')
  }

  // NativeWebmPlayer is a TurboModule spec (src/specs/NativeWebmPlayer.ts) —
  // TurboModuleRegistry.getEnforcing throws if the module isn't registered,
  // so this call is never undefined.
  const success = NativeWebmPlayer.install()
  if (!success) {
    throw new Error('Failed to install WebmPlayer JSI modules')
  }

  jsiInstalled = true
}

export function getMediaPipeline(): MediaPipelineJSI {
  ensureJSIInstalled()
  const m = global.__MediaPipeline
  if (!m) throw new Error('MediaPipeline JSI module not available')
  return m
}

/**
 * Eagerly install the WebmPlayer JSI modules.
 * Throws if the native module is missing or installation fails.
 */
export function installWebmPlayer(): void {
  ensureJSIInstalled()
}
