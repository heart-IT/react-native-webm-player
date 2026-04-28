# Integration Guide — @heartit/webm-player

Platform-specific setup for production apps. API reference: [README](../README.md). Architecture: [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Android

### Permissions

Bluetooth routing on Android 12+ (API 31+) requires `BLUETOOTH_CONNECT`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
</manifest>
```

Request at runtime before using Bluetooth routes:

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
  return result === PermissionsAndroid.RESULTS.GRANTED
}
```

Without this permission on API 31+, Bluetooth devices won't appear in `getAvailableAudioDevices()` and `setAudioRoute()` fails silently for Bluetooth routes.

### Background playback (foreground service)

Without a foreground service, Android kills audio within seconds of backgrounding.

**Step 1 — AndroidManifest.xml**

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

**Step 2 — Service (Kotlin)**

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
            context.startForegroundService(Intent(context, PlaybackForegroundService::class.java))
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

**Step 3 — React Native bridge**

```kotlin
package com.yourapp

import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

class PlaybackServiceModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    override fun getName() = "PlaybackService"

    @ReactMethod fun startService() = PlaybackForegroundService.start(reactApplicationContext)
    @ReactMethod fun stopService()  = PlaybackForegroundService.stop(reactApplicationContext)
}
```

Register in your `ReactPackage` and add to `MainApplication.kt`.

**Step 4 — From JS**

```typescript
import { NativeModules, Platform } from 'react-native'
import { MediaPipeline } from '@heartit/webm-player'

const { PlaybackService } = NativeModules

function startPlayback() {
  if (Platform.OS === 'android') PlaybackService.startService()
  MediaPipeline.start()
}

function stopPlayback() {
  MediaPipeline.stop()
  if (Platform.OS === 'android') PlaybackService.stopService()
}
```

### ProGuard / R8

The library ships consumer ProGuard rules. If JNI methods are stripped in release:

```proguard
-keep class com.heartit.webmplayer.** { *; }
-keepclasseswithmembernames class * { native <methods>; }
```

### Battery optimization

Some OEMs (Xiaomi, Huawei, Samsung) aggressively kill foreground services. Guide users to whitelist:

```typescript
import { Linking, Platform } from 'react-native'

function openBatterySettings() {
  if (Platform.OS === 'android') Linking.openSettings()
}
```

---

## iOS

### Pod install builds vendored XCFrameworks

`pod install` runs `prepare_command` from `WebmPlayer.podspec`, which builds two XCFrameworks from source:

- `ios/opus/lib/opus.xcframework` — Opus 1.6.1
- `ios/whisper/lib/whisper.xcframework` — whisper.cpp v1.8.4

**Required tools:** `cmake` (`brew install cmake`) and Xcode CLI tools. Cold first install takes ~3–5 min; subsequent installs skip if the xcframeworks already exist.

If `pod install` fails with `[CP] Copy XCFrameworks` errors at build time, the prepare step likely didn't complete. Rebuild manually:

```sh
(cd ios/opus && ./build-opus.sh build)
(cd ios/whisper && ./build-whisper.sh build)
cd example/ios && bundle exec pod install
```

Each XCFramework ships an `ios-arm64` device slice and an `ios-arm64_x86_64-simulator` universal slice — supports both Apple Silicon and Intel simulators.

### Background audio

`Info.plist`:

```xml
<key>UIBackgroundModes</key>
<array><string>audio</string></array>
```

Without this, iOS suspends audio on background.

### Audio session

The library configures `AVAudioSession` with `.playback` and `.allowBluetoothA2DP`. If your app manages the session elsewhere, ensure the category is set before `MediaPipeline.start()`.

---

## Health monitoring

```typescript
import { MediaPipeline, StreamHealth, type HealthEvent } from '@heartit/webm-player'

MediaPipeline.setHealthCallback((event: HealthEvent) => {
  switch (event.status) {
    case StreamHealth.Buffering:
      showLoadingSpinner()
      break
    case StreamHealth.Healthy:
      hideLoadingSpinner()
      break
    case StreamHealth.Degraded:
      showQualityWarning(event.detail)
      break
    case StreamHealth.Stalled:
      showReconnecting()
      break
    case StreamHealth.Failed:
      MediaPipeline.stop()
      MediaPipeline.start()
      reconnectToSource()
      break
  }
})
```

State definitions: [TECHNICAL.md → Health watchdog](../TECHNICAL.md#health-watchdog).

---

## Stall recovery

```typescript
import { MediaPipeline, StreamStatus } from '@heartit/webm-player'

MediaPipeline.setKeyFrameNeededCallback(() => requestKeyFrameFromSource())

function onSourceDisconnected() {
  MediaPipeline.setStreamStatus(StreamStatus.NoPeers)
}
function onSourceReconnected() {
  MediaPipeline.setStreamStatus(StreamStatus.Live)
}
```

Without `setKeyFrameNeededCallback` wired, recovery still works — but must wait for the next natural keyframe (~500ms typical) instead of requesting one immediately.

---

## Audio routing

```typescript
import { MediaPipeline, AudioRoute } from '@heartit/webm-player'

MediaPipeline.setAudioRouteCallback((event) => {
  updateRouteUI(event.route, event.availableDevices)
})

function switchToDevice(route: AudioRoute, deviceId?: string) {
  MediaPipeline.setAudioRoute(route, deviceId)
}

const devices = MediaPipeline.getAvailableAudioDevices()
```

Reactive — connect/disconnect headsets and the player adjusts automatically with drift compensation reset.

---

## Clip capture

```typescript
MediaPipeline.setClipBufferDuration(60) // keep last 60s

async function captureClip(seconds: number): Promise<string> {
  return MediaPipeline.captureClip(seconds) // returns file path
}
```

Output is a standalone WebM file playable by any VP9 player, publishable as a new Hypercore for P2P sharing.

---

## DVR rewind

Seek within the buffered ring for instant replay. DVR reuses the clip buffer — enable it with `setClipBufferDuration()`.

```typescript
const available = MediaPipeline.getBufferRangeSeconds()
MediaPipeline.seekTo(-10) // rewind 10s
MediaPipeline.seekTo(0) // return to live
```

---

## Audio focus (Android)

```typescript
import { MediaPipeline, AudioFocusState } from '@heartit/webm-player'

MediaPipeline.setAudioFocusCallback((state) => {
  switch (state) {
    case AudioFocusState.Lost:
      MediaPipeline.stop()
      break
    case AudioFocusState.LostTransient:
      MediaPipeline.pause()
      break
    case AudioFocusState.LostTransientCanDuck:
      MediaPipeline.setGain(0.3)
      break
    case AudioFocusState.Gained:
      MediaPipeline.resume()
      MediaPipeline.setGain(1.0)
      break
  }
})
```

---

## Metrics polling

```typescript
function logPlaybackHealth() {
  const m = MediaPipeline.getMetrics()
  console.log({
    underruns: m.quality.underruns,
    fps: m.video.currentFps,
    avSyncUs: m.video.avSyncOffsetUs,
    driftPpm: m.drift.driftPpm,
    jitterUs: m.jitter.bufferTargetUs,
    stall: m.stall.state
  })
}

setInterval(logPlaybackHealth, 5000)
```

Full field reference: [TECHNICAL.md → Metrics](../TECHNICAL.md#metrics-reference).
