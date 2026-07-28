import type { HybridView, HybridViewProps } from 'react-native-nitro-modules'
import type { WebmPlayer } from './webm-player.nitro'

/** How decoded video is fitted into the view's bounds. */
export type WebmScaleMode = 'contain' | 'cover' | 'stretch'

export interface WebmPlayerViewProps extends HybridViewProps {
  /**
   * The player whose video this view presents.
   *
   * Passing the player itself is what ties the surface to the engine. The
   * predecessor used a global view registry keyed by React tag and an
   * `attachToView(tag)` call; here the association is the prop, so it cannot
   * outlive the view or bind to the wrong instance.
   *
   * Optional because React mounts the view before props are applied, so the
   * native side must be able to exist without a player attached yet.
   */
  player?: WebmPlayer
  scaleMode: WebmScaleMode
}

export type WebmPlayerView = HybridView<WebmPlayerViewProps>
