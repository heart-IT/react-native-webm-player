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

#### Lifecycle

| Method | Returns | Description |
|--------|---------|-------------|
| `start()` | `boolean` | Start playback pipelines |
| `stop()` | `boolean` | Stop and release resources |
| `isRunning()` | `boolean` | Check if pipelines are active |
| `warmUp()` | `boolean` | Pre-start audio output with silence for zero-latency first frame |
| `pause()` | `boolean` | Pause playback (holds last frame, silences audio) |
| `resume()` | `boolean` | Resume from pause |
| `isPaused()` | `boolean` | Check if paused |
| `getPlaybackState()` | `PlaybackState` | Current state: Idle, Buffering, Playing, Paused, Stalled, Failed |
| `getCurrentTimeUs()` | `number` | Current playback position in microseconds |

#### Stream

| Method | Returns | Description |
|--------|---------|-------------|
| `feedData(buffer)` | `boolean` | Feed muxed WebM bytes. Returns `false` on parse error |
| `resetStream()` | `boolean` | Reset demuxer + pipelines for a new stream |
| `requestKeyFrame()` | `boolean` | Request video keyframe recovery upstream |
| `getTrackInfo()` | `TrackInfo \| null` | Codec IDs + video dimensions from demuxed tracks |
| `setStreamStatus(status)` | `boolean` | Set stream context (Live, Buffering, Ended, NoPeers) — enriches health events |

#### Audio

| Method | Returns | Description |
|--------|---------|-------------|
| `setMuted(muted)` | `boolean` | Mute/unmute audio |
| `setGain(gain)` | `boolean` | Set audio gain (0.0 - 2.0) |
| `setPlaybackRate(rate)` | `boolean` | Set playback speed (e.g. 1.0 = normal, 1.5 = 1.5x) |
| `setAudioRoute(route, deviceId?)` | `boolean` | Switch audio output device |
| `getAvailableAudioRoutes()` | `AudioRoute[]` | List available route types |
| `getAvailableAudioDevices()` | `AudioDeviceInfo[]` | List devices with names and IDs |
| `getCurrentAudioRoute()` | `AudioRoute` | Get current output route |

#### Callbacks

| Method | Returns | Description |
|--------|---------|-------------|
| `setAudioRouteCallback(cb)` | `boolean` | Register for audio route change events |
| `setHealthCallback(cb)` | `boolean` | Register for health state changes |
| `setKeyFrameNeededCallback(cb)` | `boolean` | Register for keyframe request events |
| `setAudioFocusCallback(cb)` | `boolean` | Register for audio focus events (Android) |

#### Tuning

| Method | Returns | Description |
|--------|---------|-------------|
| `setBufferTarget(audioMs, videoMs)` | `boolean` | Override jitter buffer targets |
| `setCatchupPolicy(policy)` | `boolean` | Set behavior when player falls behind live |

#### Clip / DVR

| Method | Returns | Description |
|--------|---------|-------------|
| `setClipBufferDuration(seconds)` | `boolean` | Set ring buffer size for clip capture (enables native buffering) |
| `captureClip(lastNSeconds)` | `Promise<string>` | Capture last N seconds as a WebM file, returns file path |
| `seekTo(offsetSeconds)` | `boolean` | Seek within the buffered range (negative = rewind from live) |
| `getBufferRangeSeconds()` | `number` | Get how many seconds are available in the ring buffer |

#### Metrics

| Method | Returns | Description |
|--------|---------|-------------|
| `getMetrics()` | `PlayerMetrics` | Full playback health snapshot (see below) |

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

### Enums

#### `AudioRoute`

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

#### `PlaybackState`

```ts
enum PlaybackState {
  Idle = 0,       // Not started
  Buffering = 1,  // Waiting for initial data
  Playing = 2,    // Active playback
  Paused = 3,     // Paused by user
  Stalled = 4,    // Data flow interrupted
  Failed = 5      // Unrecoverable
}
```

#### `StreamHealth`

```ts
enum StreamHealth {
  Healthy = 0,    // All metrics normal
  Buffering = 1,  // Initial buffering or rebuffering
  Degraded = 2,   // Playing with quality loss
  Stalled = 3,    // No data flowing
  Failed = 4      // Unrecoverable — requires stop/start
}
```

#### `CatchupPolicy`

```ts
enum CatchupPolicy {
  PlayThrough = 0,  // Play everything, fall behind if needed
  Accelerate = 1,   // Speed up playback to catch live edge
  DropToLive = 2    // Skip to live edge, dropping intermediate data
}
```

#### `StreamStatus`

```ts
enum StreamStatus {
  Live = 0,       // Actively receiving data
  Buffering = 1,  // Source is buffering
  Ended = 2,      // Stream ended normally
  NoPeers = 3     // No peers available (P2P)
}
```

#### `AudioFocusState`

Android only — iOS handles audio session interruptions automatically.

```ts
enum AudioFocusState {
  Gained = 0,              // Audio focus regained
  Lost = 1,                // Permanent loss (another app took focus)
  LostTransient = 2,       // Temporary loss (phone call)
  LostTransientCanDuck = 3 // Can continue at reduced volume
}
```

#### `VideoDecoderState`

Exposed via `getMetrics().video.decoderState` for triage diagnostics.

```ts
enum VideoDecoderState {
  NotCreated = 0,     // Decoder not initialized
  WaitingSurface = 1, // Waiting for render surface
  BackingOff = 2,     // Retrying after error
  Active = 3,         // Decoding frames
  Failed = 4          // Unrecoverable decoder error
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

// Quality
m.quality.underruns            // Audio callback had no data
m.quality.framesDropped        // Frames dropped (overflow/late)
m.quality.decodeErrors         // Opus decode failures

// Video
m.video.framesDecoded          // VP9 frames decoded
m.video.currentFps             // Current render FPS
m.video.avSyncOffsetUs         // A/V sync offset (audio = master)
m.video.needsKeyFrame          // Waiting for keyframe

// Drift
m.drift.driftPpm               // Clock drift in PPM
m.drift.active                 // Drift compensator active

// Jitter
m.jitter.bufferTargetUs        // Adaptive jitter buffer target

// Audio levels
m.levels.peakDbfs              // Peak level in dBFS
m.levels.rmsDbfs               // RMS level in dBFS
m.levels.clipCount             // Clipping events

// Stall recovery
m.stall.state                  // healthy | detecting | stalled | recovering | failed
m.stall.stallCount             // Total stalls
m.stall.recoveryCount          // Successful recoveries
m.stall.keyFrameRequests       // Keyframes requested for recovery

// Demuxer
m.demux.totalBytesFed          // Total bytes fed to demuxer
m.demux.audioPacketsEmitted    // Audio packets extracted
m.demux.videoPacketsEmitted    // Video packets extracted

// Pipeline
m.pipeline.bufferedDurationUs  // Audio buffered ahead of playback
```

See [`PlayerMetrics`](src/index.tsx) for the full metrics interface.

## Clip Capture

Capture a clip from the live stream without re-encoding. The native ring buffer stores raw WebM data for instant extraction.

```ts
// Enable clip buffering (call before or after start)
MediaPipeline.setClipBufferDuration(60) // keep last 60 seconds

// Capture a clip
const filePath = await MediaPipeline.captureClip(15) // last 15 seconds
// filePath is a local file path to a valid .webm file
```

## DVR Rewind

Seek within the buffered ring buffer for instant rewind/replay.

```ts
const available = MediaPipeline.getBufferRangeSeconds() // e.g. 45.2

MediaPipeline.seekTo(-10) // rewind 10 seconds from live
MediaPipeline.seekTo(0)   // return to live edge
```

## Audio Level Metering

Drive a VU meter or voice activity indicator from native-measured audio levels.

```ts
const m = MediaPipeline.getMetrics()
const { peakDbfs, rmsDbfs, clipCount } = m.levels
// peakDbfs/rmsDbfs are negative (0 = full scale), clipCount tracks clipping
```

## Stall Recovery

The player detects and recovers from stalls automatically:

1. **Detecting** — no new frames for a threshold period
2. **Stalled** — confirmed stall, requests keyframe via `setKeyFrameNeededCallback`
3. **Recovering** — keyframe received, rebuilding decode state
4. **Healthy** — playback resumed

Set `setStreamStatus()` to give the health watchdog context (e.g. `NoPeers` vs `Live`) so it can distinguish network issues from source-side pauses.

```ts
MediaPipeline.setKeyFrameNeededCallback(() => {
  // signal upstream to send a keyframe
  requestKeyFrameFromSource()
})

// inform the player why data stopped
MediaPipeline.setStreamStatus(StreamStatus.NoPeers)
```

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

## Documentation

| Document | Purpose |
|----------|---------|
| [Getting Started](docs/GETTING_STARTED.md) | Tutorial: build a broadcast player step by step |
| [Integration Guide](docs/INTEGRATION_GUIDE.md) | Platform setup: background audio, permissions, clip capture, DVR |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Diagnose audio/video issues using metrics |
| [Technical Reference](TECHNICAL.md) | Timing constants, metrics fields, threading model, test coverage |
| [Architecture](docs/ARCHITECTURE.md) | Design rationale, data flow, and subsystem internals |
| [Contributing](CONTRIBUTING.md) | Development workflow and pull request guidelines |

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
yarn test:asan   # AddressSanitizer + LeakSanitizer
yarn test:tsan   # ThreadSanitizer
yarn test:ubsan  # UndefinedBehaviorSanitizer
```

263 native tests across 7 test binaries. See [`tests/sanitizer/README.md`](tests/sanitizer/README.md) for details.

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
