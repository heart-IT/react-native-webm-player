import { getHostComponent, NitroModules } from 'react-native-nitro-modules'
import type { WebmPlayer } from './specs/webm-player.nitro'
import type { WebmPlayerViewProps } from './specs/webm-player-view.nitro'
import WebmPlayerViewConfig from '../nitrogen/generated/shared/json/WebmPlayerViewConfig.json'

export type { WebmPlayer, WebmPlayerMetrics } from './specs/webm-player.nitro'
export { WebmPlaybackState } from './specs/webm-player.nitro'
export type {
  WebmPlayerViewProps,
  WebmScaleMode,
} from './specs/webm-player-view.nitro'

/**
 * Create a player.
 *
 * Each call returns an independent instance with its own stream buffer, so two
 * players never share bytes.
 *
 * This is a function rather than a module-level constant on purpose:
 * `createHybridObject` throws when the HybridObject is not registered, and doing
 * that at import time makes the failure unrecoverable — it takes down the module
 * graph instead of the call site. It also bites in environments with no native
 * module at all, such as a Jest run without mocks.
 */
export function createWebmPlayer(): WebmPlayer {
  return NitroModules.createHybridObject<WebmPlayer>('WebmPlayer')
}

/**
 * Renders a player's video.
 *
 * ```tsx
 * <WebmPlayerView player={player} scaleMode="contain" style={{ flex: 1 }} />
 * ```
 */
export const WebmPlayerView = getHostComponent<WebmPlayerViewProps, {}>(
  'WebmPlayerView',
  () => WebmPlayerViewConfig
)
