import { NitroModules } from 'react-native-nitro-modules'
import type { WebmPlayer as WebmPlayerSpec } from './specs/webm-player.nitro'

export const WebmPlayer =
  NitroModules.createHybridObject<WebmPlayerSpec>('WebmPlayer')