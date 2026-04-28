import React from 'react'
import { requireNativeComponent } from 'react-native'
import type { VideoViewProps } from '../types'

const NativeVideoView = requireNativeComponent<VideoViewProps>('WebmVideoView')

/**
 * Native video view for rendering decoded VP9 frames from the broadcast stream.
 * Only a single VideoView instance is supported at a time.
 */
export function VideoView(props: VideoViewProps): React.ReactElement {
  return React.createElement(NativeVideoView, props)
}
