# Troubleshooting — @heartit/webm-player

Symptom-driven diagnosis. Each entry: **metric to check → cause → fix**. Poll `MediaPipeline.getMetrics()` to narrow the cause.

For metric definitions see [TECHNICAL.md](../TECHNICAL.md); for design context see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Audio

### Silence or dropouts — `quality.underruns` rising

Callback has no decoded frames: feed starvation or decode failure.

1. `demux.feedDataCalls` — if stuck, data isn't arriving. Verify your source is producing.
2. `quality.decodeErrors` — if rising, Opus decode is failing (corrupt data). `resetStream()` and resume.
3. `pools.encodedUnderPressure` — decode thread is behind. Check device CPU load.
4. `stall.state === "stalled"` — wire `setKeyFrameNeededCallback` so the player can request recovery.

### Clicks or pops — `quality.framesDrained > 0`

Fast-drain discarded frames because a burst arrived too fast. Crossfade masks most artifacts.

1. Investigate bursty delivery — usually upstream batching (Hypercore read-ahead, network batching).
2. If unavoidable, raise buffer: `setBufferTarget(100, 100)` — trades latency for smooth absorption.

### Robotic / pitch-shifted — `drift.driftPpm > 200`

Excessive clock drift; drift compensator is resampling aggressively.

1. > 200 PPM is unusual — check sender clock stability.
2. If started after a route change: drift compensator should have reset. `audioOutput.restartCount` should have incremented; if not, route detection may be stuck.
3. `drift.currentRatio` should be ≈1.0 (normal 0.9995–1.005).

### Gradual latency increase — `jitter.bufferTargetUs > 100,000`

Jitter spikes inflating adaptive target; buffer grows to absorb, shrinks slowly.

1. `quality.gapsOver100ms` / `gapsOver500ms` climbing → source delivering irregularly.
2. Cap latency manually: `setBufferTarget(80, 80)` (accepts more underrun risk).
3. If Hypercore, check if read-ahead is batching blocks.

### Stuttery — `quality.ptsDiscontinuities > 0`

Non-monotonic timestamps in source; decoder resets at each jump.

- Source-side issue (encoder producing bad PTS).
- Occasional: handled automatically (reset + crossfade).
- Frequent: fix encoding side.

---

## Video

### Black screen — `video.framesReceived` and `framesDecoded` both 0

Waiting for first keyframe. VP9 inter-frames require it.

1. Wait ~500ms for next natural keyframe.
2. Call `requestKeyFrame()` explicitly.
3. Wire `setKeyFrameNeededCallback` — player requests keyframes automatically during stalls.

### Freezes — `video.lateFrames` rising

Network jitter pushing frames past render deadline.

1. `video.jitterUs` high → unstable path.
2. `setBufferTarget(60, 100)` — more video buffering, tight audio.
3. `video.skippedFrames` also rising → player is dropping stale frames to stay near live (correct behavior under jitter).

### Green / corrupt video — `video.decodeErrors > 0`

VP9 state corrupted (missing keyframe or bitstream error).

1. Auto-recovery via keyframe request. Check `video.keyFrameRequests`.
2. Persistent: `resetStream()` forces full pipeline reset.
3. `video.decoderResets` ≥3 in 5s → health watchdog reports `Failed`. Must `stop()` + `start()`.

### Lip sync off — `video.avSyncOffsetUs > 45000`

A/V clocks diverged beyond correction threshold.

1. `drift.driftPpm` high → clock drift pulling audio away. Auto-corrects; extreme drift (>1000 PPM) triggers `Degraded`.
2. Started after route change? Drift compensation resets on route change; re-converges in ~30s.
3. Sustained: `resetStream()` to re-sync both pipelines.

---

## System

### `feedData()` returns false

Malformed WebM — parse error.

1. Continue feeding; demuxer retries on next call. A single parse error does not corrupt state.
2. `demux.parseState === 0` (WaitingForEBML) → stream never got a valid EBML header. First `feedData()` call must include it.
3. Consistent false → data isn't valid WebM.

### Rising memory — `pools.decodedUnderPressure` or `encodedUnderPressure`

Decode thread behind; frames accumulating.

1. Check CPU load. HW decode shouldn't bottleneck but heavy load delays submission.
2. `health.responsive === false` → decode thread stalled. `health.watchdogTripped` confirms.
3. `health.detached === true` → unrecoverable. `stop()` + `start()`.

### Audio stops after Bluetooth connect/disconnect — `audioOutput.restartCount` rising

Audio output restart failing after route change.

1. Retry uses exp backoff (100ms–3.2s, 5 attempts). `audioOutput.lastError` non-zero after retries → platform failure.
2. Android API 31+: verify `BLUETOOTH_CONNECT` granted. Without it, BT devices don't appear in `getAvailableAudioDevices()`.
3. Verify target device is still in `getAvailableAudioDevices()` after reconnection.

### Health callback reports `Failed`

Unrecoverable: decode thread detached, audio output stopped, or repeated decoder resets.

1. `stop()` → `start()` to rebuild.
2. After restart, `resetStream()` before first `feedData()` (need EBML header again).
3. Recurring `Failed` → check device logs for platform errors (MediaCodec/VTDecompress, AAudio/AudioUnit).

---

## Stall recovery

### Stuck in "stalled" — `stall.state === "stalled"`, `stall.keyFrameRequests` rising

Requesting keyframes but none arriving.

1. Verify `setKeyFrameNeededCallback` is connected and actually asks the source.
2. Source producing at all? `setStreamStatus(StreamStatus.NoPeers)` tells the watchdog this is source-side, not player-side.
3. Source live but no keyframes → check source keyframe-interval config.

### Frequent stall/recovery cycles — `stall.stallCount` matching `stall.recoveryCount`

Intermittent gaps just above the 500ms stall threshold.

1. `quality.gapsOver500ms` trip stall detection.
2. Raise buffer to absorb longer gaps: `setBufferTarget(120, 120)`.
3. Investigate source — Hypercore peer connection unstable?

---

## Build / install

### iOS — `[CP] Copy XCFrameworks` fails with `No such file or directory`

The podspec's `prepare_command` didn't finish building `opus.xcframework` or `whisper.xcframework`. Most often `cmake` is missing or the Xcode CLI tools aren't installed.

```sh
brew install cmake
xcode-select --install

(cd ios/opus && ./build-opus.sh build)
(cd ios/whisper && ./build-whisper.sh build)
cd example/ios && bundle exec pod install
```

See [Integration Guide › Pod install builds vendored XCFrameworks](INTEGRATION_GUIDE.md#pod-install-builds-vendored-xcframeworks).

### iOS — `fmt` / `consteval` build errors locally

```
call to consteval function 'fmt::basic_format_string<...>' is not a constant expression
```

Xcode 26.x ships a stricter clang that rejects React Native 0.81's vendored `fmt` headers. **Environmental, not a project bug** — CI uses a pinned Xcode image and builds clean. To unblock locally, install Xcode 16 alongside 26 and switch:

```sh
sudo xcode-select -s /Applications/Xcode_16.app/Contents/Developer
```

### Android — `error: member access into incomplete type`

Stale `.cxx/` build cache after a header restructure. Clean and rebuild:

```sh
cd example/android && ./gradlew clean
rm -rf ../../android/.cxx
yarn example android
```

If the error persists after a clean build, a real bug — file an issue with the failing compiler invocation.

### Android — NDK version mismatch

This library requires NDK r27 (clang with C++20 support). Older NDKs miss `<concepts>` and the `std::ranges` library. Set in `example/android/local.properties`:

```
ndk.dir=/Users/you/Library/Android/sdk/ndk/27.1.12297006
```

### Bundler / Ruby — `Could not find 'bundler' (2.5.23)`

System Ruby on macOS is read-only. Install a user-level Ruby:

```sh
brew install rbenv
rbenv install 3.2.2 && rbenv global 3.2.2
gem install bundler:2.5.23
```

Restart your shell (or `exec zsh`) so the rbenv shim takes precedence over `/usr/bin/ruby`.
