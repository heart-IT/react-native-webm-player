/**
 * On-device speech transcription types + TranscriptView component props.
 */
import type { ViewStyle } from 'react-native'

/** A segment of transcribed speech with timing. */
export interface TranscriptSegment {
  /** Transcribed text. */
  text: string
  /** Segment start in microseconds (pipeline clock). */
  startUs: number
  /** Segment end in microseconds (pipeline clock). */
  endUs: number
  /** True if this is a finalized segment (not a partial/in-progress result). */
  isFinal: boolean
  /** Segment duration in seconds. Use with captureClip() to extract this moment. */
  durationSeconds: number
  /** Seconds ago relative to current playback position. Use with seekTo(-agoSeconds). */
  agoSeconds: number
}

/** Callback fired when a new transcript segment is available. */
export type TranscriptCallback = (segment: TranscriptSegment) => void

/** Props for the native TranscriptView overlay component. */
export interface TranscriptViewProps {
  /** Show or hide live captions. @default false */
  enabled?: boolean
  /** React Native view style. */
  style?: ViewStyle
}
