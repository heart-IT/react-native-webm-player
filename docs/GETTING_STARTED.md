# Getting Started — @heartit/webm-player

Build a working WebM broadcast player: live VP9 video + Opus audio with health monitoring and audio routing. By the end, you have a React Native screen that plays a broadcast from any source delivering WebM bytes.

API reference: [README](../README.md). Design: [ARCHITECTURE.md](ARCHITECTURE.md).

## Prerequisites

- React Native 0.81+ with New Architecture enabled
- Xcode 15+ (iOS) or Android Studio with NDK r27 (Android)
- A source of muxed WebM data (Hypercore, HTTP, WebSocket, file, …)

---

## Step 1 — Install

```sh
yarn add @heartit/webm-player
cd ios && pod install && cd ..      # iOS only
```

Android needs no extra steps.

---

## Step 2 — Initialize JSI

Install the native JSI modules once at startup.

```tsx
// App.tsx
import { installWebmPlayer } from '@heartit/webm-player'

installWebmPlayer()
```

If the native module is missing, this throws with "WebmPlayer native module not found."

---

## Step 3 — Start and feed

Start the pipelines and feed muxed WebM bytes.

```tsx
import { MediaPipeline } from '@heartit/webm-player'

MediaPipeline.start()

function onDataReceived(chunk: ArrayBuffer) {
  const ok = MediaPipeline.feedData(chunk)
  if (!ok) console.warn('feedData parse error — continuing')
}
```

`feedData()` returns `false` on malformed data but the demuxer retries on the next call — no need to restart. The first call must include the WebM EBML header; mid-stream joins buffer silently until it arrives. Video stays black until the first VP9 keyframe (~500ms at typical broadcast intervals).

---

## Step 4 — Render video

```tsx
import { VideoView } from '@heartit/webm-player'
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

Only one `VideoView` instance is supported at a time. For the full prop reference (`scaleMode`, `mirror`), see [README › VideoView](../README.md#videoview).

---

## Step 5 — Health monitoring

Drive spinners, reconnection UI, error states from the health callback.

```tsx
import { MediaPipeline, StreamHealth, type HealthEvent } from '@heartit/webm-player'
import { useState, useEffect } from 'react'

function useBroadcastHealth() {
  const [status, setStatus] = useState<StreamHealth>(StreamHealth.Buffering)
  const [detail, setDetail] = useState('')

  useEffect(() => {
    MediaPipeline.setHealthCallback((event: HealthEvent) => {
      setStatus(event.status)
      setDetail(event.detail)
    })
    return () => MediaPipeline.setHealthCallback(null)
  }, [])

  return { status, detail }
}

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

Status transitions `Buffering → Healthy` once the first frames are decoded and playing.

---

## Step 6 — Keyframe recovery

Tell the player how to request a keyframe from your source for faster stall recovery.

```tsx
useEffect(() => {
  MediaPipeline.setKeyFrameNeededCallback(() => {
    requestKeyFrameFromSource()
  })
  return () => MediaPipeline.setKeyFrameNeededCallback(null)
}, [])
```

Without this callback, the player still recovers — it just waits for the next natural keyframe.

---

## Step 7 — Stop

```tsx
MediaPipeline.stop()   // releases decoders, audio output, buffers
```

Call `start()` again to begin a new session.

---

## Putting it together

```tsx
import React, { useEffect, useState } from 'react'
import { View, Text, StyleSheet } from 'react-native'
import {
  installWebmPlayer,
  MediaPipeline,
  VideoView,
  StreamHealth,
  type HealthEvent
} from '@heartit/webm-player'

installWebmPlayer()

export function BroadcastPlayer({ dataSource }: { dataSource: AsyncIterable<ArrayBuffer> }) {
  const [status, setStatus] = useState<StreamHealth>(StreamHealth.Buffering)

  useEffect(() => {
    MediaPipeline.setHealthCallback((event: HealthEvent) => setStatus(event.status))
    MediaPipeline.start()

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
      {status === StreamHealth.Buffering && <Text style={styles.overlay}>Buffering…</Text>}
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

## Next

- [Integration Guide](INTEGRATION_GUIDE.md) — background audio, permissions, clip capture, DVR
- [Troubleshooting](TROUBLESHOOTING.md) — diagnosing issues via metrics
- [TECHNICAL.md](../TECHNICAL.md) — constants, metrics, tests
- [ARCHITECTURE.md](ARCHITECTURE.md) — internals
- [Contributing](../CONTRIBUTING.md) — local dev setup, native test workflow, PR checklist
