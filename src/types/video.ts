/**
 * Video decoder state + VideoView component props.
 */
import type { ViewStyle } from 'react-native'

/**
 * Video decoder lifecycle state for triage diagnostics.
 * Values must match native VideoDecoderState enum.
 */
export enum VideoDecoderState {
  NotCreated = 0,
  WaitingSurface = 1,
  BackingOff = 2,
  Active = 3,
  Failed = 4
}

/** Props for the native VideoView component. */
export interface VideoViewProps {
  /** Mirror the video horizontally (useful for self-view camera preview). @default false */
  mirror?: boolean
  /** Scale mode: 0 = fit (letterbox, shows full frame with bars), 1 = fill (crop to fill view). @default 1 */
  scaleMode?: 0 | 1
  /** React Native view style. */
  style?: ViewStyle
}
