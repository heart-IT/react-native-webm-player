# Technical Reference — @holepunchto/webm-player

Factual reference for timing constants, threading model, metrics fields, queue contracts, and test coverage.

For architecture and design rationale, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For troubleshooting, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

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
| PTS backward jump | 200ms | Triggers decoder reset |
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
| Dead zone | 15ms | No correction below this (ITU-R BT.1359) |
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

### Stall Recovery

| Constant | Value | Purpose |
|----------|-------|---------|
| Detecting threshold | 250ms | Begin monitoring |
| Stall threshold | 500ms | Confirmed stall, request keyframe |
| Failed threshold | 30s | Report failure |
| Keyframe re-request interval | 2s | During sustained stalls |
| First keyframe request delay | 200ms | Avoid spam during brief hiccups |
| Recovery buffer target | 60ms | Minimum decoded buffer before declaring healthy |

### Backpressure Limits

| Pipeline | Mechanism | Limit | On overflow |
|----------|-----------|-------|-------------|
| Audio | Buffer duration ceiling | 400ms | Drop frame, increment `framesDropped` |
| Audio | Encoded pool exhaustion | 16 slots | Drop frame |
| Video | Queue depth cap | 8 frames | Drop oldest frame |

### Clip Buffer

| Constant | Value | Purpose |
|----------|-------|---------|
| Default buffer size | 4 MB | ~30s at typical bitrate |
| Max cluster index | 512 entries | Ring of ClusterRecord entries |
| Seek chunk size | 64 KB | Re-feed block size during DVR seek |

---

## Platform Audio Bridges

### iOS: AVAudioOutputBridge

| Property | Value |
|----------|-------|
| AudioUnit | RemoteIO (`kAudioUnitSubType_RemoteIO`), output-only |
| Format | float32, mono, hardware sample rate |
| Resampling | Speex when hardware rate != 48kHz |
| Category | `.playback` with `.allowBluetoothA2DP` |
| Restart backoff | 100ms base, 3200ms max, 5 retries |

### Android: AAudioOutputBridge

| Property | Value |
|----------|-------|
| API | AAudio, `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` |
| Sharing | Exclusive preferred, Shared fallback |
| Format | float32, mono, 48kHz requested |
| Resampling | Speex when AAudio grants different rate |
| Restart backoff | 100ms base, 3200ms max, 5 retries |

### Resample Buffer

Both platforms: 4096 hardware frames x 6x upsample ratio (8kHz → 48kHz) = 24,576 samples pre-allocated.

---

## Audio Route Restart Matrix

| Transition | Restart? |
|------------|----------|
| Same route | No |
| Unknown → anything | No |
| Speaker ↔ Earpiece | No |
| A2DP ↔ Speaker/Earpiece | No |
| All other transitions | **Yes** |

---

## Health Watchdog States

| State | Trigger | Recovery |
|-------|---------|----------|
| **Buffering** | First `feedData()` or underrun after Healthy | Automatic when buffer fills |
| **Healthy** | Audio playing + video rendering + sync < 45ms | — |
| **Degraded** | Sustained underruns (5+/window) or decode errors (3+/window) | Feed quality improvement |
| **Stalled** | Feed gap > 500ms | Resume feeding |
| **Failed** | Decode thread detached or audio output stopped | Requires `stop()` + `start()` |

### Transition Conditions

| Transition | Condition | Detail string |
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

Evaluation: 5-second sliding window, events fire on state transitions only, rate-limited to one event per 500ms.

---

## Metrics Reference

`MediaPipeline.getMetrics()` returns a `PlayerMetrics` object. See [`src/index.tsx`](src/index.tsx) for the full TypeScript interface.

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
| `framesReceived` | number | Audio frames received from demuxer |
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
| `plcFrames` | number | PLC (packet loss concealment) frames generated |
| `peakConsecutivePLC` | number | Longest consecutive PLC run |

### video

| Field | Type | Description |
|-------|------|-------------|
| `avSyncOffsetUs` | number | Current A/V sync offset (audio = master) |
| `framesReceived` | number | VP9 frames received from demuxer |
| `framesDecoded` | number | VP9 frames successfully decoded |
| `framesDropped` | number | Video frames dropped |
| `decodeErrors` | number | VP9 decode failures |
| `currentFps` | number | Current render frame rate |
| `width` | number | Decoded video width |
| `height` | number | Decoded video height |
| `jitterUs` | number | Video jitter estimate |
| `bufferTargetUs` | number | Video jitter buffer target |
| `lateFrames` | number | Frames past render deadline |
| `skippedFrames` | number | Frames skipped (stale, replaced by fresher) |
| `decoderResets` | number | VP9 decoder resets |
| `surfaceLostCount` | number | Render surface detach events |
| `maxInterFrameGapMs` | number | Largest gap between consecutive video frames received |
| `gapsOver50ms` | number | Video feed gaps > 50ms |
| `gapsOver100ms` | number | Video feed gaps > 100ms |
| `gapsOver500ms` | number | Video feed gaps > 500ms |
| `driftPpm` | number | Video clock drift in PPM |
| `decodeLatencyUs` | number | Last decode duration |
| `lastDecodeTimeUs` | number | Timestamp of last decode |
| `lastDecodeError` | number | Last VP9 decode error code |
| `queueDepth` | number | Current encoded video queue depth |
| `needsKeyFrame` | boolean | Waiting for keyframe |
| `keyFrameRequests` | number | Total keyframes requested |
| `decoderState` | number | VideoDecoderState enum value |

### pipeline

| Field | Type | Description |
|-------|------|-------------|
| `audioStreamState` | number | StreamState enum (0=Inactive, 1=Buffering, 2=Playing, 3=Underrun, 4=Paused) |
| `currentGain` | number | Applied audio gain (0.0-2.0) |
| `bufferedDurationUs` | number | Total buffered audio (encoded + decoded) |
| `decodedDurationUs` | number | Decoded audio ready for playback |

### session

| Field | Type | Description |
|-------|------|-------------|
| `uptimeMs` | number | Time since start() |
| `samplesOutput` | number | Total audio samples written to hardware |
| `playbackRate` | number | Current playback rate (0.5-2.0) |

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
| `callbackJitterUs` | number | Audio callback timing jitter |
| `running` | boolean | Audio output stream is running |
| `interrupted` | boolean | Audio session interrupted (iOS) |
| `sampleRateValid` | boolean | Granted sample rate is usable |

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
| `currentRatio` | number | Smoothed drift ratio (1.0 = no drift) |
| `catchupRatio` | number | Catchup speedup ratio (1.0 = no catchup) |

### levels

| Field | Type | Description |
|-------|------|-------------|
| `peakLevel` | number | Linear peak level (0.0-1.0+, >1.0 = clipping) |
| `rmsLevel` | number | Linear RMS level |
| `peakDbfs` | number | Peak in dBFS (-100 = silence, 0 = full scale) |
| `rmsDbfs` | number | RMS in dBFS |
| `clipCount` | number | Audio frames that exceeded 1.0 |

### stall

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | Current stall state: healthy, detecting, stalled, recovering, failed |
| `stallCount` | number | Total feed stalls detected |
| `recoveryCount` | number | Successful recoveries from stall |
| `keyFrameRequests` | number | Keyframes requested during stalls |
| `totalStallMs` | number | Cumulative stall duration |
| `lastStallMs` | number | Duration of most recent stall |
| `longestStallMs` | number | Longest single stall |

### demux

| Field | Type | Description |
|-------|------|-------------|
| `totalBytesFed` | number | Total bytes passed to demuxer |
| `feedDataCalls` | number | Number of feedData() calls |
| `audioPacketsEmitted` | number | Opus packets extracted |
| `videoPacketsEmitted` | number | VP9 packets extracted |
| `overflowCount` | number | Demuxer buffer overflow events |
| `blockStallCount` | number | Blocks stalled waiting for data |
| `bufferBytes` | number | Current demuxer buffer size |
| `parseState` | number | Demuxer state (0=WaitingForEBML, 1=WaitingForSegment, 2=ParsingTracks, 3=Streaming) |
| `partialDropCount` | number | Partial blocks dropped |

---

## Testing

### Automated (CI)

| Suite | Command | Tests | What it catches |
|-------|---------|-------|-----------------|
| AddressSanitizer + LeakSanitizer | `yarn test:asan` | 31 | Memory leaks, use-after-free, double-free, buffer overflow |
| ThreadSanitizer | `yarn test:tsan` | 34 | Data races, concurrent access violations |
| UndefinedBehaviorSanitizer | `yarn test:ubsan` | 87 | Integer overflow, null deref, division by zero, out-of-bounds |
| Route handler | Runs with ASan build | 18 | Audio routing logic (pure C++) |
| End-to-end demux | Runs with ASan build | 22 | WebM demuxer with real stream data |
| Temporal stress | Runs with UBSan build | 19 | Pipeline behavior under temporal stress |
| Health watchdog | Standalone | 52 | Health state machine transitions |
| TypeScript build | `yarn prepare` | — | Type errors |
| Type check | `yarn typecheck` | — | Type errors without build |
| Lint | `yarn lint` | — | Style violations |

Total: **263 native tests** across 7 test binaries.

### Test Coverage

**LeakSanitizer (test_lsan.cpp):** FramePool acquire/release/RAII, FrameQueue SPSC push/pop/clear, JitterEstimator lifecycle, DriftCompensator/Speex resampler, CatchupController, AudioMixer, DecodeThread start/stop, OpusDecoderAdapter init/decode/PLC/move, pool stress cycles, ClipBuffer enable/disable/extract lifecycle, StallRecoveryController reset cycles.

**ThreadSanitizer (test_tsan.cpp):** Concurrent FramePool acquire/release (4 threads), SPSC producer/consumer (1000 frames), JitterEstimator concurrent update/read with spike detection, DriftCompensator concurrent update+read, CatchupController bidirectional, AudioDecodeChannel concurrent push+read+decode, AudioMixer concurrent access, DecodeThread concurrent push, StreamMetrics concurrent counters, VideoFrameQueue concurrent push/pop and reset-during-push, VideoJitterEstimator and VideoSurfaceRegistry concurrent access, ClipBuffer concurrent append+extractClip+extractFromPts, StallRecoveryController concurrent evaluate+onDataReceived+stall-and-recover, AudioLevelMeter concurrent update+read.

**UndefinedBehaviorSanitizer (test_ubsan.cpp):** Pool boundary conditions (exhaustion, double-release, access-after-release), JitterEstimator arithmetic (zero interval, large deviation, negative timestamps, overflow guards), DriftCompensator division-by-zero/extreme-drift/negative-drift, Speex resampler extreme ratios (0.5-2.0), CatchupController no-slowdown/speedup-to-unity, AudioMixer null/zero/gain-ramp, AudioDecodeChannel inactive-read/push/zero-size/buffer-target-extremes, OpusDecoderAdapter null/uninitialized/DTX/garbage/zero-size, AudioResampler passthrough/max-output/float, VideoFrameQueue keyframe/oversize/null/overflow, VideoJitterEstimator/RenderClock edge cases, AudioLevelMeter silence/sine/clipping/dBFS/non-aligned/small-buffer, ClipBuffer lifecycle/wrap-around/extractFromPts/availableRange, StallRecoveryController initial-state/reset.

**RouteHandler (test_route_handler.cpp):** Initial route detection, dedup same-route, route change with callback+restart, builtin transitions (no restart), A2DP transitions (no restart), SCO/Wired/USB transitions (restart), unknown route handling, drift reset behavior, reset/reinitialize, comprehensive `needsRestart()` matrix, multiple sequential route changes.

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

### Test File Guide

| File | Sanitizer | What to add here |
|------|-----------|-----------------|
| `test_lsan.cpp` | ASan + LSan | Memory leaks, use-after-free, double-free |
| `test_tsan.cpp` | TSan | Data races, concurrent access patterns |
| `test_ubsan.cpp` | UBSan | Integer overflow, null deref, division by zero, out-of-bounds |
| `test_route_handler.cpp` | ASan | Audio routing logic (pure C++) |
| `test_temporal_stress.cpp` | UBSan | Behavioral pipeline verification under temporal stress |

---

## Directory Structure

```
webm-player/
├── src/index.tsx                    # TypeScript API
├── cpp/
│   ├── common/                      # Shared utilities
│   │   ├── MediaConfig.h            # All timing constants
│   │   ├── AVSyncCoordinator.h      # A/V clock sync
│   │   ├── PipelineOrchestrator.h   # Shared JSI dispatch (iOS + Android)
│   │   ├── ClipBuffer.h             # Rolling WebM ring buffer for clip capture + DVR
│   │   ├── StallRecoveryController.h # Automatic feed stall detection + recovery
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
│   │   ├── AudioMixer.h            # Gain + SIMD mixing + AudioLevelMeter
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
├── docs/
│   ├── ARCHITECTURE.md              # Design rationale and data flow
│   ├── GETTING_STARTED.md           # Tutorial
│   ├── INTEGRATION_GUIDE.md         # Platform setup how-to
│   └── TROUBLESHOOTING.md           # Symptom-driven diagnosis
├── example/                         # Example React Native app
└── tests/sanitizer/                 # ASan, TSan, UBSan tests
```
