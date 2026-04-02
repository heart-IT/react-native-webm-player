# Integration Guide — @holepunchto/webm-player

Platform-specific setup for production apps using the WebM broadcast player.

For API reference, see the [README](../README.md). For architecture context, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Android Setup

### Permissions

Add Bluetooth permission for wireless headset routing:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <!-- Required for Bluetooth audio routing on Android 12+ (API 31+) -->
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
</manifest>
```

Request at runtime before using Bluetooth audio routes:

```typescript
import { PermissionsAndroid, Platform } from 'react-native'

async function requestBluetoothPermission(): Promise<boolean> {
  if (Platform.OS !== 'android' || Platform.Version < 31) return true

  const result = await PermissionsAndroid.request(
    PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    {
      title: 'Bluetooth Permission',
      message: 'Allow Bluetooth access to use wireless headsets during playback.',
      buttonPositive: 'Allow',
      buttonNegative: 'Deny'
    }
  )

  if (result !== PermissionsAndroid.RESULTS.GRANTED) {
    console.warn('Bluetooth permission denied — wireless headsets will be unavailable')
    return false
  }
  return true
}
```

Without this permission on API 31+, `getAvailableAudioDevices()` won't list Bluetooth devices and `setAudioRoute()` will fail silently for Bluetooth routes.

### Background Playback (Foreground Service)

If your app needs to continue broadcast playback while backgrounded, create a foreground service. Without one, Android kills audio within seconds of backgrounding.

#### Step 1: Add to AndroidManifest.xml

```xml
<manifest>
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK" />

    <application>
        <service
            android:name=".PlaybackForegroundService"
            android:foregroundServiceType="mediaPlayback"
            android:exported="false" />
    </application>
</manifest>
```

#### Step 2: Create the service (Kotlin)

```kotlin
package com.yourapp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat

class PlaybackForegroundService : Service() {

    companion object {
        const val CHANNEL_ID = "broadcast_playback_channel"
        const val NOTIFICATION_ID = 1

        fun start(context: Context) {
            val intent = Intent(context, PlaybackForegroundService::class.java)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, PlaybackForegroundService::class.java))
        }
    }

    override fun onCreate() {
        super.onCreate()
        val channel = NotificationChannel(
            CHANNEL_ID, "Broadcast Playback", NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Active broadcast playback"
            setShowBadge(false)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            packageManager.getLaunchIntentForPackage(packageName),
            PendingIntent.FLAG_IMMUTABLE
        )
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Broadcast Playing")
            .setContentText("Tap to return")
            .setSmallIcon(R.drawable.ic_play)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setSilent(true)
            .build()
        startForeground(NOTIFICATION_ID, notification)
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
```

#### Step 3: Create a React Native bridge

```kotlin
package com.yourapp

import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

class PlaybackServiceModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    override fun getName() = "PlaybackService"

    @ReactMethod
    fun startService() {
        PlaybackForegroundService.start(reactApplicationContext)
    }

    @ReactMethod
    fun stopService() {
        PlaybackForegroundService.stop(reactApplicationContext)
    }
}
```

Register in your `ReactPackage` and add to `MainApplication.kt`.

#### Step 4: Use from JavaScript

```typescript
import { NativeModules, Platform } from 'react-native'
import { MediaPipeline } from '@holepunchto/webm-player'

const { PlaybackService } = NativeModules

function startPlayback() {
  if (Platform.OS === 'android') {
    PlaybackService.startService()
  }
  MediaPipeline.start()
}

function stopPlayback() {
  MediaPipeline.stop()
  if (Platform.OS === 'android') {
    PlaybackService.stopService()
  }
}
```

### ProGuard / R8 Rules

The library includes consumer ProGuard rules. If JNI methods are stripped in release builds, add:

```proguard
-keep class com.holepunchto.webmplayer.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
```

### Battery Optimization

Some OEMs (Xiaomi, Huawei, Samsung) aggressively kill foreground services. Guide users to whitelist your app:

```typescript
import { Linking, Platform } from 'react-native'

function openBatterySettings() {
  if (Platform.OS === 'android') {
    Linking.openSettings()
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

### Audio Session

The library configures `AVAudioSession` with category `.playback` and option `.allowBluetoothA2DP`. If your app manages the audio session elsewhere, ensure the category is set before calling `MediaPipeline.start()`.

---

## How to Set Up Health Monitoring

Wire the health callback to respond to playback state changes:

```typescript
import { MediaPipeline, StreamHealth, type HealthEvent } from '@holepunchto/webm-player'

MediaPipeline.setHealthCallback((event: HealthEvent) => {
  switch (event.status) {
    case StreamHealth.Buffering:
      showLoadingSpinner()
      break

    case StreamHealth.Healthy:
      hideLoadingSpinner()
      break

    case StreamHealth.Degraded:
      // Playback continues but quality is reduced
      showQualityWarning(event.detail)
      break

    case StreamHealth.Stalled:
      showReconnecting()
      break

    case StreamHealth.Failed:
      // Unrecoverable — must restart pipeline
      MediaPipeline.stop()
      MediaPipeline.start()
      reconnectToSource()
      break
  }
})
```

For health state definitions and transition conditions, see [TECHNICAL.md](../TECHNICAL.md#health-watchdog-states).

---

## How to Set Up Stall Recovery

Wire the keyframe callback so the player can recover from feed stalls:

```typescript
import { MediaPipeline, StreamStatus } from '@holepunchto/webm-player'

// Let the player request keyframes when it needs them
MediaPipeline.setKeyFrameNeededCallback(() => {
  requestKeyFrameFromSource()
})

// Tell the player why data stopped (helps health watchdog)
function onSourceDisconnected() {
  MediaPipeline.setStreamStatus(StreamStatus.NoPeers)
}

function onSourceReconnected() {
  MediaPipeline.setStreamStatus(StreamStatus.Live)
}
```

If `setKeyFrameNeededCallback` is not wired up, the player still recovers — but it must wait for the next natural keyframe in the stream (~500ms at typical broadcast intervals) instead of requesting one immediately.

---

## How to Set Up Audio Routing

React to route changes and allow users to switch outputs:

```typescript
import { MediaPipeline, AudioRoute } from '@holepunchto/webm-player'

// Monitor route changes (Bluetooth connect/disconnect, headset plug)
MediaPipeline.setAudioRouteCallback((event) => {
  updateRouteUI(event.route, event.availableDevices)
})

// Switch to a specific device
function switchToDevice(route: AudioRoute, deviceId?: string) {
  MediaPipeline.setAudioRoute(route, deviceId)
}

// List available devices for a picker UI
function getDevices() {
  return MediaPipeline.getAvailableAudioDevices()
}
```

Route changes are handled reactively — connect or disconnect Bluetooth or wired headsets and the player adjusts automatically with drift compensation reset.

---

## How to Set Up Clip Capture

Enable the native ring buffer and capture clips:

```typescript
import { MediaPipeline } from '@holepunchto/webm-player'

// Enable clip buffering (call before or after start)
MediaPipeline.setClipBufferDuration(60) // keep last 60 seconds

// Capture a clip — returns a local file path to a valid .webm file
async function captureClip(seconds: number): Promise<string> {
  const filePath = await MediaPipeline.captureClip(seconds)
  return filePath
}
```

The output is a standalone WebM file playable by any VP9-capable player. It can be published as a new Hypercore for P2P sharing.

---

## How to Set Up DVR Rewind

Seek within the buffered ring buffer for instant replay:

```typescript
import { MediaPipeline } from '@holepunchto/webm-player'

// Check available rewind range
const available = MediaPipeline.getBufferRangeSeconds()

// Rewind 10 seconds from live
MediaPipeline.seekTo(-10)

// Return to live edge
MediaPipeline.seekTo(0)
```

DVR reuses the clip buffer — call `setClipBufferDuration()` to enable it.

---

## How to Handle Audio Focus (Android)

Respond to Android audio focus events (other apps playing audio, phone calls):

```typescript
import { MediaPipeline, AudioFocusState } from '@holepunchto/webm-player'

MediaPipeline.setAudioFocusCallback((state) => {
  switch (state) {
    case AudioFocusState.Lost:
      // Another app took audio focus permanently
      MediaPipeline.stop()
      break
    case AudioFocusState.LostTransient:
      // Temporary loss (phone call) — pause and resume later
      MediaPipeline.pause()
      break
    case AudioFocusState.LostTransientCanDuck:
      // Can continue at lower volume
      MediaPipeline.setGain(0.3)
      break
    case AudioFocusState.Gained:
      // Focus returned
      MediaPipeline.resume()
      MediaPipeline.setGain(1.0)
      break
  }
})
```

---

## How to Monitor Metrics

Poll metrics periodically for dashboards or debugging:

```typescript
import { MediaPipeline } from '@holepunchto/webm-player'

function logPlaybackHealth() {
  const m = MediaPipeline.getMetrics()

  console.log('Audio underruns:', m.quality.underruns)
  console.log('Video FPS:', m.video.currentFps)
  console.log('A/V sync offset:', m.video.avSyncOffsetUs, 'us')
  console.log('Clock drift:', m.drift.driftPpm, 'PPM')
  console.log('Jitter buffer:', m.jitter.bufferTargetUs, 'us')
  console.log('Stall state:', m.stall.state)
}

// Poll every 5 seconds
setInterval(logPlaybackHealth, 5000)
```

For the full metrics field reference, see [TECHNICAL.md](../TECHNICAL.md#metrics-reference).
