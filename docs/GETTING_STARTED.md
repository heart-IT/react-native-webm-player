# Getting Started — @holepunchto/webm-player

In this tutorial, we will build a working WebM broadcast player that renders live VP9 video with Opus audio, monitors playback health, and handles audio routing. By the end, you will have a React Native screen that plays a broadcast stream from any source that delivers WebM bytes.

For API details, see the [README](../README.md). For architecture, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Prerequisites

- A React Native 0.81+ project with New Architecture enabled
- Xcode 15+ (iOS) or Android Studio with NDK r27 (Android)
- A source of muxed WebM data (Hypercore, HTTP, WebSocket, file, etc.)

---

## Step 1: Install the library

```sh
yarn add @holepunchto/webm-player
```

For iOS, install the CocoaPods dependencies:

```sh
cd ios && pod install && cd ..
```

Android requires no extra steps.

---

## Step 2: Initialize the JSI modules

Before using any API, install the native JSI modules. Do this once at app startup.

```tsx
// App.tsx
import { installWebmPlayer } from '@holepunchto/webm-player'

installWebmPlayer()
```

If the native module is missing (library not linked correctly), this throws an error. You will see a red screen with the message "WebmPlayer native module not found."

---

## Step 3: Start the player and feed data

Now we start the playback pipeline and feed it WebM bytes. The player handles all demuxing, decoding, and rendering internally.

```tsx
import { MediaPipeline } from '@holepunchto/webm-player'

// Start both audio and video pipelines
MediaPipeline.start()

// Feed muxed WebM data as it arrives from your source
function onDataReceived(chunk: ArrayBuffer) {
  const ok = MediaPipeline.feedData(chunk)
  if (!ok) {
    console.warn('feedData parse error — continuing')
  }
}
```

Notice that `feedData()` returns a boolean. A `false` return means the data was malformed, but the demuxer continues on the next call — you do not need to restart.

The first call to `feedData()` must include the WebM EBML header. If you join a stream mid-way, the player buffers silently until the header arrives.

Video will appear black until the first VP9 keyframe arrives (typically within ~500ms of a broadcast).

---

## Step 4: Render video

Add the `VideoView` component to display decoded VP9 frames:

```tsx
import { VideoView } from '@holepunchto/webm-player'
import { View, StyleSheet } from 'react-native'

function BroadcastPlayer() {
  return (
    <View style={styles.container}>
      <VideoView scaleMode={0} style={styles.video} />
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000' },
  video: { width: '100%', aspectRatio: 16 / 9 }
})
```

You will see the video appear once the first keyframe is decoded. `scaleMode={0}` fits the video within the view (letterbox). Use `scaleMode={1}` to fill the view (crop).

Only one `VideoView` instance is supported at a time.

---

## Step 5: Add health monitoring

The health callback tells you when playback state changes — use it to show loading spinners, reconnection UI, or error states.

```tsx
import { MediaPipeline, StreamHealth, type HealthEvent } from '@holepunchto/webm-player'
import { useState, useEffect } from 'react'

function useBroadcastHealth() {
  const [status, setStatus] = useState<StreamHealth>(StreamHealth.Buffering)
  const [detail, setDetail] = useState('')

  useEffect(() => {
    MediaPipeline.setHealthCallback((event: HealthEvent) => {
      setStatus(event.status)
      setDetail(event.detail)
    })
    return () => {
      MediaPipeline.setHealthCallback(null)
    }
  }, [])

  return { status, detail }
}
```

Now use it in your player component:

```tsx
function BroadcastPlayer() {
  const { status, detail } = useBroadcastHealth()

  return (
    <View style={styles.container}>
      <VideoView scaleMode={0} style={styles.video} />
      {status === StreamHealth.Buffering && <LoadingSpinner />}
      {status === StreamHealth.Stalled && <ReconnectingBanner />}
      {status === StreamHealth.Failed && <ErrorScreen detail={detail} />}
    </View>
  )
}
```

You will see the status transition from `Buffering` → `Healthy` once the first frames are decoded and playing.

---

## Step 6: Wire up keyframe recovery

For faster recovery from network stalls, tell the player how to request a keyframe from your source:

```tsx
useEffect(() => {
  MediaPipeline.setKeyFrameNeededCallback(() => {
    // Replace with your source's keyframe request mechanism
    requestKeyFrameFromSource()
  })
  return () => {
    MediaPipeline.setKeyFrameNeededCallback(null)
  }
}, [])
```

Without this callback, the player still recovers — it just waits for the next natural keyframe instead of requesting one immediately.

---

## Step 7: Stop playback

When the user leaves the broadcast:

```tsx
function stopPlayback() {
  MediaPipeline.stop()
}
```

This releases all native resources (decoders, audio output, buffers). Call `start()` again to begin a new session.

---

## Putting it all together

Here is a complete minimal broadcast player:

```tsx
import React, { useEffect, useState } from 'react'
import { View, Text, StyleSheet } from 'react-native'
import {
  installWebmPlayer,
  MediaPipeline,
  VideoView,
  StreamHealth,
  type HealthEvent
} from '@holepunchto/webm-player'

// Install JSI modules once
installWebmPlayer()

export function BroadcastPlayer({ dataSource }: { dataSource: AsyncIterable<ArrayBuffer> }) {
  const [status, setStatus] = useState<StreamHealth>(StreamHealth.Buffering)

  useEffect(() => {
    MediaPipeline.setHealthCallback((event: HealthEvent) => {
      setStatus(event.status)
    })

    MediaPipeline.start()

    // Feed data from the source
    const controller = new AbortController()
    ;(async () => {
      for await (const chunk of dataSource) {
        if (controller.signal.aborted) break
        MediaPipeline.feedData(chunk)
      }
    })()

    return () => {
      controller.abort()
      MediaPipeline.setHealthCallback(null)
      MediaPipeline.stop()
    }
  }, [dataSource])

  return (
    <View style={styles.container}>
      <VideoView scaleMode={0} style={styles.video} />
      {status === StreamHealth.Buffering && (
        <Text style={styles.overlay}>Buffering...</Text>
      )}
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000' },
  video: { width: '100%', aspectRatio: 16 / 9 },
  overlay: { position: 'absolute', top: 16, left: 16, color: '#fff', fontSize: 14 }
})
```

---

## Next Steps

- [Integration Guide](INTEGRATION_GUIDE.md) — platform-specific production setup (background audio, permissions, clip capture, DVR)
- [Troubleshooting](TROUBLESHOOTING.md) — diagnosing audio/video issues using metrics
- [TECHNICAL.md](../TECHNICAL.md) — timing constants, metrics reference, test coverage
- [ARCHITECTURE.md](ARCHITECTURE.md) — how the pipeline works internally
