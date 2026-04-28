/**
 * Audio routing, focus, and device types.
 */

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
 * Audio focus states (Android only — iOS handled by AVAudioSession).
 */
export enum AudioFocusState {
  Gained = 0,
  Lost = 1,
  LostTransient = 2,
  LostTransientCanDuck = 3
}

export type AudioFocusCallback = (state: AudioFocusState) => void

/**
 * Describes a specific audio device with its route type, name, and platform ID.
 */
export interface AudioDeviceInfo {
  route: AudioRoute
  deviceName: string
  deviceId: string
}

export interface AudioRouteChangeEvent {
  route: AudioRoute
  availableDevices: AudioDeviceInfo[]
}

export type AudioRouteCallback = (event: AudioRouteChangeEvent) => void
