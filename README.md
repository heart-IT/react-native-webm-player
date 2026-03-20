# @holepunchto/webm-player

Native WebM broadcast player for React Native — VP9 video + Opus audio with hardware-accelerated decoding and A/V sync.

Receive-only. No capture, no encoding, no transmission. JS passes muxed WebM bytes to native via `feedData()` — native demuxes, decodes, and renders.

## Install

```sh
npm install @holepunchto/webm-player
# or
yarn add @holepunchto/webm-player
```

### iOS

```sh
cd ios && pod install
```

Requires Xcode 15+, iOS 15.1+, arm64.

### Android

No extra steps. Requires Android 10+ (API 29), arm64-v8a, NDK r27.

## Quick Start

```tsx
import {
  MediaPipeline,
  VideoView,
  installWebmPlayer,
  StreamHealth,
  type HealthEvent
} from '@holepunchto/webm-player'

// 1. Install JSI modules (once, before any other call)
installWebmPlayer()

// 2. Start the player (initializes audio + video pipelines)
MediaPipeline.start()

// 3. Feed muxed WebM data from your source (Hypercore, fetch, etc.)
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

Eagerly install JSI modules. Call once at app startup. Throws if native module is missing.

### `MediaPipeline`

Single broadcast stream controller. `start()` initializes both audio and video pipelines.

| Method | Returns | Description |
|--------|---------|-------------|
| `start()` | `boolean` | Start playback pipelines |
| `stop()` | `boolean` | Stop and release resources |
| `isRunning()` | `boolean` | Check if pipelines are active |
| `feedData(buffer)` | `boolean` | Feed muxed WebM bytes. Returns `false` on parse error |
| `setMuted(muted)` | `boolean` | Mute/unmute audio |
| `setGain(gain)` | `boolean` | Set audio gain (0.0 - 2.0) |
| `getMetrics()` | `PlayerMetrics` | Get playback health snapshot |
| `setAudioRoute(route, deviceId?)` | `boolean` | Switch audio output device |
| `getAvailableAudioRoutes()` | `AudioRoute[]` | List available route types |
| `getAvailableAudioDevices()` | `AudioDeviceInfo[]` | List devices with names and IDs |
| `getCurrentAudioRoute()` | `AudioRoute` | Get current output route |
| `setAudioRouteCallback(cb)` | `boolean` | Register for route change events |
| `setHealthCallback(cb)` | `boolean` | Register for health state changes |

### `VideoView`

Native component for rendering decoded VP9 frames.

```tsx
<VideoView
  scaleMode={0}  // 0 = fit (letterbox), 1 = fill (crop)
  mirror={false}
  style={{ width: '100%', aspectRatio: 16/9 }}
/>
```

Only one `VideoView` instance is supported at a time.

### `AudioRoute` Enum

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
```

### `StreamHealth` Enum

```ts
enum StreamHealth {
  Healthy = 0,    // All metrics normal
  Buffering = 1,  // Initial buffering or rebuffering
  Degraded = 2,   // Playing with quality loss
  Stalled = 3,    // No data flowing
  Failed = 4      // Unrecoverable — requires stop/start
}
```

### Health Monitoring

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

### Metrics

```ts
const m = MediaPipeline.getMetrics()

m.quality.underruns        // Audio callback had no data
m.quality.framesDropped    // Frames dropped (overflow/late)
m.quality.decodeErrors     // Opus decode failures
m.video.framesDecoded      // VP9 frames decoded
m.video.currentFps         // Current render FPS
m.video.avSyncOffsetUs     // A/V sync offset (audio = master)
m.drift.driftPpm           // Clock drift in PPM
m.jitter.bufferTargetUs    // Adaptive jitter buffer target
```

See [`PlayerMetrics`](src/index.tsx) for the full metrics interface.

## Integration with Hypercore

```ts
import Hypercore from 'hypercore'
import { MediaPipeline, installWebmPlayer } from '@holepunchto/webm-player'

installWebmPlayer()
MediaPipeline.start()

const core = new Hypercore(key)
await core.ready()

const stream = core.createReadStream({ live: true })

for await (const block of stream) {
  MediaPipeline.feedData(block)
}
```

### `feedData()` Contract

- Pass muxed WebM bytes. Native demuxes into audio + video packets internally.
- First call must include the EBML header. Mid-stream join without header buffers silently until header arrives.
- Returns `false` on parse error. Log but continue feeding — the demuxer retries on next call.
- Video stays black until first keyframe (~500ms at typical broadcast intervals).
- Never blocks. If data arrives faster than decode, excess frames are dropped with metrics.

## Audio Routing

```ts
MediaPipeline.setAudioRouteCallback((event) => {
  console.log('Route:', event.route)
  console.log('Devices:', event.availableDevices)
})

const devices = MediaPipeline.getAvailableAudioDevices()
const bt = devices.find(d => d.route === AudioRoute.BluetoothA2dp)
if (bt) MediaPipeline.setAudioRoute(AudioRoute.BluetoothA2dp, bt.deviceId)
```

Route changes are handled reactively. Connect/disconnect BT or wired headsets and the player adjusts automatically with drift compensation reset.

## Build

```sh
yarn install
yarn prepare          # Build TypeScript
yarn typecheck        # Type check
yarn lint             # Lint

# Example app
yarn example ios
yarn example android
```

### Sanitizer Tests

Requires Opus and SpeexDSP system libraries:

```sh
brew install opus speexdsp   # macOS
```

```sh
yarn test:asan   # AddressSanitizer + LeakSanitizer (27 tests)
yarn test:tsan   # ThreadSanitizer (20 tests)
yarn test:ubsan  # UndefinedBehaviorSanitizer (58 tests)
```

An additional 17 pure-C++ RouteHandler tests run with the ASan build.

### iOS Opus

```sh
cd ios/opus
./build-opus.sh clean
./build-opus.sh build   # Opus 1.6.1 XCFramework
```

## Platform Requirements

| | Android | iOS |
|---|---|---|
| Min version | API 29 (Android 10) | iOS 15.1 |
| Architecture | arm64-v8a | arm64 |
| Audio API | AAudio (LOW_LATENCY) | RemoteIO AudioUnit |
| Video API | MediaCodec (HW VP9) | VTDecompressionSession |
| C++ Standard | C++20 | C++20 |

## License

Apache-2.0
