// V2 architecture API.
//
// Asymmetric platform-leaning playback:
//   - iOS: libwebm + libopus + VTDecompression → AVSampleBufferRenderSynchronizer
//   - Android: ExoPlayer with WebMStreamBuffer-backed DataSource
//
// Lives alongside the v1 API in src/index.tsx during the migration. Switch your
// app to this module when you're ready; v1 stays available until Phase 6 deletion.
//
// Setup: call installWebmPlayer() (from v1's index) once on app startup. That
// installs both v1 and v2 globals. v2 also exposes installWebmPlayerV2() as
// an alias for clarity.
import { NativeModules, Platform, requireNativeComponent } from 'react-native'
import type { ViewProps } from 'react-native'

declare global {
  // eslint-disable-next-line no-var
  var __MediaPipelineV2: MediaPipelineV2Surface | undefined
}

export enum PlaybackStateV2 {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Paused = 3,
  Failed = 4
}

export interface PlayerMetricsV2 {
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

interface MediaPipelineV2Surface {
  start(): boolean
  stop(): boolean
  pause(): boolean
  resume(): boolean
  isRunning(): boolean
  isPaused(): boolean
  feedData(buffer: ArrayBuffer | Uint8Array): boolean
  setMuted(muted: boolean): boolean
  setGain(gain: number): boolean
  setPlaybackRate(rate: number): boolean
  setEndOfStream(): boolean
  resetStream(): boolean
  getPlaybackState(): PlaybackStateV2
  getMetrics(): PlayerMetricsV2
}

interface AndroidV2Module {
  start(): Promise<boolean>
  stop(): Promise<boolean>
  pause(): Promise<boolean>
  resume(): Promise<boolean>
  feedData(base64Data: string): Promise<boolean>
  setMuted(muted: boolean): Promise<boolean>
  setGain(gain: number): Promise<boolean>
  setPlaybackRate(rate: number): Promise<boolean>
  resetStream(): Promise<boolean>
  goToLive(): Promise<boolean>
  attachToView(viewId: number): Promise<boolean>
  detachFromView(viewId: number): Promise<boolean>
  appDidEnterBackground(): Promise<boolean>
  appDidEnterForeground(): Promise<boolean>
}

const androidV2 = (NativeModules as { WebmPlayerV2Module?: AndroidV2Module }).WebmPlayerV2Module

function ios(): MediaPipelineV2Surface {
  if (!global.__MediaPipelineV2) {
    throw new Error(
      '__MediaPipelineV2 not installed. Call installWebmPlayer() once at app startup.'
    )
  }
  return global.__MediaPipelineV2
}

// Convert a Uint8Array / ArrayBuffer to base64. Chunked to avoid the ~64KB
// argument-list limit of String.fromCharCode.apply on some JS engines, but
// fast enough for typical 32–256KB feedData chunks.
function bytesToBase64(bytes: Uint8Array): string {
  const CHUNK = 8192
  let binary = ''
  for (let i = 0; i < bytes.length; i += CHUNK) {
    const slice = bytes.subarray(i, Math.min(i + CHUNK, bytes.length))
    // String.fromCharCode.apply on a TypedArray is not supported on all engines;
    // copy to a regular array first.
    binary += String.fromCharCode.apply(null, Array.from(slice))
  }
  // btoa is provided by the React Native runtime (Hermes + core-js polyfill).
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  return (globalThis as any).btoa(binary)
}

// Normalized API: every method returns Promise<boolean> regardless of platform,
// so cross-platform code never has to branch on the return shape. iOS sync calls
// are wrapped in Promise.resolve() — zero perf impact, just a microtask hop.
export const MediaPipelineV2 = {
  installWebmPlayerV2: () => {
    // Trigger lazy install. The actual install happens inside ios()/android equivalent
    // when first method is called; this is a no-op if already installed.
    if (Platform.OS === 'ios' && !global.__MediaPipelineV2) {
      throw new Error(
        '__MediaPipelineV2 not installed. Call installWebmPlayer() from @heartit/webm-player first.'
      )
    }
  },

  start: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().start()) : androidV2?.start() ?? Promise.resolve(false),

  stop: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().stop()) : androidV2?.stop() ?? Promise.resolve(false),

  pause: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().pause()) : androidV2?.pause() ?? Promise.resolve(false),

  resume: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().resume()) : androidV2?.resume() ?? Promise.resolve(false),

  isRunning: (): boolean => Platform.OS === 'ios' ? ios().isRunning() : false,
  isPaused: (): boolean => Platform.OS === 'ios' ? ios().isPaused() : false,

  feedData: (buffer: ArrayBuffer | Uint8Array): Promise<boolean> => {
    if (Platform.OS === 'ios') return Promise.resolve(ios().feedData(buffer))
    if (!androidV2) return Promise.resolve(false)
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
    return androidV2.feedData(bytesToBase64(bytes))
  },

  setMuted: (muted: boolean): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().setMuted(muted)) : androidV2?.setMuted(muted) ?? Promise.resolve(false),

  setGain: (gain: number): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().setGain(gain)) : androidV2?.setGain(gain) ?? Promise.resolve(false),

  setPlaybackRate: (rate: number): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().setPlaybackRate(rate)) : androidV2?.setPlaybackRate(rate) ?? Promise.resolve(false),

  setEndOfStream: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().setEndOfStream()) : Promise.resolve(false),

  resetStream: (): Promise<boolean> =>
    Platform.OS === 'ios' ? Promise.resolve(ios().resetStream()) : androidV2?.resetStream() ?? Promise.resolve(false),

  // Android-only lifecycle hooks. iOS handles this automatically via AVAudioSession.
  appDidEnterBackground: (): Promise<boolean> =>
    Platform.OS === 'android' ? androidV2?.appDidEnterBackground() ?? Promise.resolve(false) : Promise.resolve(true),
  appDidEnterForeground: (): Promise<boolean> =>
    Platform.OS === 'android' ? androidV2?.appDidEnterForeground() ?? Promise.resolve(false) : Promise.resolve(true),

  getPlaybackState: (): PlaybackStateV2 =>
    Platform.OS === 'ios' ? ios().getPlaybackState() : PlaybackStateV2.Idle,

  getMetrics: (): PlayerMetricsV2 =>
    Platform.OS === 'ios'
      ? ios().getMetrics()
      : ({
          bytesFedTotal: 0,
          audioPacketsDecoded: 0,
          videoPacketsDecoded: 0,
          audioUnderruns: 0,
          videoFramesDropped: 0,
          videoWidth: 0,
          videoHeight: 0,
          currentTimeSeconds: 0,
          playbackRate: 1.0,
          muted: false,
          gain: 1.0
        })
}

interface VideoViewV2Props extends ViewProps {
  /// 0 = stretch (resize), 1 = fit (resizeAspect, default), 2 = cover (resizeAspectFill)
  scaleMode?: number
  useController?: boolean  // Android only
}

// Both platforms register the same component name. iOS auto-attaches to the
// current engine on willMoveToWindow; Android uses StreamPlayerView.attachToView.
export const VideoViewV2 = (Platform.OS === 'ios' || Platform.OS === 'android')
  ? requireNativeComponent<VideoViewV2Props>('WebmPlayerV2View')
  : null

export const installWebmPlayerV2 = MediaPipelineV2.installWebmPlayerV2
