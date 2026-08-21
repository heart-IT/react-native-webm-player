# @heartit/webm-player

Native WebM broadcast player for React Native — VP9 video and Opus audio, with
A/V sync owned by the platform.

[![Version](https://img.shields.io/npm/v/@heartit/webm-player.svg)](https://www.npmjs.com/package/@heartit/webm-player)
[![License](https://img.shields.io/npm/l/@heartit/webm-player.svg)](https://github.com/heart-IT/react-native-webm-player/blob/main/LICENSE)

Receive-only: one audio track, one video track, no capture and no encode. You
push muxed WebM bytes in; native demuxes, decodes and presents them.

## How it works

JavaScript forwards bytes and does nothing else — no buffering, no timing, no
demuxing. Everything below `feedData` is native, and A/V sync belongs to the
platform rather than to this library.

```
your transport (e.g. Hypercore) ──▶ JS ──feedData(ArrayBuffer)──▶ native
                                                                    │
                              ┌─────────────────────────────────────┴──────┐
                        iOS                                          Android
              libwebm + libopus + VideoToolbox                      ExoPlayer
                            │                                            │
              AVSampleBufferRenderSynchronizer                       MediaCodec
```

The two platforms share a lock-free byte ring and the public API. iOS demuxes
with libwebm because `AVSampleBufferAudioRenderer` needs decoded packets;
Android hands the same muxed bytes to ExoPlayer, which demuxes internally. That
asymmetry is deliberate — leaning on each platform is the design.

## Requirements

- React Native 0.78+ (New Architecture)
- Node 18+
- `react-native-nitro-modules`

## Installation

```bash
npm install @heartit/webm-player react-native-nitro-modules
cd ios && pod install
```

The iOS pod builds an Opus XCFramework from source on first `pod install`.

## Usage

```ts
import { createWebmPlayer, WebmPlaybackState } from '@heartit/webm-player'

const player = createWebmPlayer()
player.start()

// Each chunk is muxed WebM. Writes are all-or-nothing: false means the chunk
// was dropped whole and MUST be re-sent before any newer data — the bytes feed
// a container parser, so skipping a chunk corrupts the stream from then on.
let backlog: Uint8Array | null = null
socket.on('data', (chunk: Uint8Array) => {
  backlog = backlog ? concat(backlog, chunk) : chunk
  if (player.feedData(backlog.buffer)) backlog = null
  // else: ring full (back off, retry on the next tick) or player stopped.
})

player.gain = 1.0
player.muted = false

if (player.playbackState === WebmPlaybackState.Playing) {
  const { videoWidth, videoHeight, audioUnderruns } = player.getMetrics()
}

player.setEndOfStream()
player.stop()
```

`createWebmPlayer()` returns an independent instance with its own stream buffer;
call it once per stream.

### Feeder contract

- **Header first.** A stream must begin with its complete WebM header (EBML +
  Segment + Tracks). Joining a broadcast mid-stream, prepend the stream's
  header before the first cluster you forward — a cluster with no header fails
  permanently on both platforms.
- **Retry rejected chunks.** `feedData` returning `false` means the chunk was
  dropped whole (never split). Re-send the same bytes before anything newer.
- **Reconnect via `resetStream()`.** It discards buffered bytes and rebuilds
  the parser; the next `feedData` must begin a new stream, header included.
  This is also the recovery path after `setEndOfStream()`.
- **A stalled feed is visible.** Health reports `Buffering` (detail "stream
  stalled") when bytes stop arriving mid-broadcast, and `Playing` again on
  recovery.

## API

| Member                                          | Notes                                                                                                           |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| `start()` / `stop()` / `pause()` / `resume()`   | Returns `boolean`                                                                                               |
| `feedData(data: ArrayBuffer)`                   | Muxed WebM. `false` = dropped whole; re-send (see contract)                                                     |
| `setEndOfStream()` / `resetStream()`            | Drain and end / abandon stream, expect a fresh header                                                           |
| `isRunning` / `isPaused` / `playbackState`      | Read-only. `Paused` with `isPaused === false` means the platform halted playback (background, call, focus loss) |
| `muted` / `gain` (0–1) / `playbackRate` (0.5–2) | Read-write                                                                                                      |
| `getMetrics()`                                  | Bytes fed, packets decoded, underruns, frame size, position                                                     |

## Development

```bash
npm install
npm run codegen        # nitrogen: TypeScript spec -> native interfaces
npm run test:native    # sanitizer suites (address / thread / undefined)
npm run test:fuzz      # demuxer fuzzing
npm run lint:size      # file size budgets
npm run lint:pack      # published tarball must be self-contained
```

The TypeScript spec in `src/specs/` is the parity contract: nitrogen fails the
build if either platform drops a member.

See [`docs/AUDIT.md`](docs/AUDIT.md) for the production-readiness audit,
including what is verified and what still needs device validation.

## License

Apache-2.0
