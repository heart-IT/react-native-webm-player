import React from 'react'
import { requireNativeComponent } from 'react-native'
import type { TranscriptViewProps } from '../types'

const NativeTranscriptView = requireNativeComponent<TranscriptViewProps>('WebmTranscriptView')

/**
 * Native overlay view for live speech transcription captions.
 * Renders text directly from the native transcript engine — no JS in the hot path.
 * Place inside or alongside VideoView.
 */
export function TranscriptView(props: TranscriptViewProps): React.ReactElement {
  return React.createElement(NativeTranscriptView, props)
}
