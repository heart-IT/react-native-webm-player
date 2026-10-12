# @heartit/webm-player

Native live-broadcast player for React Native. Hand it a Hypercore broadcast
key; native joins the swarm, demuxes VP9 + Opus WebM and plays it, with A/V
sync owned by the platform.

[![Version](https://img.shields.io/npm/v/@heartit/webm-player.svg)](https://www.npmjs.com/package/@heartit/webm-player)
[![License](https://img.shields.io/npm/l/@heartit/webm-player.svg)](https://github.com/heart-IT/react-native-webm-player/blob/main/LICENSE)

Receive-only: one audio track, one video track, no capture and no encode.

## How it works

JavaScript passes a key and nothing else. The library starts a Bare worklet
(bare-kit, in-process) that opens the broadcast's [vcr-core](https://github.com/holepunchto/vcr-core)
log through Hyperswarm, remuxes it to WebM with bare-ffmpeg, and streams the
bytes to native over the worklet's IPC. Native decodes and presents.

```
JS  ──play(key)──▶  native
                      │ starts worklet/feeder.js (Bare) for the key
                      ▼
      Corestore + VCRCore + Hyperswarm ──remux (bare-ffmpeg)──▶ WebM bytes
                      │ framed over the worklet IPC
                      ▼
   iOS: libwebm → libopus / VideoToolbox → AVSampleBufferRenderSynchronizer
   Android: ExoPlayer (MatroskaExtractor + MediaCodec + AudioTrack)
```

A live stream is never resumed from a buffer. Whenever the stream is lost — the
worklet falls behind the live edge, the network drops, an audio interruption
ends — native rejoins at the live edge with a fresh worklet. Playback keeps
running when the app is backgrounded, and on iOS runs `kLiveDelaySec` (4 s)
behind the live edge: the demux thread paces on the decoders' queues, so that
margin accumulates in the ring as compressed bytes and absorbs network jitter
without buffering decoded frames.

## Requirements

- React Native 0.78+ (New Architecture)
- `react-native-nitro-modules`
- `react-native-bare-kit` 0.16+ as a **direct** dependency of the app: its pod
  and gradle module embed the worklet's native addons, and only a direct
  dependency autolinks.
- iOS 16.6+. VP9 decodes in hardware on A13 and later.
- Android 10+ (API 29): the floor `react-native-bare-kit` declares.
- iOS apps declare `NSLocalNetworkUsageDescription`: Hyperswarm reaches a
  broadcaster on the same network directly, and iOS withholds local-network
  access from apps without it.
- To keep playing in the background: iOS apps declare `UIBackgroundModes:
audio`; Android apps keep the process alive themselves (foreground service or
  picture-in-picture) once it would otherwise be cached.

## Installation

```bash
npm install @heartit/webm-player react-native-nitro-modules react-native-bare-kit
cd ios && pod install
```

This package's `postinstall` links the worklet's native addons into
`react-native-bare-kit`, whose pod and gradle module embed them, and the gradle
module links again before every Android build. No app-side `bare-link` step is
needed. If npm scripts are disabled, run `node scripts/link-bare-addons.js ios`
and `... android` from the package after installing. The pod builds an Opus
XCFramework from source on first install.

## Usage

```tsx
import {
  createWebmPlayer,
  WebmPlayerView,
  WebmHealthStatus,
} from '@heartit/webm-player'

const player = createWebmPlayer()
player.setHealthCallback(({ status, detail }) => {
  if (status === WebmHealthStatus.Ended) showEnded()
})

player.play(broadcastKey) // z32 or hex, as the broadcaster printed it
player.muted = true

const { videoWidth, videoHeight, currentTimeSeconds } = player.getMetrics()

player.stop()
```

```tsx
<WebmPlayerView player={player} scaleMode="contain" style={{ flex: 1 }} />
```

`createWebmPlayer()` returns an independent instance; `play()` on a playing
instance stops it first.

## API

| Member                  | Notes                                                                                     |
| ----------------------- | ----------------------------------------------------------------------------------------- |
| `play(key)` / `stop()`  | Join at the live edge / tear down. A finished broadcast leaves the player stopped         |
| `playbackState`         | `Idle`, `Buffering`, `Playing`, `Failed`. `Failed` survives `stop()`; `play()` clears it  |
| `muted`                 | Read-write                                                                                |
| `getMetrics()`          | Bytes received from the worklet, packets decoded, frames dropped, frame size, position    |
| `setHealthCallback(cb)` | `Buffering`, `Playing`, `Ended`, `Failed` with a human-readable `detail`; fires on change |

`Failed` means five consecutive rejoins produced no data, or the worklet
bundle is missing from the app. Parser and player errors mid-stream are
rejoins, not failures.

## What the worklet sends

Internal, but fixed, so both platforms de-frame the same bytes. `framed-stream`
framing (4-byte little-endian length), payload byte 0 is the type:

| type   | payload                                                                                                                                                                                                              |
| ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x00` | muxed WebM bytes                                                                                                                                                                                                     |
| `0x01` | UTF-8 status: `joining`, `net <summary>` (every 5 s until a peer connects; logged, not a health event), `connected <remote address>`, `lag <detail>`, `stalled <detail>`, `reconfigured`, `ended`, `error <message>` |

The muxed stream begins at the first video keyframe after the join, so
nothing undecodable reaches the player. `lag` means the reader fell more than
10 s behind the log (usually the player not keeping up) and carries what the
watchdog measured; `stalled` means nothing was muxed for 10 s (a link that
died without an error: the log is not seen growing, so it is not lag);
`reconfigured` means the broadcaster started a new session, whose stream
parameters may differ. Native rejoins on all three. An IPC that ends without `ended` is treated the
same way, with backoff. `ended` also comes straight after the join when the
log's last session has already ended, or has not grown for five minutes
(vcr-core's `info()` liveness), so a finished broadcast stops the player
instead of waiting at an edge nothing reaches.

## Development

```bash
npm install
npm run codegen        # nitrogen: TypeScript spec -> native interfaces
npm run bundle:worklet # bare-pack worklet/feeder.js -> worklet/dist/feeder.bundle
npm run test:worklet   # remux a recorded core under Bare and check the output
npm run test:native    # sanitizer suites (address / thread / undefined)
npm run test:fuzz      # demuxer fuzzing
npm run lint:size      # file size budgets
npm run lint:pack      # published tarball must be self-contained
sh scripts/dev-broadcast.sh   # broadcast the 60 s fixture; prints a key for the example
```

The TypeScript spec in `src/specs/` is the parity contract: nitrogen fails the
build if either platform drops a member. `worklet/dist/` is generated and
git-ignored; `npm pack` rebuilds it.

See [`example/README.md`](example/README.md) for the device loop and
[`docs/DEVICE_VALIDATION.md`](docs/DEVICE_VALIDATION.md) for what has been
verified on hardware.

## License

Apache-2.0
