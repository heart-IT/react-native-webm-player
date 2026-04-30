// V2 architecture API.
//
// Asymmetric platform-leaning playback:
//   - iOS: libwebm + libopus + VTDecompression → AVSampleBufferRenderSynchronizer
//   - Android: ExoPlayer with WebMStreamBuffer-backed DataSource
//
// Lives alongside the v1 API in src/index.tsx during the migration. Switch your
// app to this module when you're ready; v1 stays available until Phase 6 deletion.
import { NativeModules, Platform, requireNativeComponent } from 'react-native'
import type { ViewProps } from 'react-native'

declare global {
  // iOS: installed by JSIInstaller as a global jsi::Object property.
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
  feedData(data: number[]): Promise<boolean>
  setMuted(muted: boolean): Promise<boolean>
  setGain(gain: number): Promise<boolean>
  resetStream(): Promise<boolean>
  goToLive(): Promise<boolean>
  attachToView(viewId: number): Promise<boolean>
  detachFromView(viewId: number): Promise<boolean>
}

const androidV2 = (NativeModules as { WebmPlayerV2Module?: AndroidV2Module }).WebmPlayerV2Module

function ios(): MediaPipelineV2Surface {
  if (!global.__MediaPipelineV2) {
    throw new Error('__MediaPipelineV2 is not installed. Did you call installWebmPlayer() (which auto-installs v2)?')
  }
  return global.__MediaPipelineV2
}

export const MediaPipelineV2 = {
  start: () => Platform.OS === 'ios' ? ios().start() : androidV2?.start() ?? Promise.resolve(false),
  stop: () => Platform.OS === 'ios' ? ios().stop() : androidV2?.stop() ?? Promise.resolve(false),
  pause: () => Platform.OS === 'ios' ? ios().pause() : androidV2?.pause() ?? Promise.resolve(false),
  resume: () => Platform.OS === 'ios' ? ios().resume() : androidV2?.resume() ?? Promise.resolve(false),
  isRunning: () => Platform.OS === 'ios' ? ios().isRunning() : false,
  isPaused: () => Platform.OS === 'ios' ? ios().isPaused() : false,

  feedData: (buffer: ArrayBuffer | Uint8Array): boolean | Promise<boolean> => {
    if (Platform.OS === 'ios') return ios().feedData(buffer)
    if (!androidV2) return false
    const arr = buffer instanceof Uint8Array ? Array.from(buffer) : Array.from(new Uint8Array(buffer))
    return androidV2.feedData(arr)
  },

  setMuted: (muted: boolean) => Platform.OS === 'ios' ? ios().setMuted(muted) : androidV2?.setMuted(muted) ?? Promise.resolve(false),
  setGain: (gain: number) => Platform.OS === 'ios' ? ios().setGain(gain) : androidV2?.setGain(gain) ?? Promise.resolve(false),
  setPlaybackRate: (rate: number) => Platform.OS === 'ios' ? ios().setPlaybackRate(rate) : Promise.resolve(false),
  setEndOfStream: () => Platform.OS === 'ios' ? ios().setEndOfStream() : Promise.resolve(false),
  resetStream: () => Platform.OS === 'ios' ? ios().resetStream() : androidV2?.resetStream() ?? Promise.resolve(false),

  getPlaybackState: (): PlaybackStateV2 => Platform.OS === 'ios' ? ios().getPlaybackState() : PlaybackStateV2.Idle,
  getMetrics: (): PlayerMetricsV2 => Platform.OS === 'ios'
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

// Android view component. iOS view wiring is TBD (Phase 3.5):
// the AVSampleBufferDisplayLayer is owned by PlaybackEngineV2.setDisplayLayer
// and currently must be wired from native code.
interface VideoViewV2Props extends ViewProps {
  useController?: boolean
}

export const VideoViewV2 = Platform.OS === 'android'
  ? requireNativeComponent<VideoViewV2Props>('WebmPlayerV2View')
  : null
