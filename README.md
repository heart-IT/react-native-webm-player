# @holepunchto/opus-native-io

Low-latency native audio and video I/O for React Native with Opus CELT codec and H.264 hardware encoding.

Provides real-time audio capture/playback and video capture/rendering optimized for voice and video communication. All processing happens in native code via JSI for minimal latency.

## Features

### Audio
- **Low-latency audio I/O** - ~20ms one-way latency (WebRTC-class)
- **Opus CELT codec** - Optimized for voice at 48kHz mono
- **Multi-participant mixing** - Up to 64 participants with 16 concurrent audio slots and automatic eviction
- **Noise suppression** - RNNoise ML-based noise reduction (enabled by default)
- **Automatic gain control** - SpeexDSP AGC for consistent volume levels
- **Audio routing** - Programmatic control of speaker/earpiece/Bluetooth

### Video
- **Hardware H.264 encode/decode** - MediaCodec (Android) / VideoToolbox (iOS)
- **VideoView component** - Native surface rendering with mirror and scale modes
- **Camera control** - Front/back switching, dynamic bitrate, key frame requests
- **A/V synchronization** - Coordinated presentation timing across audio and video streams
- **Jitter buffer** - Adaptive buffering with late-frame skip for smooth playback

### Common
- **Native processing** - Encoding, decoding, and mixing in C++ (no JS overhead)
- **JSI bindings** - Synchronous native calls, no bridge serialization

## Requirements

| Platform     | Minimum Version                   |
| ------------ | --------------------------------- |
| iOS          | 15.1+                             |
| Android      | SDK 29+ (Android 10)              |
| React Native | 0.76+ (New Architecture required) |

## Installation

```sh
npm install @holepunchto/opus-native-io
# or
yarn add @holepunchto/opus-native-io
```

### iOS

```sh
cd ios && pod install
```

Add the following to your app's `Info.plist`:

```xml
<!-- Background audio support -->
<key>UIBackgroundModes</key>
<array>
    <string>audio</string>
</array>

<!-- Camera access (required for video capture) -->
<key>NSCameraUsageDescription</key>
<string>Camera access is required for video calls</string>
```

Without `UIBackgroundModes`, iOS will suspend audio when the app is backgrounded.

### Android

No additional build steps required. The library uses CMake to build native code.

Add the camera permission to your `AndroidManifest.xml` for video capture:

```xml
<uses-permission android:name="android.permission.CAMERA" />
```

**Important:** For background audio (voice calls), you must implement a [Foreground Service](https://developer.android.com/develop/background-work/services/foreground-services). Without it, Android will kill your audio when the app is backgrounded. See the [Integration Guide](docs/INTEGRATION_GUIDE.md#background-audio-foreground-service) for complete setup instructions.

## Usage

### Audio Capture

Capture audio from the microphone, encode to Opus, and receive frames via callback:

```typescript
import { AudioCapture, type CaptureFrameResult } from '@holepunchto/opus-native-io'

// Set callback to receive encoded Opus frames
AudioCapture.setCallback((result: CaptureFrameResult) => {
  // result.buffer: ArrayBuffer containing Opus-encoded audio
  // result.byteLength: size in bytes
  sendToNetwork(result.buffer)
})

// Start capture with optional config
AudioCapture.start({
  sampleRate: 48000, // 48kHz (required for CELT)
  channels: 1, // Mono
  bitrate: 32000, // 32 kbps
  frameDurationMs: 20, // 20ms frames (960 samples)
  enableSoftwareNS: true, // RNNoise noise suppression
  enableSoftwareAGC: true // SpeexDSP automatic gain control
})

// Stop when done
AudioCapture.stop()
```

### Audio Playback

Receive Opus frames from network, decode, mix, and play:

```typescript
import { MediaPipeline } from '@holepunchto/opus-native-io'

// Start the playback pipeline
MediaPipeline.start()

// Add a participant (returns participant ID)
const participantId = MediaPipeline.addParticipant()

// Push Opus frames as they arrive from network
MediaPipeline.pushAudioFrame(
  participantId,
  opusBuffer, // ArrayBuffer or Uint8Array
  timestampUs, // Presentation timestamp in microseconds
  20000 // Frame duration in microseconds (20ms)
)

// Control individual participants
MediaPipeline.setMuted(participantId, true)
MediaPipeline.setGain(participantId, 1.5) // 0.0 to 2.0

// Remove participant when they leave
MediaPipeline.removeParticipant(participantId)

// Stop pipeline
MediaPipeline.stop()
```

### Video Capture

Capture video from the camera, encode to H.264, and receive frames via callback:

```typescript
import { VideoCapture, type VideoCaptureFrameResult } from '@holepunchto/opus-native-io'

// Set callback to receive encoded H.264 frames
VideoCapture.setCallback((result: VideoCaptureFrameResult) => {
  // result.buffer: ArrayBuffer containing H.264 NAL units
  // result.ptsUs: presentation timestamp in microseconds
  // result.isKeyFrame: true for IDR frames
  sendToNetwork(result.buffer, result.ptsUs, result.isKeyFrame)
})

// Start capture
VideoCapture.start({
  width: 1280, // Capture width
  height: 720, // Capture height
  fps: 30, // Frame rate
  bitrate: 2000000, // 2 Mbps
  keyFrameIntervalSec: 2, // IDR every 2 seconds
  camera: 'front' // 'front' or 'back'
})

// Switch camera during capture
VideoCapture.switchCamera()

// Dynamically adjust bitrate
VideoCapture.updateBitrate(1000000) // Drop to 1 Mbps

// Force a key frame (e.g., when a new participant joins)
VideoCapture.requestKeyFrame()

// Stop when done
VideoCapture.stop()
```

### Video Playback

Receive H.264 frames from network, decode, and render to native surfaces:

```typescript
import { VideoPipeline } from '@holepunchto/opus-native-io'

// Start the video pipeline
VideoPipeline.start()

// Push H.264 frames as they arrive from network
// Participants are created implicitly on first pushVideoFrame call
VideoPipeline.pushVideoFrame(
  participantId,
  h264Buffer, // ArrayBuffer or Uint8Array containing H.264 NAL units
  timestampUs, // Presentation timestamp in microseconds
  isKeyFrame // true for IDR frames
)

// Request a key frame from a participant (e.g., after packet loss)
VideoPipeline.requestKeyFrame(participantId)

// Remove participant when they leave
VideoPipeline.removeParticipant(participantId)

// Stop pipeline
VideoPipeline.stop()
```

### VideoView Component

Render a participant's decoded video in your React Native UI:

```tsx
import { VideoView } from '@holepunchto/opus-native-io'

// In your component's render:
<VideoView
  participantId={participantId}
  mirror={true}       // Mirror for self-view (front camera)
  scaleMode={1}       // 0 = fit (letterbox), 1 = fill (crop)
  style={{ width: 300, height: 200 }}
/>
```

### Audio Routing

Control audio output device:

```typescript
import { MediaPipeline, AudioRoute } from '@holepunchto/opus-native-io'
import type { AudioDeviceInfo } from '@holepunchto/opus-native-io'

// Get available devices with names and IDs
const devices: AudioDeviceInfo[] = MediaPipeline.getAvailableAudioDevices()
// [
//   { route: AudioRoute.Speaker, deviceName: 'Speaker', deviceId: '1' },
//   { route: AudioRoute.Earpiece, deviceName: 'Earpiece', deviceId: '2' },
//   { route: AudioRoute.BluetoothSco, deviceName: 'AirPods Pro', deviceId: '3' }
// ]

// Set output route by type
MediaPipeline.setAudioRoute(AudioRoute.Speaker)

// Or target a specific device when multiple of the same type are connected
MediaPipeline.setAudioRoute(AudioRoute.BluetoothSco, devices[2].deviceId)

// Get deduplicated route types (no device names/IDs)
const routeTypes: AudioRoute[] = MediaPipeline.getAvailableAudioRoutes()

// Listen for route changes (device connect/disconnect)
MediaPipeline.setAudioRouteCallback((event) => {
  console.log('Route changed to:', event.route)
  console.log('Echo mode:', event.echoMode) // 0=hardware, 1=bluetooth, 2=none
  console.log('Available devices:', event.availableDevices)
})
```

**Android 12+:** Bluetooth audio routing requires the `BLUETOOTH_CONNECT` runtime permission. See the [Integration Guide](docs/INTEGRATION_GUIDE.md#permissions) for setup.

### Metrics & Diagnostics

```typescript
// Audio capture metrics
const captureMetrics = AudioCapture.getMetrics()
console.log('Frames captured:', captureMetrics.framesCaptured)
console.log('Drop rate:', captureMetrics.dropRate)

// Audio playback metrics
const playbackMetrics = MediaPipeline.getMetrics()
console.log('Underruns:', playbackMetrics.quality.underruns)
console.log('Active participants:', playbackMetrics.session.activeParticipants)

// Per-participant audio quality
for (const p of playbackMetrics.participants) {
  console.log(
    `Participant ${p.id}: jitter=${p.jitterUs}us buffer=${p.bufferTargetUs}us stability=${p.networkStability}`
  )
}

// Video capture metrics
const videoCaptureMetrics = VideoCapture.getMetrics()
console.log('Encoded:', videoCaptureMetrics.framesEncoded, 'FPS:', videoCaptureMetrics.currentFps)

// Video playback metrics
const videoMetrics = VideoPipeline.getMetrics()
console.log('A/V sync offset:', videoMetrics.avSyncOffsetUs, 'us')
for (const p of videoMetrics.participants) {
  console.log(
    `Video ${p.id}: decoded=${p.framesDecoded} dropped=${p.framesDropped} fps=${p.currentFps}`
  )
}
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      JavaScript Layer                           │
│               (Transport only - no processing)                  │
└───────────────────────────────┬─────────────────────────────────┘
                                │ JSI (synchronous)
┌───────────────────────────────▼─────────────────────────────────┐
│                        Native Layer                             │
│  ┌─────────────────────────┐     ┌────────────────────────┐    │
│  │     AudioCapture        │     │    MediaPipeline       │    │
│  │  Mic → NS → AGC → Opus  │     │  Opus → Mix → Speaker  │    │
│  └─────────────────────────┘     └────────────────────────┘    │
│  ┌─────────────────────────┐     ┌────────────────────────┐    │
│  │     VideoCapture        │     │    VideoPipeline       │    │
│  │  Camera → H.264 Encode  │     │  H.264 → Decode →     │    │
│  │  → JS callback          │     │  Surface Render        │    │
│  └─────────────────────────┘     └────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

**Audio capture path:** Microphone → HAL → RNNoise → AGC → Opus Encoder → JS callback

**Audio playback path:** JS pushAudioFrame() → Opus Decoder → Jitter Buffer → Mixer → HAL → Speaker

**Video capture path:** Camera → Hardware H.264 Encoder → JS callback

**Video playback path:** JS pushVideoFrame() → Jitter Buffer → Hardware H.264 Decoder → Native Surface

## Audio Format

| Parameter       | Value                      |
| --------------- | -------------------------- |
| Sample rate     | 48000 Hz                   |
| Channels        | 1 (mono)                   |
| Codec           | Opus CELT (low-delay mode) |
| Frame duration  | 20ms (960 samples)         |
| Typical bitrate | 24-32 kbps                 |

## Video Format

| Parameter       | Value                                          |
| --------------- | ---------------------------------------------- |
| Codec           | H.264 (Annex B NAL units)                      |
| Encoder         | MediaCodec (Android) / VideoToolbox (iOS)      |
| Default resolution | 1280x720                                    |
| Default FPS     | 30                                             |
| Default bitrate | 2 Mbps                                         |
| Key frame interval | 2 seconds                                   |

## API Reference

### AudioCapture

| Method            | Description                                     |
| ----------------- | ----------------------------------------------- |
| `start(config?)`  | Start audio capture with optional configuration |
| `stop()`          | Stop audio capture                              |
| `isCapturing()`   | Check if capture is active                      |
| `getMetrics()`    | Get capture statistics                          |
| `setCallback(fn)` | Set callback for encoded frames                 |

### MediaPipeline

| Method                                      | Description                                           |
| ------------------------------------------- | ----------------------------------------------------- |
| `start()`                                   | Start the playback pipeline                           |
| `stop()`                                    | Stop the playback pipeline                            |
| `warmUp()`                                  | Pre-warm audio hardware for lower first-frame latency |
| `isRunning()`                               | Check if pipeline is active                           |
| `addParticipant()`                          | Add a participant, returns ID                         |
| `removeParticipant(id)`                     | Remove a participant                                  |
| `pushAudioFrame(id, buffer, pts, duration)` | Push Opus frame for decoding                          |
| `setMuted(id, muted)`                       | Mute/unmute a participant                             |
| `setGain(id, gain)`                         | Set participant volume (0.0-2.0)                      |
| `getMetrics()`                              | Get playback statistics                               |
| `getLimits()`                               | Get system limits (max participants, slots, version)  |
| `resetWatchdog()`                           | Reset pipeline health watchdog timer                  |
| `setEvictionCallback(fn)`                   | Subscribe to participant eviction events              |
| `setAudioRoute(route, deviceId?)`           | Set audio output device, optionally by device ID      |
| `getAvailableAudioRoutes()`                 | Get deduplicated list of available route types        |
| `getAvailableAudioDevices()`                | Get all devices with name, route type, and device ID  |
| `getCurrentAudioRoute()`                    | Get current active audio output device                |
| `setAudioRouteCallback(fn)`                 | Subscribe to route/device changes                     |

### VideoCapture

| Method                  | Description                                    |
| ----------------------- | ---------------------------------------------- |
| `start(config?)`        | Start video capture with optional configuration |
| `stop()`                | Stop video capture                             |
| `isCapturing()`         | Check if capture is active                     |
| `switchCamera()`        | Toggle between front and back camera           |
| `requestKeyFrame()`     | Force an IDR frame                             |
| `updateBitrate(bps)`    | Change encoding bitrate dynamically            |
| `getMetrics()`          | Get capture statistics                         |
| `setCallback(fn)`       | Set callback for encoded H.264 frames          |

### VideoPipeline

| Method                                        | Description                                    |
| --------------------------------------------- | ---------------------------------------------- |
| `start()`                                     | Start the video decode pipeline                |
| `stop()`                                      | Stop the video decode pipeline                 |
| `isRunning()`                                 | Check if pipeline is active                    |
| `pushVideoFrame(id, buffer, ptsUs, isKeyFrame)` | Push H.264 frame for decoding               |
| `removeParticipant(id)`                       | Remove a participant and release decoder       |
| `requestKeyFrame(id)`                         | Request a key frame for a participant          |
| `getMetrics()`                                | Get decode statistics and A/V sync offset      |

### VideoView

| Prop            | Type        | Description                                |
| --------------- | ----------- | ------------------------------------------ |
| `participantId` | `number`    | Participant whose video to render          |
| `mirror`        | `boolean`   | Mirror the video (default: `false`)        |
| `scaleMode`     | `0 \| 1`   | 0 = fit (letterbox), 1 = fill (crop)      |
| `style`         | `ViewStyle` | Standard React Native view style           |

## Documentation

- [Integration Guide](docs/INTEGRATION_GUIDE.md) - Platform setup, background audio/video, permissions, and troubleshooting

## License

Apache-2.0
