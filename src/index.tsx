import React from 'react'
import {
  NativeModules,
  Platform,
  requireNativeComponent,
  type ViewStyle
} from 'react-native'

// --- Types ---

export interface FeedResult {
  audioPackets: number
  videoPackets: number
  error?: string
}

export interface PlayerMetrics {
  health: {
    running: boolean
    responsive: boolean
    watchdogTripped: boolean
    timeSinceHeartbeatMs: number
  }
  audio: {
    underruns: number
    framesDecoded: number
    framesDropped: number
    decodeErrors: number
    decoderResets: number
    samplesOutput: number
  }
  video: {
    activeStreams: number
    avSyncOffsetUs: number
    framesDecoded: number
    framesDropped: number
    decodeErrors: number
    currentFps: number
    width: number
    height: number
    lateFrames: number
    skippedFrames: number
  }
  session: {
    uptimeMs: number
  }
  latency: {
    mode: string
    isLowestLatency: boolean
  }
  audioOutput: {
    restartCount: number
    lastError: number
    actualSampleRate: number
    latencyUs: number
  }
}

export interface BluetoothMetrics {
  route: string
  isA2dp: boolean
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
 * Describes a specific audio device with its route type, name, and platform ID.
 */
export interface AudioDeviceInfo {
  route: AudioRoute
  deviceName: string
  deviceId: string
}

/**
 * Echo cancellation mode.
 * - 0: Hardware AEC (built-in speaker/earpiece)
 * - 1: Bluetooth AEC (SCO headset)
 * - 2: None (wired headset, A2DP, USB — no echo path)
 */
export type EchoMode = 0 | 1 | 2

export interface AudioRouteChangeEvent {
  route: AudioRoute
  echoMode: EchoMode
  availableDevices: AudioDeviceInfo[]
}

export type AudioRouteCallback = (event: AudioRouteChangeEvent) => void

export interface VideoViewProps {
  participantId: number
  mirror?: boolean
  /** 0 = fit (letterbox), 1 = fill (crop). @default 0 */
  scaleMode?: 0 | 1
  style?: ViewStyle
}

// --- JSI Interface ---

interface WebmPlayerModuleType {
  install(): boolean
}

interface MediaPipelineJSI {
  warmUp(): boolean
  start(): boolean
  stop(): boolean
  isRunning(): boolean
  addParticipant(): number
  removeParticipant(participantId: number): boolean
  pushAudioFrame(
    participantId: number,
    buffer: ArrayBuffer | Uint8Array,
    ptsUs: number,
    durationUs: number
  ): boolean
  setMuted(participantId: number, muted: boolean): boolean
  setGain(participantId: number, gain: number): boolean
  getMetrics(): PlayerMetrics
  resetWatchdog(): boolean
  setAudioRoute(route: AudioRoute, deviceId?: string): boolean
  getAvailableAudioRoutes(): AudioRoute[]
  getAvailableAudioDevices(): AudioDeviceInfo[]
  getCurrentAudioRoute(): AudioRoute
  setAudioRouteCallback(callback: AudioRouteCallback | null): boolean
}

interface VideoPipelineJSI {
  start(): boolean
  stop(): boolean
  isRunning(): boolean
  pushVideoFrame(
    participantId: number,
    buffer: ArrayBuffer | Uint8Array,
    ptsUs: number,
    isKeyFrame: boolean
  ): boolean
  removeParticipant(participantId: number): boolean
  requestKeyFrame(participantId: number): boolean
  getMetrics(): {
    activeStreams: number
    avSyncOffsetUs: number
    participants: Array<{
      id: number
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
    }>
  }
}

declare global {
  // eslint-disable-next-line no-var
  var __MediaPipeline: MediaPipelineJSI | undefined
  // eslint-disable-next-line no-var
  var __VideoPipeline: VideoPipelineJSI | undefined
}

const WebmPlayerNativeModule = NativeModules.WebmPlayer as
  | WebmPlayerModuleType
  | undefined

let jsiInstalled = false

function ensureJSIInstalled(): void {
  if (jsiInstalled && global.__MediaPipeline) return

  if (Platform.OS !== 'android' && Platform.OS !== 'ios') {
    throw new Error('WebmPlayer is only supported on Android and iOS')
  }

  if (!WebmPlayerNativeModule) {
    throw new Error(
      'WebmPlayer native module not found. Make sure the library is properly linked.'
    )
  }

  const success = WebmPlayerNativeModule.install()
  if (!success) {
    throw new Error('Failed to install WebmPlayer JSI modules')
  }

  jsiInstalled = true
}

// --- Public API ---

export const MediaPipeline = {
  warmUp(): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.warmUp()
  },

  start(): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.start()
  },

  stop(): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.stop()
  },

  isRunning(): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.isRunning()
  },

  addParticipant(): number {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.addParticipant()
  },

  removeParticipant(participantId: number): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.removeParticipant(participantId)
  },

  pushAudioFrame(
    participantId: number,
    buffer: ArrayBuffer | Uint8Array,
    ptsUs: number,
    durationUs: number
  ): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.pushAudioFrame(
      participantId,
      buffer,
      ptsUs,
      durationUs
    )
  },

  setMuted(participantId: number, muted: boolean): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.setMuted(participantId, muted)
  },

  setGain(participantId: number, gain: number): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.setGain(participantId, gain)
  },

  getMetrics(): PlayerMetrics {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.getMetrics()
  },

  resetWatchdog(): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.resetWatchdog()
  },

  setAudioRoute(route: AudioRoute, deviceId?: string): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.setAudioRoute(route, deviceId)
  },

  getAvailableAudioRoutes(): AudioRoute[] {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.getAvailableAudioRoutes()
  },

  getAvailableAudioDevices(): AudioDeviceInfo[] {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.getAvailableAudioDevices()
  },

  getCurrentAudioRoute(): AudioRoute {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.getCurrentAudioRoute()
  },

  setAudioRouteCallback(callback: AudioRouteCallback | null): boolean {
    ensureJSIInstalled()
    if (!global.__MediaPipeline) {
      throw new Error('MediaPipeline JSI module not available')
    }
    return global.__MediaPipeline.setAudioRouteCallback(callback)
  }
}

export const VideoPipeline = {
  start(): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.start()
  },

  stop(): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.stop()
  },

  isRunning(): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.isRunning()
  },

  pushVideoFrame(
    participantId: number,
    buffer: ArrayBuffer | Uint8Array,
    ptsUs: number,
    isKeyFrame: boolean
  ): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.pushVideoFrame(
      participantId,
      buffer,
      ptsUs,
      isKeyFrame
    )
  },

  removeParticipant(participantId: number): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.removeParticipant(participantId)
  },

  requestKeyFrame(participantId: number): boolean {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.requestKeyFrame(participantId)
  },

  getMetrics() {
    ensureJSIInstalled()
    if (!global.__VideoPipeline) {
      throw new Error('VideoPipeline JSI module not available')
    }
    return global.__VideoPipeline.getMetrics()
  }
}

// --- VideoView Native Component ---

const NativeVideoView =
  requireNativeComponent<VideoViewProps>('WebmVideoView')

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
