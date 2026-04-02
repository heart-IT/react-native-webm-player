# Architecture — @holepunchto/webm-player

How the broadcast player works internally: design decisions, data flow, and the reasoning behind each subsystem.

For timing constants, metrics fields, and API signatures, see [TECHNICAL.md](../TECHNICAL.md). For troubleshooting, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## System Overview

The player receives a muxed WebM stream (VP9 video + Opus audio) from JavaScript, demuxes it in shared C++, and routes packets to platform-specific decode and render pipelines. Audio is the master clock for A/V synchronization.

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

### Why This Layering

The architecture separates concerns across three layers — JavaScript, shared C++, and platform native — to enforce a critical constraint: **JavaScript has no timing authority**. All pacing, buffering, and synchronization happens in native code where real-time guarantees are possible.

JavaScript's only job is to call `feedData()` with muxed WebM bytes. This keeps the JS thread free for UI work and prevents GC pauses from affecting playback timing.

The shared C++ layer (demuxer, audio DSP, video sync, jitter estimation) is identical on both platforms. Platform-specific code is limited to audio output (AAudio vs RemoteIO) and video decode (MediaCodec vs VTDecompressionSession). This ensures iOS and Android have identical playback behavior where it matters most — timing and synchronization.

### Layer Responsibilities

| Layer | Does | Does NOT |
|-------|------|----------|
| **Bare Thread** | Hypercore read, delivers muxed WebM bytes | Demuxing, audio/video processing |
| **JavaScript** | Receives muxed WebM via callback, calls `feedData()` | Buffering, DSP, timing, pacing, demuxing |
| **Native (Demuxer)** | Parses WebM container, extracts Opus + VP9 packets | Decoding, playback |
| **Native Audio** | Opus decode, jitter buffer, drift comp, mixer, routing | Capture, encode, network I/O |
| **Native Video** | VP9 HW decode, render sync, surface management | Encode, network I/O |

---

## Data Flow

### feedData() Path

When JavaScript calls `feedData(webmBytes)`, the data flows through a deterministic pipeline:

```
JS: feedData(webmBytes)
  → JSI: extractBuffer() (zero-copy or single-copy)
  → ClipBuffer::appendBytes() (tee raw bytes for clip capture / DVR)
  → StallRecoveryController::onDataReceived() (reset stall detection)
  → WebmDemuxer::feedData()
    → StreamReader::append() (append to internal buffer)
    → Incremental parse (fallthrough state machine)
    → Return DemuxResult { AudioPacket[], VideoPacket[], error, newClusterCount }
  → ClipBuffer: setStreamHeader(), onNewCluster(), onKeyFrame(), onBlockPts()
  → Route AudioPackets → MediaSession::pushAudioFrame() → AudioDecodeChannel
  → Route VideoPackets → VideoFrameQueue::pushEncodedFrame()
  → Return bool to JS (false if parse error)
```

The ClipBuffer tee happens before demuxing — raw bytes are captured for clip extraction and DVR rewind, independent of the decode pipeline. This means clip capture works even if the decoder is in an error state.

### Audio Pipeline

Audio follows a strict producer-consumer chain with lock-free handoffs between threads:

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
  → AudioLevelMeter: peak/RMS computation (NEON/SSE SIMD, atomic store)
  → Platform audio output (AAudio / RemoteIO)
```

The decode thread acquires a pool token *before* popping from the encoded queue. This prevents a subtle frame-loss race: if the pool is exhausted when the decode thread pops, the encoded frame would have nowhere to go.

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

Video uses a mutex-protected deque rather than a lock-free queue because the video decode thread runs at lower frequency (~30Hz vs audio's ~50Hz decode rate) and hardware decode submission is already the bottleneck — the mutex overhead is negligible compared to GPU dispatch latency.

---

## A/V Synchronization

Audio is the master clock. This is a deliberate choice: audio hardware runs on a crystal oscillator with extremely stable timing, while video frame scheduling is inherently jittery (GPU load, compositor scheduling, display vsync). Using audio as the reference clock means the most perceptible artifact (audio pitch/timing changes) never occurs.

`AVSyncCoordinator` tracks the offset between audio and video render timestamps using ITU-R BT.1359 perceptibility thresholds:

| Offset | Action | Rationale |
|--------|--------|-----------|
| < 15ms | No correction | Below human perception threshold |
| 15-45ms | Gradual proportional correction | Noticeable but not jarring — smooth adjustment |
| > 45ms | Aggressive full correction | Clearly perceptible lip-sync error |

Video adjusts its jitter buffer target based on the sync offset. Audio never adjusts for video — this asymmetry is intentional. Adjusting audio timing causes pitch artifacts that are far more noticeable than a slightly early or late video frame.

---

## Demuxer State Machine

The WebM demuxer uses an incremental state machine that can process partial data. This is essential because `feedData()` receives arbitrarily-sized chunks from the network — a single WebM cluster may arrive across multiple calls.

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

Each state attempts to parse and falls through on success. If more data is needed, it returns an empty result (no error) — the caller simply feeds more data on the next call. This design avoids blocking and means the demuxer never needs to buffer internally beyond the current parse window.

Buffer compaction discards consumed bytes before the current cluster to prevent unbounded memory growth. Packets in `DemuxResult` point into the demuxer's internal buffer and are valid only until the next `feedData()` or `reset()` call — both pipelines copy the data immediately.

---

## Jitter Buffer

The adaptive jitter estimator uses EWMA smoothing with spike detection and burst memory to balance latency against robustness.

### Why Adaptive

A fixed jitter buffer would either be too small (causing underruns during network spikes) or too large (adding unnecessary latency during stable conditions). The adaptive approach starts conservative and converges to the minimum safe buffer for current conditions.

### Algorithm

1. **Arrival jitter:** Track inter-packet arrival time vs. expected interval (20ms for Opus)
2. **EWMA smoothing:** `jitter = alpha * sample + (1-alpha) * jitter` (alpha = 0.125). This smooths out individual packet timing variations while tracking trends.
3. **Spike detection:** If deviation exceeds 3x the current estimate, bypass EWMA and jump immediately. Without this, a sudden network degradation would take many packets to converge, causing underruns during the transition.
4. **Spike hold:** Maintain the elevated target for 2 seconds after a spike. Network jitter tends to cluster — dropping the target immediately after one spike often leads to another underrun.
5. **Burst memory:** Track recurring spike patterns via a ring buffer of 4 recent spikes. If 2+ spikes occur within a 30-second window, maintain a floor at 75% of the minimum recent spike jitter. This prevents the buffer from repeatedly shrinking and expanding on periodic jitter sources (e.g., competing traffic patterns).
6. **Buffer target:** `max(jitter * 2.5, minBuffer, burstFloor)` clamped to `[40ms, 200ms]`

### Drift Tracking

The jitter estimator also tracks long-term clock drift between sender and receiver. Over 30-second windows, it measures cumulative sample-count vs. wall-clock delta and reports drift as PPM to `DriftCompensator`. This piggybacks on jitter estimation because the same arrival timestamps are needed — no additional measurement overhead.

---

## Stream Recovery

### Keyframe Gating

`VideoFrameQueue` starts with `needsKeyFrame_ = true`. All non-keyframe VP9 packets are dropped until a keyframe arrives. This is the correct behavior because VP9 inter-frames reference previous frames — decoding without the reference produces green/corrupt output that is worse than showing nothing.

- **Stream start:** Video stays black until first keyframe (~500ms at typical broadcast intervals)
- **After decode error:** `requestKeyFrame()` clears the queue and re-enables gating, forcing a clean restart from the next keyframe
- **Upstream signal:** `setKeyFrameNeededCallback()` notifies JS when native needs a keyframe, allowing the integrator to request one from the source — reducing recovery time below the natural keyframe interval

### PTS Discontinuity Handling

When `AudioDecodeChannel` detects a PTS jump > 500ms forward or > 200ms backward, it treats this as a stream discontinuity (source restart, seek, or encoder reset). The response:

1. Sets `needsDecoderReset_` flag
2. Decode thread resets the Opus decoder on next cycle — necessary because Opus maintains internal state that would produce artifacts if fed discontinuous data
3. Crossfade prevents clicks at the splice point
4. `ptsDiscontinuities` metric is incremented

The asymmetric thresholds (500ms forward vs 200ms backward) reflect that forward jumps are common during network gaps, while backward jumps indicate a more unusual condition (stream restart or source-side seek).

### Automatic Stall Recovery

`StallRecoveryController` handles feed stalls without JS intervention:

```
Healthy → Detecting (gap > 250ms) → Stalled (gap > 500ms) → Recovering (data resumes) → Healthy (buffer sufficient)
                                        ↓
                                  Failed (gap > 30s)
```

The two-phase detection (250ms → 500ms) avoids false positives from normal network jitter while still responding quickly to real stalls. The 250ms "detecting" phase is a monitoring-only window — no action is taken, but the system is primed to respond if the gap persists.

During sustained stalls, keyframe re-requests fire every 2 seconds. The first request fires 200ms into the stall to avoid spamming during brief hiccups.

### Feed Starvation

If JS stops calling `feedData()`:
- **Audio:** Transitions to Underrun state, outputs silence, PLC conceals gaps up to 160ms (8 frames)
- **Video:** Holds last rendered frame — freezing is less jarring than showing a black screen
- **Recovery:** When data resumes, decoder resets if gap > 500ms; crossfade prevents click

---

## Clip Capture

`ClipBuffer` maintains a rolling ring buffer of raw WebM bytes, teed from `feedData()`. The key design decision is **zero re-encoding** — clips are extracted by prepending the cached EBML+Segment+Tracks header to selected cluster data, producing a valid standalone WebM file instantly.

### Why Zero Re-Encode

Re-encoding a clip would require a VP9 encoder (heavy, ~100ms per frame) and would degrade quality. By capturing raw muxed bytes, clip extraction is O(memcpy) regardless of clip duration — a 30-second clip takes the same time as a 1-second clip.

### Ring Buffer Design

```
feedData() → ClipBuffer::appendBytes() (circular write)
           → onNewCluster() / onKeyFrame() (cluster index)
           → setStreamHeader() (cached once at Streaming state)

captureClip(seconds) → walk keyframe index backward → copy header + clusters → file I/O on background thread → Promise<string>
```

- **Circular byte buffer:** Configurable size (default 4MB, ~30s at typical bitrate)
- **Cluster index:** Ring of up to 512 `ClusterRecord` entries tracking ring offset, size, PTS, keyframe flag
- **Keyframe-aligned extraction:** Clips always start at a keyframe cluster — ensures the output file is playable from the first frame
- **Wrap-around safety:** Each cluster record stores a monotonic byte counter; extraction detects overwritten data
- **Thread safety:** Mutex-protected (JS thread writes, background thread reads for extraction; neither is RT, so mutex is acceptable)

Output is a valid standalone WebM file playable by any VP9-capable player. It can be published as a new Hypercore for P2P distribution.

---

## DVR Timeshift

`seekTo()` reuses the ClipBuffer infrastructure to seek backward into buffered data without network requests.

### How It Works

1. Compute target PTS from offset: `targetPts = currentTimeUs + offsetSeconds * 1e6`
2. Set `timeshifting_ = true` — incoming `feedData()` calls accumulate in `livePendingBuffer_` instead of demuxing
3. Extract bytes from ClipBuffer via `extractFromPts(targetPts)` — header + clusters from nearest keyframe
4. Reset demuxer + audio/video pipelines (without clearing ClipBuffer)
5. Re-feed extracted bytes through the demuxer in 64KB chunks, dispatching packets normally
6. Drain `livePendingBuffer_` through the demuxer to catch up to live
7. Clear `timeshifting_` — resume normal flow

The `livePendingBuffer_` accumulation during seek is important: it ensures no data is lost while the pipelines are being rebuilt. Without it, any `feedData()` calls during the seek window would be silently dropped.

---

## Audio Level Metering

`AudioLevelMeter` computes peak and RMS levels on every audio callback with zero additional allocations. It runs inside the audio callback path, so it uses:

- **SIMD:** NEON (ARM) and SSE (x86) paths for vectorized peak/sum-of-squares — processes 4 samples per instruction
- **Lock-free:** Results stored in atomics, readable from any thread via `getMetrics()`
- **Clip detection:** Counts frames exceeding 1.0 (digital clipping)

This design allows JS to drive a VU meter by polling `getMetrics()` without any synchronization overhead or impact on the audio callback's real-time budget.

---

## Audio Routing

The routing model is reactive — zero state machines, zero timers. `RouteHandler` (shared C++) receives OS route change events and makes a single decision: dedup, restart, drift reset, JS callback.

### Why Reactive

A state-machine approach would need to track "connecting", "connected", "disconnecting" states for each device type, with timers for debouncing. The reactive model is simpler: when the OS reports a route, compare it to the previous route and decide. The OS already handles the state machine internally — duplicating it adds complexity without benefit.

### Platform Integration

**iOS:**
```
AVAudioSessionRouteChangeNotification → serial GCD queue →
  detectCurrentRoute() → RouteHandler::onRouteDetected() →
  [dedup | restart | drift reset | JS callback]
```
Category: `.playback` with `.allowBluetoothA2DP` option.

**Android:**
```
Kotlin AudioSessionBridge → JNI nativeOnAudioRouteChanged →
  RouteHandler::onRouteDetected() →
  [dedup | restart | drift reset | JS callback]
```
Uses `setCommunicationDevice()` on API 31+, legacy `setSpeakerphoneOn/startBluetoothSco` below.

### Restart Decision

Not all route changes require restarting the audio output stream. The decision is based on whether the hardware path actually changes:

| Transition | Restart? | Reason |
|------------|----------|--------|
| Same route | No | No change |
| Unknown → anything | No | Initial detection |
| Speaker ↔ Earpiece | No | Same hardware path |
| A2DP ↔ Speaker/Earpiece | No | Both output-only |
| All other transitions | **Yes** | Different hardware path |

When a restart does occur, drift compensation is reset because the new audio device likely has a different clock source.

---

## Health Watchdog

The health watchdog monitors playback quality and reports state transitions to JavaScript. It runs on the decode thread (already periodic) and reads only atomics — no allocation, no locks.

### State Machine

```
         ┌─── feedData() after start() ───► Buffering
         │                                      │
         │                    buffer filled +   │
         │                    sync < 45ms       ↓
    start()                               ► Healthy ◄─── recovered
         │                                      │
         │          sustained underruns /        │
         │          decode errors                ↓
         │                                  Degraded
         │                                      │
         │                    feed gap > 500ms  │
         │                                      ↓
         │                                  Stalled
         │                                      │
         │          decode thread detached /     │
         │          audio output stopped         ↓
         └──────────────────────────────────► Failed
```

Events fire on **transitions only** — not repeatedly while in a state. This prevents JS thread flooding during sustained degradation. A rate limit of one event per 500ms provides additional protection during cascading failures.

The watchdog uses `weak_from_this` + `CallInvoker::invokeAsync` to dispatch callbacks to the JS thread safely. This avoids use-after-free if the pipeline is destroyed while a callback is in flight.

---

## Platform Audio Bridges

### iOS: AVAudioOutputBridge

- **AudioUnit:** RemoteIO (`kAudioUnitSubType_RemoteIO`), output-only
- **Format:** float32, mono, at hardware sample rate
- **Resampling:** Speex resampler when hardware rate != 48kHz (common on Bluetooth, which often grants 16kHz or 24kHz)
- **Callback drain:** Tracks in-flight callbacks via atomic counter. Waits for drain before AudioUnit disposal — prevents use-after-free of the callback context.
- **Restart:** Dedicated thread with exponential backoff (100ms base, 3200ms max, 5 retries)
- **Warm-up:** Pre-starts AudioUnit with silence for zero-latency first-frame playback

### Android: AAudioOutputBridge

- **API:** AAudio with `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
- **Sharing:** Exclusive mode preferred (lower latency), falls back to Shared if unavailable or sample rate mismatch
- **Format:** float32, mono, 48kHz requested
- **Resampling:** Speex resampler when AAudio grants a different sample rate
- **Error callback:** Stores error via atomic (no I/O on RT thread), schedules restart via atomic flag
- **Restart:** Dedicated thread with same backoff strategy as iOS

### Resample Buffer Sizing

Both platforms pre-allocate source buffers for worst-case: 4096 hardware frames at 6x upsample ratio (8kHz → 48kHz) = 24,576 samples. The render callback clamps to buffer capacity as defense-in-depth. This worst case covers Bluetooth SCO headsets that negotiate 8kHz.
