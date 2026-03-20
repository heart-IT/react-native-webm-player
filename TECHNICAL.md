# Technical Reference — @holepunchto/webm-player

Internals, architecture, data flow, timing model, and testing guide for developers integrating or debugging the WebM broadcast player.

---

## Architecture

```
Hypercore (read-only) → Bare Thread → RPC (muxed WebM bytes)
    ↓
JavaScript — feedData() forwarding only (no processing, no buffering)
    ↓ JSI
WebmDemuxer (shared C++) — incremental parse → AudioPacket[] + VideoPacket[]
    ↓                                    ↓
Audio Pipeline                     Video Pipeline
  AudioDecodeChannel:                VideoFrameQueue:
  Opus → JitterBuffer →             VP9 HW Decode → JitterBuffer →
  DriftComp → Mixer →               VideoSyncController →
  AAudio / RemoteIO →               MediaCodec Surface / VideoView →
  Speaker                           Screen
    ↑                                    ↑
    └──────── AVSyncCoordinator ─────────┘
              (audio = master clock)
```

### Layer Responsibilities

| Layer | Does | Does NOT |
|-------|------|----------|
| **Bare Thread** | Hypercore read, delivers muxed WebM bytes | Demuxing, audio/video processing |
| **JavaScript** | Receives muxed WebM via callback, calls `feedData()` | Buffering, DSP, timing, pacing, demuxing |
| **Native (Demuxer)** | Parses WebM container, extracts Opus + VP9 packets | Decoding, playback |
| **Native Audio** | Opus decode, jitter buffer, drift comp, mixer, routing | Capture, encode, network I/O |
| **Native Video** | VP9 HW decode, render sync, surface management | Encode, network I/O |

### Invariants

- JS passes muxed WebM bytes — native handles all demuxing.
- Audio callbacks only touch PCM. No codec, no locks, no allocations.
- JS has no timing authority — native controls all pacing.
- Audio is the master clock for A/V sync.
- Single broadcast stream — one audio track, one video track.
- Receive-only player — no capture, no encoding, no TX path.

---

## Data Flow

### feedData() Path

```
JS: feedData(webmBytes)
  → JSI: extractBuffer() (zero-copy or single-copy)
  → WebmDemuxer::feedData()
    → StreamReader::append() (append to internal buffer)
    → Incremental parse (fallthrough state machine)
    → Return DemuxResult { AudioPacket[], VideoPacket[], error }
  → Route AudioPackets → MediaSession::pushAudioFrame() → AudioDecodeChannel
  → Route VideoPackets → VideoFrameQueue::pushEncodedFrame()
  → Return bool to JS (false if parse error)
```

### Audio Pipeline

```
feedData() → demux → AudioDecodeChannel::pushEncodedFrame()
  → PendingAudioPool (16 slots, pre-allocated)
  → PendingAudioQueue (SPSC: JS thread → decode thread)
  → DecodeThread: processPendingDecode()
    → Pool token acquired BEFORE popping encoded queue (no frame loss)
    → Opus decode → DecodedAudioFrame (960 samples, 20ms)
    → DecodedAudioQueue (SPSC: decode thread → audio callback)
  → Audio callback: readSamples() (LOCK-FREE)
    → Drift compensation (Speex resampler if ratio != 1.0)
    → Crossfade on PLC→real transitions
    → Fast-drain with crossfade for burst backlogs
  → AudioMixer: gain application (NEON/SSE SIMD)
  → Platform audio output (AAudio / RemoteIO)
```

### Video Pipeline

```
feedData() → demux → VideoFrameQueue::pushEncodedFrame()
  → Keyframe gating (drops non-keyframes until first keyframe)
  → Mutex-protected deque (max 8 frames)
  → VideoDecodeThread: tryPopReadyFrame()
    → VideoRenderClock: wait until scheduled render time
    → Skip stale frames (late by > half frame interval)
  → VP9 HW decode (MediaCodec / VTDecompressionSession)
  → Surface render (ANativeWindow / AVSampleBufferDisplayLayer)
  → AVSyncCoordinator: report video render PTS
```

### A/V Sync

Audio is the master clock. `AVSyncCoordinator` tracks the offset between audio and video render timestamps:

| Offset | Action |
|--------|--------|
| < 15ms | No correction (ITU-R BT.1359 dead zone) |
| 15-45ms | Gradual proportional correction |
| > 45ms | Aggressive full correction |

Video adjusts its buffer target based on the sync offset. Audio never adjusts for video.

---

## Demuxer State Machine

```
                    feedData() with EBML magic
WaitingForEBML ──────────────────────────────► WaitingForSegment
     ↑                                                │
     │ reset()                      Segment created   │
     │                                                ↓
  [any state]                                   ParsingTracks
                                                      │
                                     Tracks loaded    │
                                     (A_OPUS + V_VP9) ↓
                                                  Streaming
                                                 (extracting blocks)
```

- **Incremental parsing:** Each state attempts parse and falls through on success. If more data needed, returns empty result (no error).
- **Buffer compaction:** Discards consumed bytes before current cluster to prevent unbounded memory growth.
- **Packet lifetime:** Packets in `DemuxResult` point into demuxer's internal buffer. Valid until next `feedData()` or `reset()` call. Both pipelines copy immediately.

---

## Threading Model

| Thread | Period | Blocking Budget | Responsibility |
|--------|--------|-----------------|----------------|
| JS (feedData) | Bursty (Hypercore-driven) | Unbounded (non-RT) | Feed WebM data, setup, metrics |
| Audio Decode | 500us base, backoff to 16ms | Must keep decoded queue > 0 | Opus decode, PLC generation |
| Video Decode | 2ms fixed poll | Must submit before render time | VP9 HW decode submission |
| Audio Callback | ~10-20ms (hardware-driven) | **Zero** — RT, no blocking | PCM read from lock-free queue |
| Video Render | VSync-driven | Must not stall UI thread | Surface present |

### SPSC Queue Contract

All audio queues are **Single-Producer Single-Consumer** with `acquire`/`release` memory ordering:

| Queue | Producer | Consumer | Clear |
|-------|----------|----------|-------|
| `encodedQueue_` (audio) | JS thread (push) | Decode thread (pop) | Deferred via `encodedClearRequested_` atomic flag |
| `decodedQueue_` (audio) | Decode thread (push) | Audio callback (pop) | Deferred via `decodedClearRequested_` atomic flag |

`clear()` is implemented as `while (pop()) {}` — always runs on the consumer thread. Cross-thread clear uses an atomic flag checked by the consumer.

### Lock-Free Audio Callback

`readSamples()` on the audio callback thread uses:
- Atomic loads for state checks
- SPSC queue pops (lock-free)
- `memcpy` for PCM data
- Atomic counter increments for metrics

**Forbidden on audio callback:** mutexes, allocations, logging, JNI calls, ObjC message sends, `std::string`, `std::function` creation, `try/catch`.

---

## Timing Constants

### Audio

| Constant | Value | Purpose |
|----------|-------|---------|
| Sample rate | 48,000 Hz | Native Opus rate |
| Channels | 1 (mono) | Broadcast voice |
| Frame duration | 20ms (960 samples) | Opus frame size |
| Jitter buffer default | 60ms (3 frames) | Adaptive target (40-200ms range) |
| Max buffered | 400ms (20 frames) | Hard ceiling — drop above this |
| PLC threshold | 40ms (2 frames) | Proactive PLC when < 2 decoded frames remain |
| Catchup threshold | 40ms excess | Activate 5% speedup above this |
| Fast-drain threshold | 100ms excess | Discard frames with crossfade above this |
| Max speedup | 1.05x | Catchup resampler cap |
| Max consecutive PLC | 8 frames (160ms) | Decoder reset after this |
| PTS forward jump | 500ms | Triggers decoder reset |
| Decode pool | 16 slots | Pre-allocated decoded frame pool |
| Decode queue | 8 slots | SPSC decoded frame queue |
| Encoded pool | 16 slots | Pre-allocated encoded frame pool |

### Video

| Constant | Value | Purpose |
|----------|-------|---------|
| Jitter buffer default | 66ms (2 frames @ 30fps) | Adaptive target (33-200ms) |
| Late frame threshold | 16.7ms | Half-frame — skip if this late |
| Re-anchor interval | 30s | Render clock re-anchor period |
| Queue depth | 8 frames | Encoded video queue cap |
| Max encoded frame | 512 KB | 4K keyframe worst case |

### A/V Sync

| Constant | Value | Purpose |
|----------|-------|---------|
| Dead zone | 15ms | No correction below this |
| Emergency threshold | 45ms | Full correction above this |
| Correction range | 30ms (15-45ms) | Proportional correction band |
| EWMA alpha | 1/8 | Offset smoothing factor |

### Drift Compensation

| Constant | Value | Purpose |
|----------|-------|---------|
| Min drift | 50 PPM | Activate only above this |
| Max drift | 5,000 PPM | Cap at 0.5% |
| Measurement window | 30s | PPM calculation window |
| Max combined ratio | 1.1025 | drift(1.05) x catchup(1.05) absolute max |

---

## Jitter Buffer

Adaptive EWMA estimator with spike detection and burst memory.

### Algorithm

1. **Arrival jitter:** Track inter-packet arrival time vs. expected interval
2. **EWMA smoothing:** `jitter = alpha * sample + (1-alpha) * jitter` (alpha = 0.125)
3. **Spike detection:** If deviation > 3x current estimate, bypass EWMA and jump immediately
4. **Spike hold:** Maintain elevated target for 2 seconds after spike
5. **Burst memory:** Track recurring spike patterns (ring buffer of 4 recent spikes). If 2+ spikes in 30s window, maintain a floor at 75% of minimum recent spike jitter
6. **Buffer target:** `max(jitter * 2.5, minBuffer, burstFloor)` clamped to `[40ms, 200ms]`

### Drift Tracking

The jitter estimator also tracks long-term clock drift between sender and receiver:
- Measures cumulative sample-count vs. wall-clock delta over 30s windows
- Reports drift as PPM to `DriftCompensator`
- Drift compensation activates only above 50 PPM threshold

---

## Stream Recovery

### Keyframe Gating (Video)

`VideoFrameQueue` starts with `needsKeyFrame_ = true`. All non-keyframe VP9 packets are dropped until a keyframe arrives.

- **Stream start:** Video is black until first keyframe (~500ms)
- **After decode error:** `requestKeyFrame()` clears queue and re-enables gating
- **Upstream signal:** `setKeyFrameNeededCallback()` notifies JS when native needs a keyframe. JS can request one from the Hypercore source to reduce recovery time below the natural keyframe interval (~500ms).

### PTS Discontinuity (Audio)

When `AudioDecodeChannel` detects a PTS jump > 500ms forward or > 200ms backward:
1. Sets `needsDecoderReset_` flag
2. Decode thread resets Opus decoder on next cycle
3. Crossfade prevents click at splice point
4. `ptsDiscontinuities` metric incremented

### Feed Starvation

If JS stops calling `feedData()`:
- **Audio:** Transitions to Underrun state, outputs silence, PLC conceals gaps up to 160ms
- **Video:** Holds last rendered frame
- **Recovery:** When data resumes, decoder resets if gap > 500ms; crossfade prevents click

### Backpressure

| Pipeline | Mechanism | Limit | On overflow |
|----------|-----------|-------|-------------|
| Audio | Buffer duration ceiling | 400ms | Drop frame, increment `framesDropped` |
| Audio | Encoded pool exhaustion | 16 slots | Drop frame |
| Video | Queue depth cap | 8 frames | Drop oldest frame |

---

## Audio Routing

Reactive model — zero state machines, zero timers. `RouteHandler` (shared C++) receives OS route change events and decides: dedup, restart, drift reset, JS callback.

### iOS

```
AVAudioSessionRouteChangeNotification → serial GCD queue →
  detectCurrentRoute() → RouteHandler::onRouteDetected() →
  [dedup | restart | drift reset | JS callback]
```

Category: `.playback` with `.allowBluetoothA2DP` option.

### Android

```
Kotlin AudioSessionBridge → JNI nativeOnAudioRouteChanged →
  RouteHandler::onRouteDetected() →
  [dedup | restart | drift reset | JS callback]
```

Uses `setCommunicationDevice()` on API 31+, legacy `setSpeakerphoneOn/startBluetoothSco` below.

### Restart Rules

| Transition | Restart? | Reason |
|------------|----------|--------|
| Same route | No | No change |
| Unknown → anything | No | Initial detection |
| Speaker ↔ Earpiece | No | Same hardware path |
| A2DP ↔ Speaker/Earpiece | No | Both output-only |
| All other transitions | **Yes** | Different hardware path |

---

## Platform Audio Bridges

### iOS: AVAudioOutputBridge

- **AudioUnit:** RemoteIO (`kAudioUnitSubType_RemoteIO`), output-only
- **Format:** float32, mono, at hardware sample rate
- **Resampling:** Speex resampler when hardware rate != 48kHz (common on BT)
- **Callback drain:** Tracks in-flight callbacks via atomic counter. Waits for drain before AudioUnit disposal.
- **Restart:** Dedicated thread with exponential backoff (100ms base, 3200ms max, 5 retries)
- **Warm-up:** Pre-starts AudioUnit with silence for zero-latency first-frame

### Android: AAudioOutputBridge

- **API:** AAudio with `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
- **Sharing:** Exclusive mode preferred, falls back to Shared if unavailable or sample rate mismatch
- **Format:** float32, mono, 48kHz requested
- **Resampling:** Speex resampler when AAudio grants different sample rate
- **Error callback:** Stores error (no I/O on RT thread), schedules restart via atomic flag
- **Restart:** Dedicated thread with same backoff strategy as iOS

### Resample Buffer Sizing

Both platforms pre-allocate source buffers for worst-case: 4096 hardware frames at 6x upsample ratio (8kHz → 48kHz) = 24,576 samples. The render callback clamps to buffer capacity as defense-in-depth.

---

## Health Watchdog

State machine in `cpp/common/HealthWatchdog.h`, wired through `DecodeThread` → `MediaSession` → `MediaPipelineModule` (both platforms). Uses `weak_from_this` + `CallInvoker::invokeAsync` for JS dispatch.

### API

```typescript
enum StreamHealth {
  Healthy = 0,      // All metrics normal
  Buffering = 1,    // Initial buffering or rebuffering after gap
  Degraded = 2,     // Playback continuing with quality loss
  Stalled = 3,      // No data flowing, PLC/frame-hold active
  Failed = 4        // Unrecoverable — requires stop/start
}

interface HealthEvent {
  status: StreamHealth
  detail: string           // Human-readable (e.g., "audio underrun: feed gap > 500ms")
  metrics: {
    underruns: number
    framesDropped: number
    avSyncOffsetUs: number
    bufferTargetUs: number
    gapsOver100ms: number
  }
}

type HealthCallback = (event: HealthEvent) => void
// On MediaPipeline: setHealthCallback(callback: HealthCallback | null): boolean
```

### States

| State | Trigger | Recovery |
|-------|---------|----------|
| **Buffering** | First `feedData()` or underrun after Healthy | Automatic when buffer fills |
| **Healthy** | Audio playing + video rendering + sync < 45ms | — |
| **Degraded** | Sustained underruns (5+/window) or decode errors (3+/window) | Feed quality improvement |
| **Stalled** | Feed gap > 500ms | Resume feeding |
| **Failed** | Decode thread detached or audio output stopped | Requires `stop()` + `start()` |

### Trigger Conditions

Events fire on **transitions only** — not repeatedly while in a state.

| Transition | Condition | Detail |
|-----------|-----------|--------|
| → Buffering | First `feedData()` after `start()`, before audio plays | "initial buffering" |
| → Buffering | `underruns` increments after being in Healthy | "rebuffering: audio underrun" |
| → Healthy | Audio playing + video rendering + `avSyncOffsetUs` < 45ms | "playback recovered" |
| → Degraded | `avSyncOffsetUs` > 45ms sustained for > 2s | "a/v sync degraded" |
| → Degraded | `decodeErrors` (video) rising + `framesDropped` > 10% | "video decode degraded" |
| → Degraded | `driftPpm` > 1000 | "excessive clock drift" |
| → Stalled | `gapsOver500ms` increments (no data for 500ms+) | "feed stalled" |
| → Stalled | Video `framesReceived` stops incrementing for > 1s | "video feed stalled" |
| → Failed | Audio output `lastError` non-zero + restart retries exhausted | "audio output failed" |
| → Failed | `decoderResets` > 3 in 10 seconds | "decoder failing repeatedly" |

### Evaluation Window

- 5-second sliding window for delta computation
- Compares current metrics snapshot against window-start baseline
- Only fires on state *transition*, not repeatedly while in a state

### Implementation Notes

- **Thread:** Health checks run on the decode thread (already periodic). Callback dispatched to JS thread via `CallInvoker::invokeAsync`.
- **Dedup:** Only fire on state transition. Track `currentHealth_` atomic. Compare-and-swap before firing.
- **Lifecycle:** Same `weak_from_this` + null-check pattern as route callback. Clear in `uninstall()`.
- **No RT-thread work:** Health evaluation reads atomics only. No allocation, no locks.
- **Rate limit:** Maximum one event per 500ms to prevent JS thread flooding during cascading failures.

---

## Testing

### Automated (CI)

| Test | Command | Tests |
|------|---------|-------|
| AddressSanitizer + LeakSanitizer | `yarn test:asan` | 27 |
| ThreadSanitizer | `yarn test:tsan` | 20 |
| UndefinedBehaviorSanitizer | `yarn test:ubsan` | 58 |
| RouteHandler (pure C++) | Runs with ASan build | 17 |
| TypeScript build | `yarn prepare` | — |
| Type check | `yarn typecheck` | — |
| Lint | `yarn lint` | — |

Total: **122 native tests** across 4 test suites.

### What the Tests Cover

**LeakSanitizer (test_lsan.cpp):** FramePool acquire/release/RAII, FrameQueue SPSC push/pop/clear, JitterEstimator lifecycle, DriftCompensator/Speex resampler, CatchupController, AudioMixer, DecodeThread start/stop, OpusDecoderAdapter init/decode/PLC/move, pool stress cycles.

**ThreadSanitizer (test_tsan.cpp):** Concurrent FramePool acquire/release (4 threads), SPSC producer/consumer (1000 frames), JitterEstimator concurrent update/read with spike detection, DriftCompensator concurrent update+read, CatchupController bidirectional, AudioDecodeChannel concurrent push+read+decode, AudioMixer concurrent access, DecodeThread concurrent push, StreamMetrics concurrent counters, VideoFrameQueue concurrent push/pop and reset-during-push, VideoJitterEstimator and VideoSurfaceRegistry concurrent access.

**UndefinedBehaviorSanitizer (test_ubsan.cpp):** Pool boundary conditions (exhaustion, double-release, access-after-release), JitterEstimator arithmetic (zero interval, large deviation, negative timestamps, overflow guards), DriftCompensator division-by-zero/extreme-drift/negative-drift, Speex resampler extreme ratios (0.5-2.0), CatchupController no-slowdown/speedup-to-unity, AudioMixer null/zero/gain-ramp, AudioDecodeChannel inactive-read/push/zero-size/buffer-target-extremes, OpusDecoderAdapter null/uninitialized/DTX/garbage/zero-size, AudioResampler passthrough/max-output/float, VideoFrameQueue keyframe/oversize/null/overflow, VideoJitterEstimator/RenderClock edge cases.

**RouteHandler (test_route_handler.cpp):** Initial route detection, dedup same-route, route change with callback+restart, builtin transitions (no restart), A2DP transitions (no restart), SCO/Wired/USB transitions (restart), unknown route handling, drift reset behavior, reset/reinitialize, comprehensive `needsRestart()` matrix, multiple sequential route changes.

### Manual (Pre-Release)

| Test | Requirement |
|------|-------------|
| Device matrix | 3+ Android OEMs, 2+ iOS devices |
| A/V sync | < 45ms offset sustained |
| 4-hour stability | No memory growth, no crashes |
| Power consumption | < 8%/hr (screen on) |
| Bluetooth routing | Connect/disconnect/reconnect A2DP |
| Background execution | Audio continues in background |
| Route changes | Speaker ↔ BT ↔ Wired transitions |
| Video quality | No persistent artifacts, keyframe recovery |

### Running Tests Locally

```sh
# Prerequisites
brew install opus speexdsp

# ASan (memory errors + leaks)
cd tests/sanitizer
cmake -B build -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/test_lsan
./build/test_ubsan
./build/test_route_handler

# TSan (data races)
cmake -B build_tsan -DSANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tsan -j --target test_tsan
./build_tsan/test_tsan
```

### Writing New Tests

Tests use a simple static-registration pattern:

```cpp
#include <cstdio>

struct Test { const char* name; bool (*fn)(); };
static Test tests[512];
static int testCount = 0;

#define TEST(name) \
  static bool test_##name(); \
  static struct Register_##name { \
    Register_##name() { tests[testCount++] = {#name, test_##name}; } \
  } reg_##name; \
  static bool test_##name()

TEST(my_new_test) {
    // Test code here
    return true;  // pass
}
```

Add tests to the appropriate file based on what sanitizer should catch the class of bug:
- `test_lsan.cpp` — memory leaks, use-after-free, double-free
- `test_tsan.cpp` — data races, concurrent access patterns
- `test_ubsan.cpp` — integer overflow, null deref, division by zero, out-of-bounds

---

## Metrics Reference

`MediaPipeline.getMetrics()` returns a `PlayerMetrics` object:

### health

| Field | Type | Description |
|-------|------|-------------|
| `running` | boolean | Pipeline is active |
| `responsive` | boolean | Decode thread responding to heartbeats |
| `detached` | boolean | Decode thread was force-detached (unrecoverable) |
| `watchdogTripped` | boolean | Decode thread missed heartbeat deadline |
| `watchdogTripCount` | number | Total watchdog trips |
| `timeSinceHeartbeatMs` | number | Time since last decode thread heartbeat |

### quality

| Field | Type | Description |
|-------|------|-------------|
| `underruns` | number | Audio callback had no data (silence output) |
| `framesDropped` | number | Frames dropped due to overflow or late arrival |
| `framesDrained` | number | Frames discarded by fast-drain (burst backlog) |
| `decodeErrors` | number | Opus decode failures |
| `decoderResets` | number | Opus decoder resets (PTS jump or error accumulation) |
| `ptsDiscontinuities` | number | Timestamp jumps detected |
| `fastPathSwitches` | number | Transitions between direct-copy and resampled path |
| `catchupDeadZoneSnaps` | number | Catchup ratio snapped to unity (dead-zone) |
| `maxInterFrameGapMs` | number | Largest gap between consecutive feedData() calls |
| `gapsOver50ms` | number | Feed gaps > 50ms |
| `gapsOver100ms` | number | Feed gaps > 100ms |
| `gapsOver500ms` | number | Feed gaps > 500ms |

### video

| Field | Type | Description |
|-------|------|-------------|
| `avSyncOffsetUs` | number | Current A/V sync offset (audio = master) |
| `framesDecoded` | number | VP9 frames successfully decoded |
| `framesDropped` | number | Video frames dropped |
| `decodeErrors` | number | VP9 decode failures |
| `decoderResets` | number | VP9 decoder resets (error accumulation or session invalidation) |
| `currentFps` | number | Current render frame rate |
| `width` | number | Decoded video width |
| `height` | number | Decoded video height |
| `lateFrames` | number | Frames past render deadline |
| `skippedFrames` | number | Frames skipped (stale, replaced by fresher) |
| `surfaceLostCount` | number | Render surface detach events |
| `maxInterFrameGapMs` | number | Largest gap between consecutive video frames received |
| `gapsOver50ms` | number | Video feed gaps > 50ms |
| `gapsOver100ms` | number | Video feed gaps > 100ms |
| `gapsOver500ms` | number | Video feed gaps > 500ms |

### session

| Field | Type | Description |
|-------|------|-------------|
| `uptimeMs` | number | Time since start() |
| `samplesOutput` | number | Total audio samples written to hardware |

### latency

| Field | Type | Description |
|-------|------|-------------|
| `mode` | string | "low_latency" or "standard" |
| `isLowestLatency` | boolean | AAudio exclusive / RemoteIO low-latency granted |

### pools

| Field | Type | Description |
|-------|------|-------------|
| `decodedUnderPressure` | boolean | Decoded frame pool < 25% available |
| `encodedUnderPressure` | boolean | Encoded frame pool < 25% available |

### audioOutput

| Field | Type | Description |
|-------|------|-------------|
| `restartCount` | number | Audio output stream restarts |
| `lastError` | number | Last platform error code (0 = OK) |
| `actualSampleRate` | number | Hardware sample rate granted |
| `latencyUs` | number | Measured output latency |

### bluetooth

| Field | Type | Description |
|-------|------|-------------|
| `route` | string | Current route name |
| `isA2dp` | boolean | True if Bluetooth A2DP active |

### jitter

| Field | Type | Description |
|-------|------|-------------|
| `jitterUs` | number | Estimated network jitter |
| `bufferTargetUs` | number | Current adaptive buffer target |

### drift

| Field | Type | Description |
|-------|------|-------------|
| `driftPpm` | number | Clock drift in parts-per-million |
| `active` | boolean | Drift compensation resampler active |

---

## Triage Guide

### Audio Issues

| Symptom | Check | Likely Cause |
|---------|-------|-------------|
| Silence / dropouts | `quality.underruns` rising | Feed starvation or decode failure |
| Clicks / pops | `quality.framesDrained` > 0 | Fast-drain discarding frames (burst too large) |
| Robotic / pitch shift | `drift.driftPpm` > 200 | Excessive clock drift |
| Gradual latency increase | `jitter.bufferTargetUs` > 100000 | Jitter spikes inflating target |
| Stuttery audio | `quality.ptsDiscontinuities` > 0 | Timestamp jumps in source |

### Video Issues

| Symptom | Check | Likely Cause |
|---------|-------|-------------|
| Black screen | `video.framesDropped` = `video.framesDecoded` = 0 | Waiting for keyframe |
| Freezes | `video.lateFrames` rising | Network jitter too high |
| Green / corrupt | `video.decodeErrors` > 0 | VP9 decoder state corrupted (auto-recovers via keyframe) |
| Lip sync off | `video.avSyncOffsetUs` > 45000 | Clock drift dragging audio away from video |

### System Issues

| Symptom | Check | Likely Cause |
|---------|-------|-------------|
| `feedData()` returns false | — | Malformed WebM data |
| Rising memory | `pools.*UnderPressure` | Decode thread falling behind |
| Audio stops on BT connect | `audioOutput.restartCount` rising | Audio output restart failed |

---

## Directory Structure

```
webm-player/
├── src/index.tsx                    # TypeScript API
├── cpp/
│   ├── common/                      # Shared utilities
│   │   ├── MediaConfig.h            # All timing constants
│   │   ├── AVSyncCoordinator.h      # A/V clock sync
│   │   ├── RouteHandler.h           # Reactive audio routing
│   │   ├── HealthWatchdog.h         # Health state machine
│   │   ├── AudioResampler.h         # Speex resampler wrapper
│   │   └── ...
│   ├── playback/                    # Audio pipeline
│   │   ├── AudioDecodeChannel.h     # Core: encode→decode→read
│   │   ├── DecodeThread.h           # Decode loop
│   │   ├── FramePool.h             # Lock-free pool + SPSC queue
│   │   ├── JitterEstimator.h        # Adaptive jitter + drift
│   │   ├── DriftCompensator.h       # Clock drift + catchup
│   │   ├── AudioMixer.h            # Gain + SIMD mixing
│   │   └── OpusDecoderAdapter.h     # Opus wrapper
│   ├── video/                       # Video pipeline
│   │   ├── VideoFrameQueue.h        # VP9 frame queue
│   │   ├── VideoSyncController.h    # Video jitter + render clock
│   │   └── VideoSurfaceRegistry.h   # Surface management
│   └── demux/
│       ├── WebmDemuxer.h/cpp        # Incremental WebM parser
│       └── ...
├── android/src/main/
│   └── jni/
│       ├── playback/                # AAudio + audio session
│       └── video/                   # MediaCodec VP9
├── ios/
│   ├── playback/                    # RemoteIO + audio session
│   └── video/                       # VTDecompressionSession VP9
├── example/                         # Example React Native app
└── tests/sanitizer/                 # ASan, TSan, UBSan tests
```
