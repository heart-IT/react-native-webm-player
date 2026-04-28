# Technical Reference — @heartit/webm-player

Factual reference: timing constants, threading, metrics, queue contracts, test coverage. For design rationale see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); for symptom-driven triage see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

---

## Threading model

| Thread | Period | Blocking budget | Responsibility |
|--------|--------|-----------------|----------------|
| JS (`feedData`) | Bursty (Hypercore-driven) | Unbounded (non-RT) | Feed WebM, setup, metrics |
| Audio Decode | 500µs base, backoff → 16ms | Must keep decoded queue >0 | Opus decode, PLC gen |
| Video Decode | 2ms base, adaptive → 16ms | Must submit before render time | VP9 HW decode submit |
| Audio Callback | ~10–20ms (HW) | **Zero** — RT, no blocking | PCM read from lock-free queue |
| Video Render | VSync-driven | Must not stall UI | Surface present |

### SPSC queue contract

All audio queues are **Single-Producer Single-Consumer** with `acquire`/`release` ordering:

| Queue | Producer | Consumer | Clear |
|-------|----------|----------|-------|
| `encodedQueue_` (audio) | JS thread push | Decode thread pop | Deferred via `encodedClearRequested_` atomic |
| `decodedQueue_` (audio) | Decode thread push | Audio callback pop | Deferred via `decodedClearRequested_` atomic |

`clear()` = `while (pop()) {}` — always on consumer thread. Cross-thread clear uses an atomic flag polled by consumer.

### Lock-free audio callback

`readSamples()` uses atomic loads, SPSC pops, `memcpy`, and atomic counter increments only. **Forbidden on audio callback:** mutexes, allocations, logging, JNI, ObjC messages, `std::string`, `std::function` creation, `try/catch`.

---

## Timing constants

### Audio

| Constant | Value | Purpose |
|----------|-------|---------|
| Sample rate | 48,000 Hz | Native Opus rate |
| Channels | 1 (mono) | Broadcast voice |
| Frame duration | 20ms (960 samples) | Opus frame size |
| Jitter buffer default | 60ms (3 frames) | Adaptive 40–200ms |
| Max buffered | 400ms (20 frames) | Drop above this |
| PLC threshold | 60ms (3 frames) | Proactive PLC while 2 real frames remain |
| Catchup threshold | 40ms excess | Activate 5% speedup |
| Fast-drain threshold | 100ms excess | Discard with crossfade |
| Max speedup | 1.05× | Catchup cap |
| Max consecutive PLC | 8 frames (160ms) | Trigger decoder reset |
| PTS forward jump | 500ms | Trigger decoder reset |
| PTS backward jump | 200ms | Trigger decoder reset |
| Decode pool | 16 slots | Pre-allocated decoded pool |
| Decode queue | 8 slots | SPSC decoded queue |
| Encoded pool | 16 slots | Pre-allocated encoded pool |

### Video

| Constant | Value | Purpose |
|----------|-------|---------|
| Jitter buffer default | 66ms (2 frames @ 30fps) | Adaptive 33–200ms |
| Late frame threshold | 16.7ms | Half-frame — skip if late |
| Re-anchor interval | 30s | Render-clock re-anchor |
| Queue depth | 8 frames | Encoded queue cap |
| Max encoded frame | 512 KB | 4K keyframe worst case |

### Video decoder reset backoff

Exponential backoff prevents reset storms. Doubles on consecutive failures, resets on success. Keyframe requested after every reset.

| Parameter | Value | Source |
|-----------|-------|--------|
| Initial backoff | 200ms | `kInitialDecoderResetBackoffUs` |
| Max backoff | 2s | `kMaxDecoderResetBackoffUs` |
| Trigger | 5 consecutive failures | `kMaxConsecutiveErrors` |

### A/V sync

| Constant | Value | Purpose |
|----------|-------|---------|
| Dead zone | 15ms | No correction (ITU-R BT.1359) |
| Emergency threshold | 45ms | Full correction |
| Correction range | 15–45ms | Proportional band |
| EWMA α | 1/8 | Offset smoothing |

### Drift compensation

| Constant | Value | Purpose |
|----------|-------|---------|
| Min drift | 50 PPM | Activate above this |
| Max drift | 5,000 PPM | Cap at 0.5% |
| Measurement window | 30s | PPM calc window |
| Max combined ratio | 2.205 | user(2.0) × drift(1.05) × catchup(1.05) |

### Stall recovery

| Constant | Value | Purpose |
|----------|-------|---------|
| Detecting threshold | 250ms | Begin monitoring |
| Stall threshold | 500ms | Confirm, request keyframe |
| Failed threshold | 30s | Report failure |
| Keyframe re-request | 2s | During sustained stall |
| First keyframe request | Immediate | On Detecting→Stalled (500ms) |
| Recovery buffer target | 60ms | Min decoded before healthy |

### Backpressure

| Pipeline | Mechanism | Limit | On overflow |
|----------|-----------|-------|-------------|
| Audio | Buffer duration | 400ms | Drop, `framesDropped++` |
| Audio | Encoded pool | 16 slots | Drop |
| Video | Queue depth | 8 frames | Drop oldest |

### Clip buffer

| Constant | Value | Purpose |
|----------|-------|---------|
| Default size | 4 MB | ~30s at typical bitrate |
| Max cluster index | 512 | Ring of `ClusterRecord` |
| Seek chunk | 64 KB | Re-feed block size during DVR seek |

---

## Platform audio bridges

| | iOS (AVAudioOutputBridge) | Android (AAudioOutputBridge) |
|---|---|---|
| Unit / API | RemoteIO (`kAudioUnitSubType_RemoteIO`), output-only | AAudio, `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` |
| Sharing | — | Exclusive preferred, Shared fallback |
| Format | float32, mono, HW rate | float32, mono, 48kHz requested |
| Resampling | Speex when HW rate ≠ 48kHz | Speex when AAudio grants different rate |
| Category | `.playback` + `.allowBluetoothA2DP` | — |
| Restart backoff | 100ms base; sleeps 200→400→800→1600ms; 5th failure is fatal (3200ms cap unreachable) | Same |

**Resample buffer (both):** 4096 HW frames × 6× upsample ratio (8kHz → 48kHz) = 24,576 pre-allocated samples. Covers Bluetooth SCO at 8kHz worst case.

---

## Audio route restart matrix

| Transition | Restart? |
|------------|----------|
| Same route | No |
| Unknown → anything | No |
| Speaker ↔ Earpiece | No |
| A2DP ↔ Speaker/Earpiece | No |
| All other transitions | **Yes** |

---

## Health watchdog

Evaluation: 5s sliding window, events fire on **transitions only**, rate-limited to 1/500ms.

### States

| State | Trigger | Recovery |
|-------|---------|----------|
| **Buffering** | First `feedData()` or underrun after Healthy | Auto when buffer fills |
| **Healthy** | Audio playing + video rendering + sync <45ms | — |
| **Degraded** | Sustained underruns (≥5/window) or decode errors (≥3/window) | Feed quality recovers |
| **Stalled** | Feed gap >500ms | Data resumes |
| **Failed** | Thread detached; video decoder failed; excessive resets | Requires `stop()` + `start()` |

### Transitions

| Transition | Condition | `detail` |
|-----------|-----------|----------|
| → Buffering | `underruns` increments after Healthy | "rebuffering: audio underrun" |
| → Healthy | No underruns/errors/gaps/desync in window | "playback recovered" |
| → Degraded | `underruns` ≥5/window (×2 in speculative mode) | "degraded: sustained underruns" |
| → Degraded | `decodeErrors` ≥3/window | "degraded: sustained decode errors" |
| → Degraded | Audio receiving but not producing | "degraded: video decode stalled" |
| → Degraded | Video receiving but not decoding | "degraded: video decode stalled" |
| → Degraded | `avSyncOffsetUs` >45ms | "degraded: A/V sync drift > 45ms" |
| → Degraded | >10% video frame drop rate | "degraded: high video frame drop rate" |
| → Degraded | Both frame pools under pressure | "degraded: frame pool pressure" |
| → Stalled | `gapsOver500ms` increments | "feed stalled: gap > 500ms" |
| → Failed | Audio/video decode thread, ingest thread, or audio output detached | "failed: audio output, decode thread, video decode thread, or ingest thread detached" |
| → Failed | Video decoder factory returned null | (same as above) |
| → Failed | Audio `decoderResets` ≥3 in 5s | "failed: excessive audio decoder resets" |
| → Failed | Video `decoderResets` ≥3 in 5s | "failed: excessive video decoder resets" |

---

## Metrics reference

`MediaPipeline.getMetrics()` returns `PlayerMetrics`. See [`src/index.tsx`](src/index.tsx) for the authoritative TypeScript interface.

### `health`

| Field | Type | Description |
|-------|------|-------------|
| `running` | boolean | Pipeline active |
| `responsive` | boolean | Decode thread responding to heartbeats |
| `detached` | boolean | Decode thread force-detached (unrecoverable) |
| `watchdogTripped` | boolean | Decode thread missed heartbeat deadline |
| `watchdogTripCount` | number | Total watchdog trips |
| `timeSinceHeartbeatMs` | number | Time since last decode thread heartbeat |

### `quality` (audio)

| Field | Description |
|-------|-------------|
| `framesReceived` | Audio frames from demuxer |
| `underruns` | Audio callback had no data |
| `framesDropped` | Dropped due to overflow / late arrival |
| `framesDrained` | Discarded by fast-drain (burst backlog) |
| `decodeErrors` | Opus decode failures |
| `decoderResets` | Opus decoder resets (PTS jump / error accumulation) |
| `ptsDiscontinuities` | Timestamp jumps |
| `fastPathSwitches` | Transitions between direct-copy and resampled paths |
| `catchupDeadZoneSnaps` | Catchup ratio snapped to unity |
| `maxInterFrameGapMs` | Largest gap between `feedData()` calls |
| `gapsOver50ms` / `gapsOver100ms` / `gapsOver500ms` | Feed-gap histogram buckets |
| `plcFrames` | PLC frames generated |
| `fecFrames` | Recovered via Opus in-band FEC |
| `silenceSkipFrames` | Silence frames skipped for instant catchup |
| `peakConsecutivePLC` | Longest consecutive PLC run |
| `currentConsecutivePLC` | Current PLC run (0 when decoding) |
| `audioDecodeLatencyUs` | EWMA of Opus decode duration |
| `audioLastDecodeError` | Last non-zero Opus error code (0 = OK) |

### `video`

| Field | Description |
|-------|-------------|
| `avSyncOffsetUs` | Current A/V offset (audio = master) |
| `peakAbsAvSyncOffsetUs` | High-water absolute offset |
| `avSyncExceedCount` | Raw render offsets exceeding 45ms |
| `framesReceived` / `framesDecoded` / `framesDropped` | Frame counts |
| `keyFrameGatedDrops` | Frames dropped while waiting for keyframe (⊂ framesDropped) |
| `decodeErrors` | VP9 decode failures |
| `currentFps` | Current render FPS |
| `width` / `height` | Decoded dimensions |
| `jitterUs` / `bufferTargetUs` | Video jitter estimate / buffer target |
| `lateFrames` | Past render deadline |
| `skippedFrames` | Stale, replaced by fresher |
| `decoderResets` | VP9 decoder resets |
| `surfaceLostCount` | Render surface detach events |
| `maxInterFrameGapMs`, `gapsOver{50,100,500}ms` | Video feed-gap histogram |
| `driftPpm` | Video clock drift |
| `decodeLatencyUs` / `lastDecodeTimeUs` | Last decode duration / timestamp |
| `lastDecodeError` | Last VP9 error code |
| `queueDepth` | Current encoded queue depth |
| `needsKeyFrame` | Waiting for keyframe |
| `keyFrameRequests` | Total keyframes requested |
| `decoderState` | `VideoDecoderState` enum |
| `decodeThreadResponsive` / `timeSinceVideoHeartbeatMs` | Video decode thread heartbeat |

### `pipeline`

| Field | Description |
|-------|-------------|
| `audioStreamState` | `StreamState` (0=Inactive, 1=Buffering, 2=Playing, 3=Underrun, 4=Paused) |
| `currentGain` | Applied audio gain (0.0–2.0) |
| `muted` | Audio output muted |
| `bufferedDurationUs` | Total buffered audio (encoded + decoded) |
| `decodedDurationUs` | Decoded audio ready for playback |

### `playbackState` (root)

`playbackState` — `PlaybackState` enum (0=Idle, 1=Buffering, 2=Playing, 3=Paused, 4=Stalled, 5=Failed).

### `session`

| Field | Description |
|-------|-------------|
| `uptimeMs` | Time since `start()` |
| `samplesOutput` | Total audio samples written to HW |
| `playbackRate` | Current rate (0.5–2.0) |
| `streamStatus` | `StreamStatus` (0=Live, 1=Buffering, 2=Ended, 3=NoPeers) |

### `latency`

| Field | Description |
|-------|-------------|
| `mode` | `"low_latency"` or `"standard"` |
| `isLowestLatency` | AAudio exclusive / RemoteIO low-latency granted |
| `endToEndUs` | Estimated e2e latency (buffered + output) |

### `pools`

| Field | Description |
|-------|-------------|
| `decodedUnderPressure` | Decoded pool <25% available |
| `encodedUnderPressure` | Encoded pool <25% available |

### `audioOutput`

| Field | Description |
|-------|-------------|
| `restartCount` | Audio output stream restarts |
| `lastError` | Last platform error code (0=OK) |
| `actualSampleRate` | HW sample rate granted |
| `latencyUs` | Measured output latency |
| `callbackJitterUs` | Callback timing jitter |
| `running` | Stream running |
| `interrupted` | Audio session interrupted (iOS) |
| `sampleRateValid` | Granted rate is usable |

### `bluetooth`

| Field | Description |
|-------|-------------|
| `route` | Current route name |
| `isA2dp` | Bluetooth A2DP active |
| `routeChangeCount` | Monotonic count of confirmed route changes since construction |

### `jitter`

| Field | Description |
|-------|-------------|
| `jitterUs` | Network jitter estimate |
| `jitterTrendUs` | First derivative; positive = degrading |
| `bufferTargetUs` | Adaptive buffer target |
| `arrivalConfidence` | 0.0 unknown → 1.0 stable |
| `speculativeMode` | Low-confidence estimator mode |

### `drift`

| Field | Description |
|-------|-------------|
| `driftPpm` | Clock drift in PPM |
| `active` | Drift compensator resampler active |
| `currentRatio` | Smoothed drift ratio (1.0 = no drift) |
| `catchupRatio` | Catchup speedup (1.0 = none) |

### `levels`

| Field | Description |
|-------|-------------|
| `peakLevel` / `rmsLevel` | Linear 0.0–1.0+ (>1.0 = clipping) |
| `peakDbfs` / `rmsDbfs` | dBFS (−100 = silence, 0 = full scale) |
| `clipCount` | Frames exceeding 1.0 |

### `stall`

| Field | Description |
|-------|-------------|
| `state` | `healthy`\|`detecting`\|`stalled`\|`recovering`\|`failed` |
| `stallCount` / `recoveryCount` | Total stalls / successful recoveries |
| `keyFrameRequests` | Keyframes requested during stalls |
| `totalStallMs` / `lastStallMs` / `longestStallMs` | Stall durations |
| `lastRecoveryMs` / `longestRecoveryMs` | Recovery durations |

### `clip`

| Field | Description |
|-------|-------------|
| `enabled` | Clip ring buffer active (`setClipBufferDuration > 0`) |
| `bufferCapacity` / `bytesUsed` | Ring capacity / bytes stored |
| `clusterCount` | WebM clusters retained |
| `availableSeconds` | Rewind range |

### `demux`

| Field | Description |
|-------|-------------|
| `totalBytesFed` / `feedDataCalls` | Total bytes / call count |
| `audioPacketsEmitted` / `videoPacketsEmitted` | Extracted packets |
| `overflowCount` / `blockStallCount` / `parseErrorCount` | Error counters |
| `cumulativeParseErrorCount` | Parse errors accumulated across session resets |
| `sessionResetCount` | Demuxer session resets |
| `timeInErrorMs` | Time spent in the permanent `Error` parse state |
| `oversizedFrameDrops` | Video frames rejected for exceeding `kMaxEncodedFrameSize` |
| `packetCapDrops` | Packets dropped for exceeding per-feed cap |
| `appendBackpressureDrops` | Blocks dropped due to scratch-buffer backpressure |
| `feedJitterUs` | Network-layer arrival jitter (EWMA) |
| `feedDataLatencyUs` | EWMA of `feedData()` parse duration |
| `bufferBytes` | Current demuxer buffer size |
| `parseState` | 0=WaitingForEBML, 1=WaitingForSegment, 2=ParsingTracks, 3=Streaming |
| `partialDropCount` | Partial blocks dropped |
| `ingestBytesDropped` | Bytes dropped to ingest ring backpressure |
| `ingestRingUsed` / `ingestRingCapacity` | Ring buffer fill |
| `ingestRingWriteRejects` | Ingest ring write rejections on full buffer |
| `ingestThreadResponsive` / `timeSinceIngestHeartbeatMs` | Ingest thread heartbeat |
| `ingestThreadDetached` | Ingest thread force-detached (unrecoverable) |

---

## Testing

### Suites

| Suite | Command | Tests | Catches |
|-------|---------|-------|---------|
| ASan + LSan | `yarn test:asan` | 30 | Leaks, UAF, double-free, OOB |
| TSan | `yarn test:tsan` | 36 | Data races, concurrent access |
| UBSan | `yarn test:ubsan` | 86 | Int overflow, null deref, div0, OOB |
| Route handler | ASan build | 17 | Audio routing logic (pure C++) |
| E2E demux | ASan build | 22 | WebmDemuxer with real streams |
| Temporal stress | UBSan build | 29 | Pipeline under temporal stress |
| Health watchdog | Standalone | 55 | Health state machine |
| Audit proof | ASan build | 40 | Confirm/refute P0/P1 audit findings |
| Transcript | ASan build | 13 | whisper.cpp pipeline |
| Ingest thread | ASan build | 16 | Ingest ring + thread lifecycle |
| RT safety | ASan build | 3 | Real-time audio callback safety |
| TS build | `yarn prepare` | — | Type errors |
| Type check | `yarn typecheck` | — | Type errors without build |
| Lint | `yarn lint` | — | Style |

**Total: 347 native tests across 11 binaries.**

### Test file guide

| File | Sanitizer | Add here |
|------|-----------|----------|
| `test_lsan.cpp` | ASan + LSan | Leaks, UAF, double-free |
| `test_tsan.cpp` | TSan | Data races, concurrent patterns |
| `test_ubsan.cpp` | UBSan | Int overflow, null deref, div0, OOB |
| `test_route_handler.cpp` | ASan | Audio routing (pure C++) |
| `test_e2e_demux.cpp` | ASan | WebmDemuxer with real streams |
| `test_temporal_stress.cpp` | UBSan | Behavioral pipeline under temporal stress |
| `test_health_watchdog.cpp` | Standalone | Health state machine |
| `test_audit_proof.cpp` | ASan | P0/P1 audit-finding verification |
| `test_transcript.cpp` | ASan | whisper.cpp pipeline |
| `test_ingest_thread.cpp` | ASan | Ingest ring + thread lifecycle |
| `test_rt_safety.cpp` | ASan | RT audio callback safety |

### Running locally

```sh
brew install opus speexdsp   # Prerequisite

# ASan
cd tests/sanitizer
cmake -B build -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/test_lsan
./build/test_ubsan
./build/test_route_handler

# TSan
cmake -B build_tsan -DSANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tsan -j --target test_tsan
./build_tsan/test_tsan
```
