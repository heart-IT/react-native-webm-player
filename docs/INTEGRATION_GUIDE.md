# Integration Guide

This guide covers platform-specific setup required for production apps using `@holepunchto/opus-native-io`.

## Table of Contents

- [Android Setup](#android-setup)
  - [Background Audio (Foreground Service)](#background-audio-foreground-service)
  - [Permissions](#permissions)
  - [ProGuard / R8 Rules](#proguard--r8-rules)
  - [Battery Optimization](#battery-optimization)
- [iOS Setup](#ios-setup)
  - [Background Audio](#background-audio)
  - [Permissions](#permissions-1)
  - [Audio Session Interruptions](#audio-session-interruptions)
- [Common Integration Patterns](#common-integration-patterns)
  - [Voice Call Lifecycle](#voice-call-lifecycle)
  - [Video Call Lifecycle](#video-call-lifecycle)
  - [Capture Mute / Unmute](#capture-mute--unmute)
  - [Error Handling](#error-handling)
  - [Metrics Monitoring](#metrics-monitoring)
- [Troubleshooting](#troubleshooting)
- [CallKit Integration (iOS VoIP Apps)](#callkit-integration-ios-voip-apps)
  - [Why Use CallKit?](#why-use-callkit)
  - [How It Works](#how-it-works)
  - [Installation](#installation)
  - [Configuration](#configuration)
  - [Basic Integration](#basic-integration)
  - [Complete VoIP Call Manager with CallKit](#complete-voip-call-manager-with-callkit)
  - [Technical Details](#technical-details-why-no-library-changes-are-needed)
  - [Important Notes](#important-notes)

---

## Android Setup

### Background Audio (Foreground Service)

**This is required for voice calls.** Without a foreground service, Android will kill your audio within seconds of the app being backgrounded.

#### Why It's Needed

Android 8+ (API 26+) enforces strict background execution limits:

- Background apps are aggressively killed to save battery
- Audio streams are terminated when the app leaves the foreground
- Users will experience dropped calls when switching apps

#### Step 1: Add Permissions to AndroidManifest.xml

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <!-- Required for foreground service -->
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MICROPHONE" />

    <!-- Required for audio capture -->
    <uses-permission android:name="android.permission.RECORD_AUDIO" />

    <!-- Required for video capture -->
    <uses-permission android:name="android.permission.CAMERA" />

    <!-- Required for Bluetooth audio routing (Android 12+) -->
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />

    <!-- Optional: Keep WiFi active during calls -->
    <uses-permission android:name="android.permission.WAKE_LOCK" />

    <application ...>

        <!-- Declare the foreground service -->
        <service
            android:name=".AudioForegroundService"
            android:foregroundServiceType="microphone"
            android:exported="false" />

    </application>
</manifest>
```

#### Step 2: Create the Foreground Service (Kotlin)

```kotlin
// AudioForegroundService.kt
package com.yourapp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

class AudioForegroundService : Service() {

    companion object {
        const val CHANNEL_ID = "voice_call_channel"
        const val NOTIFICATION_ID = 1

        fun start(context: Context) {
            val intent = Intent(context, AudioForegroundService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, AudioForegroundService::class.java))
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = createNotification()
        startForeground(NOTIFICATION_ID, notification)
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Voice Call",
                NotificationManager.IMPORTANCE_LOW // Low = no sound, but visible
            ).apply {
                description = "Active voice call"
                setShowBadge(false)
            }

            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun createNotification(): Notification {
        // Intent to open app when notification is tapped
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            packageManager.getLaunchIntentForPackage(packageName),
            PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Voice Call Active")
            .setContentText("Tap to return to call")
            .setSmallIcon(R.drawable.ic_mic) // Your app's mic icon
            .setContentIntent(pendingIntent)
            .setOngoing(true) // Cannot be swiped away
            .setSilent(true) // No notification sound
            .build()
    }
}
```

#### Step 3: Create a React Native Bridge Module

```kotlin
// AudioServiceModule.kt
package com.yourapp

import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

class AudioServiceModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    override fun getName() = "AudioService"

    @ReactMethod
    fun startService() {
        val context = reactApplicationContext
        AudioForegroundService.start(context)
    }

    @ReactMethod
    fun stopService() {
        val context = reactApplicationContext
        AudioForegroundService.stop(context)
    }
}
```

#### Step 4: Register the Module

```kotlin
// AudioServicePackage.kt
package com.yourapp

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

class AudioServicePackage : ReactPackage {
    override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> {
        return listOf(AudioServiceModule(reactContext))
    }

    override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
        return emptyList()
    }
}
```

Add to your `MainApplication.kt`:

```kotlin
override fun getPackages(): List<ReactPackage> = listOf(
    MainReactPackage(),
    AudioServicePackage(), // Add this
    // ... other packages
)
```

#### Step 5: Use from JavaScript

```typescript
import { NativeModules } from 'react-native'
import { AudioCapture, MediaPipeline } from '@holepunchto/opus-native-io'

const { AudioService } = NativeModules

async function startVoiceCall() {
  // 1. Start foreground service FIRST
  AudioService.startService()

  // 2. Then start audio
  MediaPipeline.start()
  AudioCapture.start()
}

function endVoiceCall() {
  // 1. Stop audio first
  AudioCapture.stop()
  MediaPipeline.stop()

  // 2. Then stop foreground service
  AudioService.stopService()
}
```

### Permissions

Request microphone, camera, and Bluetooth permissions before starting capture:

```typescript
import { PermissionsAndroid, Platform } from 'react-native'

async function requestPermissions(includeVideo: boolean = false): Promise<boolean> {
  if (Platform.OS !== 'android') return true

  const micGranted = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.RECORD_AUDIO, {
    title: 'Microphone Permission',
    message: 'This app needs microphone access for voice calls.',
    buttonPositive: 'Allow',
    buttonNegative: 'Deny'
  })

  if (micGranted !== PermissionsAndroid.RESULTS.GRANTED) return false

  // Camera permission is required for video capture
  if (includeVideo) {
    const camGranted = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.CAMERA, {
      title: 'Camera Permission',
      message: 'This app needs camera access for video calls.',
      buttonPositive: 'Allow',
      buttonNegative: 'Deny'
    })

    if (camGranted !== PermissionsAndroid.RESULTS.GRANTED) return false
  }

  // BLUETOOTH_CONNECT is required on Android 12+ (API 31+) for Bluetooth
  // audio routing (SCO/A2DP). Without it, Bluetooth headsets won't appear
  // in getAvailableAudioRoutes() and setAudioRoute() will fail silently.
  if (Platform.Version >= 31) {
    const btGranted = await PermissionsAndroid.request(
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      {
        title: 'Bluetooth Permission',
        message: 'This app needs Bluetooth access to use wireless headsets during calls.',
        buttonPositive: 'Allow',
        buttonNegative: 'Deny'
      }
    )

    if (btGranted !== PermissionsAndroid.RESULTS.GRANTED) {
      console.warn('Bluetooth permission denied — wireless headsets will be unavailable')
    }
  }

  return true
}
```

### ProGuard / R8 Rules

If using ProGuard or R8 (release builds), add these rules to prevent JNI methods from being stripped:

```proguard
# consumer-proguard-rules.pro (already included in library)
# But if you have issues, add to your app's proguard-rules.pro:

-keep class com.holepunchto.opusnativeio.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
```

### Battery Optimization

Some OEMs (Xiaomi, Huawei, Samsung) have aggressive battery optimization that can kill foreground services. Guide users to whitelist your app:

```typescript
import { Linking, Platform } from 'react-native'

function openBatterySettings() {
  if (Platform.OS === 'android') {
    Linking.openSettings() // Opens app settings where user can disable optimization
  }
}
```

---

## iOS Setup

### Background Audio

Add to your `Info.plist`:

```xml
<key>UIBackgroundModes</key>
<array>
    <string>audio</string>
</array>
```

Without this, iOS suspends audio when the app is backgrounded.

### Permissions

Add microphone and camera usage descriptions to `Info.plist`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>This app needs microphone access for voice calls.</string>
<key>NSCameraUsageDescription</key>
<string>This app needs camera access for video calls.</string>
```

Request permissions in JavaScript:

```typescript
import { request, PERMISSIONS, RESULTS } from 'react-native-permissions'

async function requestPermissions(includeVideo: boolean = false): Promise<boolean> {
  const micResult = await request(PERMISSIONS.IOS.MICROPHONE)
  if (micResult !== RESULTS.GRANTED) return false

  if (includeVideo) {
    const camResult = await request(PERMISSIONS.IOS.CAMERA)
    if (camResult !== RESULTS.GRANTED) return false
  }

  return true
}
```

### Audio Session Interruptions

The library handles audio interruptions (phone calls, Siri) automatically, but your app should respond to focus changes:

```typescript
import { MediaPipeline } from '@holepunchto/opus-native-io'

// The library automatically pauses/resumes on interruptions.
// Use the route callback to update your UI:
MediaPipeline.setAudioRouteCallback((event) => {
  console.log('Audio route:', event.route)
  console.log('Available devices:', event.availableDevices)
})
```

---

## Common Integration Patterns

### Voice Call Lifecycle

Complete example of a voice call flow:

```typescript
import { AudioCapture, MediaPipeline, AudioRoute } from '@holepunchto/opus-native-io'
import { NativeModules, Platform } from 'react-native'

const { AudioService } = NativeModules

class VoiceCallManager {
  private participantIds: Map<string, number> = new Map()

  async startCall() {
    // 1. Request permissions
    const hasPermission = await this.requestPermissions()
    if (!hasPermission) {
      throw new Error('Microphone permission denied')
    }

    // 2. Start foreground service (Android only)
    if (Platform.OS === 'android') {
      AudioService.startService()
    }

    // 3. Set up callbacks
    AudioCapture.setCallback((result) => {
      this.sendToNetwork(result.buffer)
    })

    MediaPipeline.setEvictionCallback((event) => {
      console.warn('Participant evicted:', event.evictedParticipantId, event.reasonName)
    })

    // 4. Start audio pipeline
    MediaPipeline.warmUp() // Optional: reduces first-frame latency
    MediaPipeline.start()
    AudioCapture.start({
      enableSoftwareNS: true,
      enableSoftwareAGC: true
    })
  }

  endCall() {
    // 1. Stop audio
    AudioCapture.stop()
    AudioCapture.setCallback(null)

    // Remove all participants
    for (const [_, id] of this.participantIds) {
      MediaPipeline.removeParticipant(id)
    }
    this.participantIds.clear()

    MediaPipeline.stop()

    // 2. Stop foreground service (Android)
    if (Platform.OS === 'android') {
      AudioService.stopService()
    }
  }

  addRemoteParticipant(peerId: string): number {
    const participantId = MediaPipeline.addParticipant()
    this.participantIds.set(peerId, participantId)
    return participantId
  }

  removeRemoteParticipant(peerId: string) {
    const participantId = this.participantIds.get(peerId)
    if (participantId !== undefined) {
      MediaPipeline.removeParticipant(participantId)
      this.participantIds.delete(peerId)
    }
  }

  onAudioReceived(peerId: string, opusFrame: ArrayBuffer, timestampUs: number) {
    const participantId = this.participantIds.get(peerId)
    if (participantId !== undefined) {
      MediaPipeline.pushAudioFrame(participantId, opusFrame, timestampUs, 20000)
    }
  }

  // Mute/unmute the local microphone.
  // While muted, DTX (silence marker) frames are sent so the remote side
  // can distinguish mute from packet loss.  The mic and audio processing
  // chain stay running — unmute is instantaneous with no pipeline rebuild.
  setMuted(muted: boolean) {
    AudioCapture.setMuted(muted)
  }

  isMuted(): boolean {
    return AudioCapture.isMuted()
  }

  setSpeakerMode(enabled: boolean) {
    MediaPipeline.setAudioRoute(enabled ? AudioRoute.Speaker : AudioRoute.Earpiece)
  }

  private sendToNetwork(buffer: ArrayBuffer) {
    // Implement your network transport here
  }

  private async requestPermissions(): Promise<boolean> {
    // Platform-specific permission request
    return true
  }
}
```

### Video Call Lifecycle

Adding video to an audio call. Video can be started and stopped independently of audio:

```typescript
import {
  AudioCapture,
  MediaPipeline,
  VideoCapture,
  VideoPipeline,
  VideoView
} from '@holepunchto/opus-native-io'

class VideoCallManager {
  private participantIds: Map<string, number> = new Map()

  async startCall(withVideo: boolean = true) {
    // 1. Start audio (same as voice call)
    MediaPipeline.start()
    AudioCapture.setCallback((result) => {
      this.sendAudioToNetwork(result.buffer)
    })
    AudioCapture.start({ enableSoftwareNS: true, enableSoftwareAGC: true })

    // 2. Optionally start video
    if (withVideo) {
      this.startVideo()
    }
  }

  startVideo() {
    // Start the video decode pipeline for remote participants
    VideoPipeline.start()

    // Start local camera capture
    VideoCapture.setCallback((result) => {
      this.sendVideoToNetwork(result.buffer, result.ptsUs, result.isKeyFrame)
    })
    VideoCapture.start({
      width: 1280,
      height: 720,
      fps: 30,
      bitrate: 2000000,
      camera: 'front'
    })
  }

  stopVideo() {
    VideoCapture.stop()
    VideoCapture.setCallback(null)
    VideoPipeline.stop()
  }

  endCall() {
    // Stop video first, then audio
    this.stopVideo()
    AudioCapture.stop()
    AudioCapture.setCallback(null)
    for (const [_, id] of this.participantIds) {
      MediaPipeline.removeParticipant(id)
    }
    this.participantIds.clear()
    MediaPipeline.stop()
  }

  // Push received video frames — participants are created implicitly
  onVideoReceived(peerId: string, h264Frame: ArrayBuffer, ptsUs: number, isKeyFrame: boolean) {
    const participantId = this.getParticipantId(peerId)
    VideoPipeline.pushVideoFrame(participantId, h264Frame, ptsUs, isKeyFrame)
  }

  onAudioReceived(peerId: string, opusFrame: ArrayBuffer, timestampUs: number) {
    const participantId = this.getParticipantId(peerId)
    MediaPipeline.pushAudioFrame(participantId, opusFrame, timestampUs, 20000)
  }

  switchCamera() {
    VideoCapture.switchCamera()
  }

  private getParticipantId(peerId: string): number {
    let id = this.participantIds.get(peerId)
    if (id === undefined) {
      id = MediaPipeline.addParticipant()
      this.participantIds.set(peerId, id)
    }
    return id
  }

  private sendAudioToNetwork(buffer: ArrayBuffer) { /* ... */ }
  private sendVideoToNetwork(buffer: ArrayBuffer, ptsUs: number, isKeyFrame: boolean) { /* ... */ }
}
```

Render remote video in your component:

```tsx
import { VideoView } from '@holepunchto/opus-native-io'

function RemoteVideo({ participantId }: { participantId: number }) {
  return (
    <VideoView
      participantId={participantId}
      scaleMode={1}  // fill (crop to fit)
      style={{ width: '100%', aspectRatio: 16 / 9 }}
    />
  )
}

function LocalPreview({ participantId }: { participantId: number }) {
  return (
    <VideoView
      participantId={participantId}
      mirror={true}  // Mirror for self-view
      scaleMode={1}
      style={{ width: 120, height: 160, position: 'absolute', right: 16, top: 16 }}
    />
  )
}
```

### Capture Mute / Unmute

Use `AudioCapture.setMuted()` to mute the local microphone during a call. This is a lightweight operation — the mic, AEC, and audio processing chain stay running so unmute is instantaneous with no pipeline rebuild.

```typescript
import { AudioCapture } from '@holepunchto/opus-native-io'

// Mute — remote side receives DTX (comfort noise), not silence/packet-loss
AudioCapture.setMuted(true)

// Unmute — audio resumes immediately, no glitches
AudioCapture.setMuted(false)

// Query current state
const muted = AudioCapture.isMuted()
```

**How it works:**

| State   | What happens locally                          | What the remote side hears |
| ------- | --------------------------------------------- | -------------------------- |
| Unmuted | Mic → RNNoise → AGC → Opus encode → network   | Normal audio               |
| Muted   | Mic → RNNoise → AGC → **DTX frame** → network | Comfort noise (~-60dBFS)   |

**Key behaviors:**

- **DTX frames** are sent while muted so the remote decoder can distinguish mute from packet loss (PLC would produce audible artifacts; DTX produces clean comfort noise)
- The **audio processing chain stays warm** — RNNoise and AGC continue processing mic input, avoiding state discontinuities on unmute
- **Mute state resets on `stop()`** — if you call `AudioCapture.stop()` then `AudioCapture.start()`, mute resets to `false`
- **Metrics stay healthy** — DTX frames are counted in `framesEncoded`, so `isHealthy()` won't false-alarm during mute
- **Do not use `stop()`/`start()` to simulate mute** — that tears down the entire audio pipeline (AudioUnit/AAudio, encoder thread, ring buffer) and can cause timing issues or audio loss on restart

### Error Handling

```typescript
import { MediaPipeline, AudioCapture } from '@holepunchto/opus-native-io'

// Monitor for issues
function monitorAudioHealth() {
  setInterval(() => {
    const metrics = MediaPipeline.getMetrics()

    // Check for underruns (audio glitches)
    if (metrics.quality.underruns > 0) {
      console.warn('Audio underruns detected:', metrics.quality.underruns)
    }

    // Check decode thread health
    if (!metrics.health.responsive) {
      console.error('Decode thread unresponsive - audio may be stuck')
      // Consider restarting the pipeline
    }

    // Check for dropped frames
    const captureMetrics = AudioCapture.getMetrics()
    if (captureMetrics.framesDropped > 0) {
      console.warn('Capture frames dropped:', captureMetrics.framesDropped)
    }
  }, 5000)
}
```

### Metrics Monitoring

```typescript
interface AudioQualityMetrics {
  captureDropRate: number
  playbackUnderruns: number
  activeParticipants: number
  participantHealth: Array<{
    id: number
    jitterUs: number
    bufferTargetUs: number
    networkStability: number
  }>
}

function getAudioQuality(): AudioQualityMetrics {
  const capture = AudioCapture.getMetrics()
  const playback = MediaPipeline.getMetrics()

  return {
    captureDropRate: capture.dropRate,
    playbackUnderruns: playback.quality.underruns,
    activeParticipants: playback.session.activeParticipants,
    participantHealth: playback.participants.map((p) => ({
      id: p.id,
      jitterUs: p.jitterUs,
      bufferTargetUs: p.bufferTargetUs,
      networkStability: p.networkStability
    }))
  }
}
```

---

## Troubleshooting

### Android: Audio stops in background

**Cause:** Foreground service not running or killed by OEM battery optimization.

**Solution:**

1. Ensure foreground service is started before audio
2. Check that `foregroundServiceType="microphone"` is set in manifest
3. Guide users to disable battery optimization for your app

### iOS: Audio stops when screen locks

**Cause:** Missing `UIBackgroundModes` in Info.plist.

**Solution:** Add `audio` to `UIBackgroundModes` array.

### Echo on speaker phone

**Cause:** Hardware AEC not active.

**Solution:** The library uses VoiceProcessingIO (iOS) and VOICE_COMMUNICATION mode (Android) which enable hardware AEC. If echo persists, ensure:

- AVAudioSession category is `playAndRecord` with mode `voiceChat`
- On Android, audio is using `AAUDIO_USAGE_VOICE_COMMUNICATION`

### High latency

**Cause:** Jitter buffer compensating for network jitter.

**Solution:**

1. Check `avgJitterBufferMs` in metrics
2. Reduce network jitter if possible
3. Use `warmUp()` before `start()` to reduce first-frame latency

### Bluetooth audio not working

**Cause:** Bluetooth permissions or routing not configured.

**Solution:**

1. iOS: Ensure `AVAudioSessionCategoryOptionAllowBluetooth` is set (library does this)
2. Android 12+ (API 31+): Ensure `BLUETOOTH_CONNECT` permission is declared in the manifest and granted at runtime — without it, the system blocks all Bluetooth audio device access
3. Android: Bluetooth audio routes automatically when connected
4. Use `getAvailableAudioDevices()` to check if Bluetooth devices are detected
5. Use `setAudioRoute(AudioRoute.BluetoothSco)` to force Bluetooth, or pass a specific `deviceId` from `getAvailableAudioDevices()`

### Video: Black screen / no video rendering

**Cause:** VideoView not linked to the correct participant, or no key frame received yet.

**Solution:**

1. Ensure the `participantId` prop on `<VideoView>` matches the ID used in `pushVideoFrame()`
2. The first frame pushed must be a key frame (IDR) — the decoder cannot start from a P-frame
3. Call `VideoPipeline.requestKeyFrame(participantId)` to request a new IDR from the sender
4. Check `VideoPipeline.getMetrics()` — if `framesReceived > 0` but `framesDecoded === 0`, the decoder is waiting for SPS/PPS parameter sets

### Video: Choppy or low FPS playback

**Cause:** Network jitter causing late frames, or encoder bitrate too high for available bandwidth.

**Solution:**

1. Check `VideoCapture.getMetrics().currentFps` to verify the sender is encoding at expected FPS
2. Check per-participant metrics: `lateFrames` and `skippedFrames` indicate jitter buffer pressure
3. Lower bitrate with `VideoCapture.updateBitrate()` if bandwidth is limited
4. The jitter buffer adapts automatically — `bufferTargetUs` in metrics shows the current target

### Video: Camera permission denied

**Cause:** Missing permission declaration or runtime permission not granted.

**Solution:**

1. Android: Add `<uses-permission android:name="android.permission.CAMERA" />` to AndroidManifest.xml and request at runtime
2. iOS: Add `NSCameraUsageDescription` to Info.plist
3. `VideoCapture.start()` returns `false` if camera access is unavailable

---

## CallKit Integration (iOS VoIP Apps)

For production VoIP apps that need native iOS call UI integration, you can use CallKit via [react-native-callkeep](https://github.com/react-native-webrtc/react-native-callkeep). **No library changes are required** - the library is fully compatible with CallKit-managed audio sessions.

### Why Use CallKit?

CallKit provides:

- Native incoming call UI (full-screen or banner)
- Call history in the Phone app
- Siri integration ("Call John on MyApp")
- CarPlay / Bluetooth car display integration
- Do Not Disturb / Focus mode integration
- Background execution during active calls

### How It Works

When CallKit is active:

1. **CallKit manages AVAudioSession** - It activates/deactivates the audio session
2. **Library operations are idempotent** - Calling `setCategory` or `setActive:YES` when already configured is safe
3. **Interruption handling aligns** - Both CallKit and the library respond to the same iOS notifications

### Installation

```bash
npm install react-native-callkeep
cd ios && pod install
```

### Configuration

#### Info.plist

```xml
<key>UIBackgroundModes</key>
<array>
    <string>audio</string>
    <string>voip</string>
</array>
```

#### AppDelegate.m

```objc
#import "RNCallKeep.h"

- (BOOL)application:(UIApplication *)application
    continueUserActivity:(NSUserActivity *)userActivity
      restorationHandler:(void (^)(NSArray<id<UIUserActivityRestoring>> *))restorationHandler
{
    return [RNCallKeep application:application
              continueUserActivity:userActivity
                restorationHandler:restorationHandler];
}
```

### Basic Integration

```typescript
import RNCallKeep from 'react-native-callkeep'
import { AudioCapture, MediaPipeline } from '@holepunchto/opus-native-io'
import { Platform } from 'react-native'

// Initialize CallKeep once at app startup
async function setupCallKeep() {
  if (Platform.OS !== 'ios') return

  await RNCallKeep.setup({
    ios: {
      appName: 'MyApp',
      supportsVideo: false,
      maximumCallGroups: 1,
      maximumCallsPerCallGroup: 1
    }
  })

  // Register event handlers
  RNCallKeep.addEventListener('answerCall', onAnswerCall)
  RNCallKeep.addEventListener('endCall', onEndCall)
  RNCallKeep.addEventListener('didActivateAudioSession', onAudioSessionActivated)
  RNCallKeep.addEventListener('didDeactivateAudioSession', onAudioSessionDeactivated)
}

// Display incoming call (typically triggered by push notification)
function displayIncomingCall(callId: string, callerName: string) {
  RNCallKeep.displayIncomingCall(
    callId, // Unique call UUID
    callerName, // Display name
    callerName, // Localized caller name
    'generic', // Handle type
    false // Has video
  )
}

// Start outgoing call
function startOutgoingCall(callId: string, callerName: string) {
  RNCallKeep.startCall(callId, callerName, callerName, 'generic', false)
}

// Event handlers
function onAnswerCall({ callUUID }: { callUUID: string }) {
  console.log('Call answered:', callUUID)
  // Audio session will be activated by CallKit
  // Connect to your signaling/peer
}

function onEndCall({ callUUID }: { callUUID: string }) {
  console.log('Call ended:', callUUID)
  stopAudio()
}

function onAudioSessionActivated() {
  // CallKit has activated the audio session
  // NOW safe to start audio capture/playback
  console.log('Audio session activated by CallKit')
  startAudio()
}

function onAudioSessionDeactivated() {
  // CallKit has deactivated the audio session
  // MUST stop audio here
  console.log('Audio session deactivated by CallKit')
  stopAudio()
}

// Audio control functions
function startAudio() {
  MediaPipeline.start()
  AudioCapture.start()
}

function stopAudio() {
  AudioCapture.stop()
  MediaPipeline.stop()
}
```

### Complete VoIP Call Manager with CallKit

```typescript
import RNCallKeep from 'react-native-callkeep'
import { AudioCapture, MediaPipeline, AudioRoute } from '@holepunchto/opus-native-io'
import { Platform, NativeModules } from 'react-native'
import { v4 as uuidv4 } from 'uuid'

const { AudioService } = NativeModules

interface Call {
  id: string
  peerId: string
  participantId?: number
  isOutgoing: boolean
}

class VoIPCallManager {
  private activeCall: Call | null = null
  private audioStarted = false

  async initialize() {
    if (Platform.OS === 'ios') {
      await this.setupCallKeep()
    }
  }

  private async setupCallKeep() {
    await RNCallKeep.setup({
      ios: {
        appName: 'MyApp',
        supportsVideo: false,
        maximumCallGroups: 1,
        maximumCallsPerCallGroup: 1
      }
    })

    RNCallKeep.addEventListener('answerCall', this.onAnswerCall)
    RNCallKeep.addEventListener('endCall', this.onEndCall)
    RNCallKeep.addEventListener('didActivateAudioSession', this.onAudioActivated)
    RNCallKeep.addEventListener('didDeactivateAudioSession', this.onAudioDeactivated)
    RNCallKeep.addEventListener('didPerformSetMutedCallAction', this.onMuteToggle)
  }

  // Called when receiving an incoming call (e.g., from push notification)
  async handleIncomingCall(peerId: string, callerName: string) {
    const callId = uuidv4()

    this.activeCall = {
      id: callId,
      peerId,
      isOutgoing: false
    }

    if (Platform.OS === 'ios') {
      // Show native iOS call UI
      RNCallKeep.displayIncomingCall(callId, callerName, callerName, 'generic', false)
    } else {
      // Android: Start foreground service and audio immediately
      // (or show your own incoming call UI)
      AudioService.startService()
      this.startAudioPipeline()
    }
  }

  // Called when user initiates an outgoing call
  async startOutgoingCall(peerId: string, callerName: string) {
    const callId = uuidv4()

    this.activeCall = {
      id: callId,
      peerId,
      isOutgoing: true
    }

    if (Platform.OS === 'ios') {
      RNCallKeep.startCall(callId, callerName, callerName, 'generic', false)
      // Audio will start when didActivateAudioSession fires
    } else {
      AudioService.startService()
      this.startAudioPipeline()
    }

    // Signal to peer that we're calling
    this.signalOutgoingCall(peerId)
  }

  // End the current call
  endCall() {
    if (!this.activeCall) return

    if (Platform.OS === 'ios') {
      RNCallKeep.endCall(this.activeCall.id)
      // Audio will stop when didDeactivateAudioSession fires
    } else {
      this.stopAudioPipeline()
      AudioService.stopService()
    }

    this.signalCallEnded(this.activeCall.peerId)
    this.activeCall = null
  }

  // Push audio from network to playback
  onAudioReceived(peerId: string, opusFrame: ArrayBuffer, timestampUs: number) {
    if (!this.activeCall || this.activeCall.peerId !== peerId) return
    if (this.activeCall.participantId === undefined) return

    MediaPipeline.pushAudioFrame(
      this.activeCall.participantId,
      opusFrame,
      timestampUs,
      20000 // 20ms frame duration
    )
  }

  // Audio routing
  setSpeakerEnabled(enabled: boolean) {
    MediaPipeline.setAudioRoute(enabled ? AudioRoute.Speaker : AudioRoute.Earpiece)
  }

  // CallKit event handlers
  private onAnswerCall = ({ callUUID }: { callUUID: string }) => {
    if (this.activeCall?.id !== callUUID) return
    console.log('Call answered via CallKit')
    // Audio session will be activated shortly
    this.signalCallAnswered(this.activeCall.peerId)
  }

  private onEndCall = ({ callUUID }: { callUUID: string }) => {
    if (this.activeCall?.id !== callUUID) return
    console.log('Call ended via CallKit')
    // Audio will be deactivated shortly
    this.signalCallEnded(this.activeCall.peerId)
    this.activeCall = null
  }

  private onAudioActivated = () => {
    // CallKit has activated the audio session - NOW start audio
    console.log('CallKit activated audio session')
    this.startAudioPipeline()
  }

  private onAudioDeactivated = () => {
    // CallKit has deactivated the audio session - MUST stop audio
    console.log('CallKit deactivated audio session')
    this.stopAudioPipeline()
  }

  private onMuteToggle = ({ muted, callUUID }: { muted: boolean; callUUID: string }) => {
    if (this.activeCall?.id !== callUUID) return
    // Mute/unmute capture from the native CallKit UI.
    // Sends DTX frames while muted so the remote decoder outputs comfort noise.
    AudioCapture.setMuted(muted)
    console.log('Mute toggled:', muted)
  }

  private startAudioPipeline() {
    if (this.audioStarted) return
    this.audioStarted = true

    // Set up capture callback
    AudioCapture.setCallback((result) => {
      if (this.activeCall) {
        this.sendAudioToNetwork(this.activeCall.peerId, result.buffer)
      }
    })

    // Start playback and add remote participant
    MediaPipeline.start()
    if (this.activeCall) {
      this.activeCall.participantId = MediaPipeline.addParticipant()
    }

    // Start capture
    AudioCapture.start({
      enableSoftwareNS: true,
      enableSoftwareAGC: true
    })
  }

  private stopAudioPipeline() {
    if (!this.audioStarted) return
    this.audioStarted = false

    AudioCapture.stop()
    AudioCapture.setCallback(null)

    if (this.activeCall?.participantId !== undefined) {
      MediaPipeline.removeParticipant(this.activeCall.participantId)
    }
    MediaPipeline.stop()
  }

  // Implement these for your signaling layer
  private signalOutgoingCall(peerId: string) {
    /* ... */
  }
  private signalCallAnswered(peerId: string) {
    /* ... */
  }
  private signalCallEnded(peerId: string) {
    /* ... */
  }
  private sendAudioToNetwork(peerId: string, buffer: ArrayBuffer) {
    /* ... */
  }
}

export const callManager = new VoIPCallManager()
```

### Technical Details: Why No Library Changes Are Needed

The library is compatible with CallKit because:

1. **AVAudioSession operations are idempotent**
   - `setCategory:mode:options:` with same values is a no-op
   - `setActive:YES` when already active returns success
   - CallKit uses the same category (`PlayAndRecord`) and mode (`VoiceChat`)

2. **Interruption handling aligns with CallKit**
   - Both respond to `AVAudioSessionInterruptionNotification`
   - Library pauses on interruption began, resumes on ended
   - CallKit's `didActivateAudioSession` fires after interruption ends

3. **Background lifecycle is handled correctly**
   - CallKit grants background execution during active calls
   - Library pauses capture on background, resumes on foreground
   - Brief pause/resume is seamless for users

### Important Notes

1. **Start audio only after `didActivateAudioSession`**
   - Don't start audio in `answerCall` - the session isn't ready yet
   - Wait for CallKit to signal that audio is available

2. **Stop audio immediately on `didDeactivateAudioSession`**
   - Don't delay - iOS expects audio to stop
   - The library handles this gracefully

3. **Android doesn't use CallKit**
   - Use your own incoming call UI or a library like `react-native-push-notification`
   - Start foreground service before audio

4. **PushKit for VoIP pushes (iOS)**
   - Use PushKit to receive incoming call notifications
   - You **must** report a call to CallKit when receiving a VoIP push
   - Failure to do so will cause iOS to terminate your app

### CallKit vs Non-CallKit Comparison

| Aspect            | Without CallKit    | With CallKit              |
| ----------------- | ------------------ | ------------------------- |
| Incoming call UI  | Custom (your app)  | Native iOS call screen    |
| Background audio  | Pauses capture     | Full audio (during call)  |
| Phone app history | Not shown          | Shows in Recents          |
| Siri integration  | None               | "Call X on MyApp"         |
| CarPlay           | Manual integration | Automatic                 |
| Do Not Disturb    | Ignores calls      | Respects emergency bypass |
| Audio session     | Library manages    | CallKit manages           |
