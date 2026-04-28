# @heartit/webm-player

Native WebM broadcast player for React Native — VP9 video + Opus audio with hardware-accelerated decode and A/V sync.

Receive-only. No capture, no encoding, no transmission. JS passes muxed WebM bytes to native via `feedData()`; native demuxes, decodes, and renders.

## Install

```sh
npm install @heartit/webm-player
# or
yarn add @heartit/webm-player
```

**iOS** — `cd ios && pod install`. Requires Xcode 15+, iOS 15.1+, arm64.
**Android** — no extra steps. Requires Android 10+ (API 29), arm64-v8a, NDK r27.

## Quick start

```tsx
import {
  MediaPipeline,
  VideoView,
  installWebmPlayer,
  StreamHealth,
  type HealthEvent
} from '@heartit/webm-player'

// 1. Install JSI modules (once, before any other call)
installWebmPlayer()

// 2. Start pipelines (audio + video)
MediaPipeline.start()

// 3. Feed muxed WebM from your source
function onData(chunk: ArrayBuffer) {
  MediaPipeline.feedData(chunk)
}

// 4. Render video
function Player() {
  return <VideoView scaleMode={0} style={{ flex: 1 }} />
}

// 5. Stop when done
MediaPipeline.stop()
```

## API

### `installWebmPlayer()`

Eagerly install JSI modules. Call once at app startup. Throws if the native module is missing.

### `MediaPipeline`

Single broadcast-stream controller. `start()` initializes both audio and video pipelines.

#### Lifecycle

| Method                                | Returns         | Description                                                 |
| ------------------------------------- | --------------- | ----------------------------------------------------------- |
| `start()`                             | `boolean`       | Start pipelines                                             |
| `stop()`                              | `boolean`       | Stop and release resources                                  |
| `isRunning()`                         | `boolean`       | Pipelines active?                                           |
| `warmUp()`                            | `boolean`       | Pre-start audio with silence for zero-latency first frame   |
| `pause()` / `resume()` / `isPaused()` | `boolean`       | Pause / resume / query                                      |
| `getPlaybackState()`                  | `PlaybackState` | Idle \| Buffering \| Playing \| Paused \| Stalled \| Failed |
| `getCurrentTimeUs()`                  | `number`        | Current position (µs)                                       |

#### Stream

| Method                    | Returns             | Description                                   |
| ------------------------- | ------------------- | --------------------------------------------- |
| `feedData(buffer)`        | `boolean`           | Feed muxed WebM bytes. `false` on parse error |
| `resetStream()`           | `boolean`           | Reset demuxer + pipelines for a new stream    |
| `requestKeyFrame()`       | `boolean`           | Request video keyframe recovery upstream      |
| `getTrackInfo()`          | `TrackInfo \| null` | Codec IDs + video dimensions                  |
| `setStreamStatus(status)` | `boolean`           | Enrich health events with stream context      |

#### Audio

| Method                                                     | Returns      | Description               |
| ---------------------------------------------------------- | ------------ | ------------------------- |
| `setMuted(muted)` / `setGain(gain)`                        | `boolean`    | Mute / set gain (0.0–2.0) |
| `setPlaybackRate(rate)`                                    | `boolean`    | Speed (e.g. 1.5 = 1.5×)   |
| `setAudioRoute(route, deviceId?)`                          | `boolean`    | Switch output device      |
| `getAvailableAudioRoutes()` / `getAvailableAudioDevices()` | array        | List routes / devices     |
| `getCurrentAudioRoute()`                                   | `AudioRoute` | Current output route      |

#### Callbacks

| Method                          | Description                  |
| ------------------------------- | ---------------------------- |
| `setAudioRouteCallback(cb)`     | Audio route change events    |
| `setHealthCallback(cb)`         | Health state changes         |
| `setKeyFrameNeededCallback(cb)` | Keyframe request events      |
| `setAudioFocusCallback(cb)`     | Audio focus events (Android) |

#### Tuning

| Method                              | Description                       |
| ----------------------------------- | --------------------------------- |
| `setBufferTarget(audioMs, videoMs)` | Override jitter buffer targets    |
| `setCatchupPolicy(policy)`          | Behavior when falling behind live |

#### Clip / DVR

| Method                           | Description                                 |
| -------------------------------- | ------------------------------------------- |
| `setClipBufferDuration(seconds)` | Ring buffer size (enables native buffering) |
| `captureClip(lastNSeconds)`      | `Promise<string>` — WebM file path          |
| `seekTo(offsetSeconds)`          | Seek in buffered range (negative = rewind)  |
| `getBufferRangeSeconds()`        | Available rewind range                      |

#### Metrics

`getMetrics(): PlayerMetrics` — full playback-health snapshot (see below).

### `VideoView`

Native component for decoded VP9 frames. **Only one instance at a time.**

```tsx
<VideoView
  scaleMode={0} // 0 = fit (letterbox), 1 = fill (crop)
  mirror={false}
  style={{ width: '100%', aspectRatio: 16 / 9 }}
/>
```

### Enums

```ts
enum AudioRoute {
  Unknown = 0,
  Earpiece = 1,
  Speaker = 2,
  WiredHeadset = 3,
  BluetoothSco = 4,
  BluetoothA2dp = 5,
  UsbDevice = 6
}

enum PlaybackState {
  Idle = 0,
  Buffering = 1,
  Playing = 2,
  Paused = 3,
  Stalled = 4,
  Failed = 5
}

enum StreamHealth {
  Healthy = 0,
  Buffering = 1,
  Degraded = 2,
  Stalled = 3,
  Failed = 4
}

enum CatchupPolicy {
  PlayThrough = 0,
  Accelerate = 1,
  DropToLive = 2
}

enum StreamStatus {
  Live = 0,
  Buffering = 1,
  Ended = 2,
  NoPeers = 3
}

// Android only — iOS handles session interruptions automatically.
enum AudioFocusState {
  Gained = 0,
  Lost = 1,
  LostTransient = 2,
  LostTransientCanDuck = 3
}

// Exposed via getMetrics().video.decoderState
enum VideoDecoderState {
  NotCreated = 0,
  WaitingSurface = 1,
  BackingOff = 2,
  Active = 3,
  Failed = 4
}
```

### Health monitoring

```ts
MediaPipeline.setHealthCallback((event: HealthEvent) => {
  switch (event.status) {
    case StreamHealth.Buffering:
      showSpinner()
      break
    case StreamHealth.Stalled:
      reconnectStream()
      break
    case StreamHealth.Failed:
      MediaPipeline.stop()
      MediaPipeline.start()
      reconnectStream()
      break
    case StreamHealth.Healthy:
      hideSpinner()
      break
  }
})
```

### Metrics (selected fields)

```ts
const m = MediaPipeline.getMetrics()

m.quality.underruns // Audio callback had no data
m.quality.framesDropped // Dropped (overflow / late)
m.quality.decodeErrors // Opus decode failures

m.video.framesDecoded // VP9 frames decoded
m.video.currentFps // Current render FPS
m.video.avSyncOffsetUs // A/V offset (audio = master)
m.video.needsKeyFrame // Waiting for keyframe

m.drift.driftPpm // Clock drift in PPM
m.drift.active // Compensator active
m.jitter.bufferTargetUs // Adaptive buffer target

m.levels.peakDbfs / rmsDbfs // dBFS (0 = full scale)
m.levels.clipCount // Clipping events

m.stall.state // healthy | detecting | stalled | recovering | failed
m.stall.stallCount / recoveryCount / keyFrameRequests

m.demux.totalBytesFed
m.demux.audioPacketsEmitted / videoPacketsEmitted

m.pipeline.bufferedDurationUs
```

Full interface: [`PlayerMetrics`](src/index.tsx). Field definitions: [TECHNICAL.md](TECHNICAL.md#metrics-reference).

## Clip capture

Zero-reencode — raw muxed bytes copied from a native ring buffer into a standalone `.webm` file, instantly.

```ts
MediaPipeline.setClipBufferDuration(60) // keep last 60s
const filePath = await MediaPipeline.captureClip(15) // last 15s → .webm
```

## DVR rewind

Seek backward into the clip buffer without network requests.

```ts
const available = MediaPipeline.getBufferRangeSeconds() // e.g. 45.2
MediaPipeline.seekTo(-10) // rewind 10s
MediaPipeline.seekTo(0) // return to live
```

## Audio level metering

Native-measured peak/RMS, zero-allocation, lock-free.

```ts
const { peakDbfs, rmsDbfs, clipCount } = MediaPipeline.getMetrics().levels
```

## Stall recovery

Automatic: **Detecting** (no frames for threshold) → **Stalled** (confirmed, requests keyframe) → **Recovering** (keyframe received) → **Healthy**. Set `setStreamStatus()` so the watchdog distinguishes network vs. source-side pauses.

```ts
MediaPipeline.setKeyFrameNeededCallback(() => requestKeyFrameFromSource())
MediaPipeline.setStreamStatus(StreamStatus.NoPeers)
```

## `feedData()` contract

- Pass muxed WebM bytes; native demuxes internally.
- First call must include the EBML header. Mid-stream joins buffer silently until the header arrives.
- Returns `false` on parse error. Log and continue — demuxer retries on next call.
- Video is black until first keyframe (~500ms typical).
- Never blocks. Excess frames are dropped with metrics.

## Integration with Hypercore

```ts
import Hypercore from 'hypercore'
import { MediaPipeline, installWebmPlayer } from '@heartit/webm-player'

installWebmPlayer()
MediaPipeline.start()

const core = new Hypercore(key)
await core.ready()

for await (const block of core.createReadStream({ live: true })) {
  MediaPipeline.feedData(block)
}
```

## Audio routing

```ts
MediaPipeline.setAudioRouteCallback((event) => {
  console.log(event.route, event.availableDevices)
})

const bt = MediaPipeline.getAvailableAudioDevices().find(
  (d) => d.route === AudioRoute.BluetoothA2dp
)
if (bt) MediaPipeline.setAudioRoute(AudioRoute.BluetoothA2dp, bt.deviceId)
```

Reactive — connect/disconnect headsets and the player adjusts automatically with drift compensation reset.

## Documentation

| Doc                                            | Purpose                                          |
| ---------------------------------------------- | ------------------------------------------------ |
| [Getting Started](docs/GETTING_STARTED.md)     | Tutorial                                         |
| [Integration Guide](docs/INTEGRATION_GUIDE.md) | Background audio, permissions, clip capture, DVR |
| [Troubleshooting](docs/TROUBLESHOOTING.md)     | Symptom → metric → fix                           |
| [Technical Reference](TECHNICAL.md)            | Constants, metrics, threading, tests             |
| [Architecture](docs/ARCHITECTURE.md)           | Design rationale and subsystem internals         |
| [Contributing](CONTRIBUTING.md)                | Dev workflow and PR guidelines                   |

## Build

```sh
yarn install
yarn prepare          # TS → lib/
yarn typecheck
yarn lint
yarn example ios | android
```

### Sanitizer tests

```sh
brew install opus speexdsp   # Prerequisite

yarn test:asan   # AddressSanitizer + LeakSanitizer
yarn test:tsan   # ThreadSanitizer
yarn test:ubsan  # UndefinedBehaviorSanitizer
```

368 native tests across 12 binaries. See [`tests/sanitizer/README.md`](tests/sanitizer/README.md).

### iOS Opus XCFramework

```sh
cd ios/opus
./build-opus.sh clean
./build-opus.sh build   # Opus 1.6.1
```

## Platform requirements

|              | Android              | iOS                    |
| ------------ | -------------------- | ---------------------- |
| Min version  | API 29 (Android 10)  | iOS 15.1               |
| Architecture | arm64-v8a            | arm64                  |
| Audio API    | AAudio (LOW_LATENCY) | RemoteIO AudioUnit     |
| Video API    | MediaCodec (HW VP9)  | VTDecompressionSession |
| C++ standard | C++20                | C++20                  |

## License

Apache-2.0
