# Architecture — @heartit/webm-player

How the broadcast player works internally: design decisions, data flow, and the reasoning behind each subsystem.

For timing constants, metrics, and state-machine tables see [TECHNICAL.md](../TECHNICAL.md); for symptom-driven triage see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## Overview

The player receives a muxed WebM stream (VP9 + Opus) from JavaScript, demuxes it in shared C++, and routes packets to platform-specific decode/render pipelines. **Audio is the master clock** for A/V sync.

```
Hypercore ─► Bare Thread ─RPC(muxed WebM)─► JS (feedData forwarding only)
                                               │ JSI
                                               ▼
                     IngestRingBuffer (lock-free SPSC) ─► IngestThread
                                               │
                                               ▼
                     BroadcastPipeline: DemuxStage → AudioPacket[] + VideoPacket[]
                              │                                          │
                              ▼ Opus                                     ▼ VP9
                     Audio pipeline                                Video pipeline
                     Opus → Jitter → Drift → Mixer →           HW decode → JitterBuf →
                     AAudio / RemoteIO                          VideoSync → Surface
                              ▲                                          ▲
                              └────── AVSyncCoordinator ─────────────────┘
                                      (audio = master clock)
```

### Why this layering

The architecture separates concerns across three layers — JS, shared C++, platform native — to enforce one constraint: **JavaScript has no timing authority**. All pacing, buffering, and sync happens in native code where real-time guarantees are possible.

JS's only job is to call `feedData()`. This keeps the JS thread free for UI and prevents GC pauses from affecting playback. The shared C++ layer (demuxer, audio DSP, video sync, jitter) is identical on both platforms. Platform-specific code is limited to audio output (AAudio / RemoteIO) and video decode (MediaCodec / VTDecompressionSession) — ensuring iOS and Android have identical timing behavior.

### Layer responsibilities

| Layer | Does | Does NOT |
|-------|------|----------|
| Bare Thread | Hypercore read, deliver muxed WebM | Demux, decode, process |
| JavaScript | Receive callback, call `feedData()` | Buffer, DSP, time, pace, demux |
| Native demuxer | Parse WebM, emit Opus + VP9 packets | Decode, play |
| Native audio | Opus decode, jitter, drift, mixer, route | Capture, encode, net I/O |
| Native video | VP9 HW decode, render sync, surfaces | Encode, net I/O |
| Native transcript | 48→16kHz resample, whisper.cpp, text | Block audio/video, net I/O |

---

## Data flow

### `feedData()` path

```
JS: feedData(webmBytes)
 → JSI: validate, extractBuffer() (zero or single copy)
 → StallRecoveryController::onDataReceived()  (reset stall detection)
 → IngestRingBuffer::write()                   (lock-free SPSC)
 → wake IngestThread
──── IngestThread ────
 → IngestRingBuffer::read()
 → BroadcastPipeline::process()
   → DemuxStage wraps WebmDemuxer::feedData()
     → StreamReader::append() (internal buffer)
     → Incremental parse (fallthrough state machine)
     → DemuxResult { AudioPacket[], VideoPacket[], error, newClusterCount }
   → AudioPackets → AudioDecodeChannel::pushEncodedFrame()
   → VideoPackets → VideoFrameQueue::pushEncodedFrame()
   → ClipIndex: cluster position + keyframe events
 → bool to JS (false on parse error)
```

The `IngestRingBuffer` decouples JS from demuxing — `feedData()` returns as soon as bytes are written. `ClipIndex` taps the demux output (cluster positions + keyframe events), downstream of `DemuxStage`.

### Audio pipeline

```
feedData → demux → AudioDecodeChannel::pushEncodedFrame()
 → PendingAudioPool (16 slots, pre-allocated)
 → PendingAudioQueue (SPSC: JS → decode)
 → DecodeThread::processPendingDecode()
   → Pool token acquired BEFORE popping encoded queue (no frame loss)
   → Opus decode → DecodedAudioFrame (960 samples, 20ms)
   → DecodedAudioQueue (SPSC: decode → audio callback)
 → Audio callback: readSamples()  [LOCK-FREE]
   → Drift compensation (Speex if ratio ≠ 1.0)
   → Crossfade on PLC→real transitions
   → Fast-drain with crossfade for burst backlogs
 → AudioMixer: gain (NEON/SSE SIMD)
 → AudioLevelMeter: peak/RMS (NEON/SSE SIMD, atomic store)
 → Platform output (AAudio / RemoteIO)
```

**Critical invariant:** the decode thread acquires a pool token *before* popping the encoded queue. Without this, if the pool is exhausted when the decode thread pops, the encoded frame has nowhere to go — a subtle frame-loss race.

### Video pipeline

```
feedData → demux → VideoFrameQueue::pushEncodedFrame()
 → Keyframe gating (drop non-keyframes until first keyframe)
 → Mutex-protected deque (max 8 frames)
 → VideoDecodeThread::tryPopReadyFrame()
   → VideoRenderClock: wait until scheduled render time
   → Skip stale frames (> half frame-interval late)
 → VP9 HW decode (MediaCodec / VTDecompressionSession)
 → Surface render (ANativeWindow / AVSampleBufferDisplayLayer)
 → AVSyncCoordinator: report video render PTS
```

Video uses a mutex-protected deque (not lock-free): video decode runs at ~30Hz vs audio's ~50Hz, and HW decode submission is the bottleneck anyway — mutex overhead is negligible vs GPU dispatch.

---

## A/V synchronization

Audio is the master clock. Rationale: audio hardware runs on a crystal oscillator with extremely stable timing; video frame scheduling is inherently jittery (GPU load, compositor scheduling, vsync). Using audio as reference means the most perceptible artifact (pitch/timing changes) never occurs.

`AVSyncCoordinator` tracks the offset between audio and video render PTS using ITU-R BT.1359 perceptibility thresholds. See [TECHNICAL.md → A/V sync](../TECHNICAL.md#av-sync) for the correction table.

Video adjusts its jitter buffer target based on offset. **Audio never adjusts for video** — pitch artifacts from adjusting audio are far more noticeable than a slightly early/late video frame.

---

## Demuxer state machine

```
                    feedData() with EBML magic
WaitingForEBML ──────────────────────────────► WaitingForSegment
     ▲                                                │
     │ reset()                        Segment created │
     │                                                ▼
  [any state]                                   ParsingTracks
                                                      │
                                Tracks loaded         │
                                (A_OPUS + V_VP9)      ▼
                                                  Streaming
                                             (extracting blocks)
```

The demuxer is incremental — it processes partial data. `feedData()` receives arbitrarily-sized chunks; a single WebM cluster may arrive across multiple calls. Each state tries to parse and falls through on success; if more data is needed, it returns an empty result (no error) and the caller feeds more on the next call. This avoids blocking and means the demuxer never buffers beyond the current parse window.

Buffer compaction discards consumed bytes before the current cluster to prevent unbounded growth. `DemuxResult` packets point into the demuxer's internal buffer and are valid only until the next `feedData()` / `reset()` — both pipelines copy immediately.

---

## Jitter buffer

Adaptive EWMA estimator with spike detection and burst memory. Fixed buffers are either too small (underruns during spikes) or too large (unnecessary latency). The adaptive approach converges to the minimum safe buffer for current conditions.

**Algorithm:**

1. **Arrival jitter:** inter-packet arrival time vs. expected interval (20ms for Opus).
2. **EWMA smoothing:** `jitter = α·sample + (1−α)·jitter` with α = 0.125 — tracks trends without overreacting to individual variations.
3. **Spike detection:** if deviation >3× current estimate, bypass EWMA and jump immediately. Otherwise a sudden degradation takes many packets to converge, causing underruns.
4. **Spike hold:** maintain the elevated target for 2s. Jitter tends to cluster — dropping the target right after a spike often leads to another underrun.
5. **Burst memory:** ring of 4 recent spikes. If 2+ within 30s, hold a floor at 75% of minimum recent spike jitter. Prevents oscillating on periodic jitter sources.
6. **Target:** `max(jitter × 2.5, minBuffer, burstFloor)` clamped to `[40ms, 200ms]`.

**Drift tracking:** same arrival timestamps feed a 30s measurement of cumulative sample-count vs. wall-clock delta, reported as PPM to `DriftCompensator`. Zero additional measurement overhead.

---

## Stream recovery

### Keyframe gating

`VideoFrameQueue` starts with `needsKeyFrame_ = true`. All non-keyframe VP9 packets are dropped until a keyframe arrives — decoding inter-frames without the reference produces green/corrupt output, worse than showing nothing.

- **Stream start:** black until first keyframe (~500ms typical).
- **After decode error:** `requestKeyFrame()` clears the queue and re-enables gating.
- **Upstream signal:** `setKeyFrameNeededCallback()` lets JS request a keyframe from the source — reduces recovery time below natural keyframe interval.

### PTS discontinuity

`AudioDecodeChannel` detects PTS jumps (>500ms forward or >200ms backward) as discontinuity (restart, seek, encoder reset). Response:

1. Set `needsDecoderReset_`.
2. Decode thread resets Opus on next cycle — Opus has internal state that produces artifacts on discontinuous input.
3. Crossfade prevents clicks at the splice.
4. `ptsDiscontinuities` increments.

The asymmetry (500 vs 200ms) reflects that forward jumps are common during network gaps while backward jumps indicate unusual conditions (restart, source seek).

### Automatic stall recovery

```
Healthy ─► Detecting (gap >250ms) ─► Stalled (gap >500ms) ─► Recovering ─► Healthy
                                           │
                                           ▼ (gap >30s)
                                        Failed
```

Two-phase detection (250→500ms) avoids false positives from normal jitter while responding to real stalls. The 250ms "Detecting" phase is monitoring-only — the system primes but takes no action. First keyframe request fires immediately on Detecting→Stalled (500ms); subsequent re-requests every 2s.

### Feed starvation

- **Audio:** → Underrun, silence, PLC up to 160ms (8 frames).
- **Video:** holds last rendered frame (freezing is less jarring than black).
- **Recovery:** decoder reset if gap >500ms; crossfade prevents click.

---

## Clip capture

`ClipIndex` maintains a rolling index of cluster positions + keyframes from the demux output. **Key design: zero re-encoding.** Clips are extracted by prepending the cached EBML+Segment+Tracks header to selected cluster data — a valid standalone WebM file, instantly.

Re-encoding would require a VP9 encoder (~100ms/frame) and degrade quality. By capturing raw muxed bytes, extraction is O(memcpy) regardless of duration.

```
DemuxStage → ClipIndex::onNewCluster() / onKeyFrame() / onBlockPts()
           → setStreamHeader()  (cached once at Streaming state)

captureClip(seconds) → walk keyframe index backward → copy header + clusters
                     → file I/O on background thread → Promise<string>
```

- **Circular byte buffer:** 4MB default (~30s at typical bitrate).
- **Cluster index:** ring of up to 512 `ClusterRecord` entries (ring offset, size, PTS, keyframe flag).
- **Keyframe-aligned extraction:** clips always start at a keyframe cluster.
- **Wrap-around safety:** monotonic byte counter detects overwritten data.
- **Thread safety:** mutex (JS writes, background reads; neither is RT).

Output is a standalone WebM file playable anywhere, and can be published as a new Hypercore for P2P distribution.

---

## DVR timeshift

`seekTo()` reuses ClipIndex to seek backward into buffered data without network requests:

1. `targetPts = currentTimeUs + offsetSeconds × 1e6`
2. Set `timeshifting_ = true` — incoming `feedData()` accumulates in `livePendingBuffer_` instead of demuxing.
3. `extractFromPts(targetPts)` → header + clusters from nearest keyframe.
4. Reset demuxer + audio/video pipelines (keep ClipIndex).
5. Re-feed extracted bytes through the demuxer in 64KB chunks, dispatching packets normally.
6. Drain `livePendingBuffer_` to catch up to live.
7. Clear `timeshifting_`.

`livePendingBuffer_` is critical: without it, `feedData()` calls during rebuild would be silently dropped.

---

## Audio level metering

`AudioLevelMeter` computes peak/RMS on every audio callback with zero allocations:

- **SIMD:** NEON (ARM) and SSE (x86) — 4 samples per instruction.
- **Lock-free:** atomic store; readable from any thread via `getMetrics()`.
- **Clip detection:** counts frames exceeding 1.0.

JS polls `getMetrics()` for VU meters without any sync overhead or RT-budget impact.

---

## Audio routing

Reactive model — zero state machines, zero timers. `RouteHandler` (shared C++) receives OS route change events and makes one decision: dedup, restart, drift reset, JS callback.

A state-machine approach would track connecting/connected/disconnecting states with debouncing timers. The OS already does this internally; duplicating it adds complexity without benefit.

**iOS:** `AVAudioSessionRouteChangeNotification` → serial GCD queue → `detectCurrentRoute()` → `RouteHandler::onRouteDetected()`. Category `.playback` with `.allowBluetoothA2DP`.

**Android:** Kotlin `AudioSessionBridge` → JNI `nativeOnAudioRouteChanged` → `RouteHandler::onRouteDetected()`. `setCommunicationDevice()` on API 31+, legacy `setSpeakerphoneOn` / `startBluetoothSco` below.

Restart matrix: see [TECHNICAL.md → Audio route restart matrix](../TECHNICAL.md#audio-route-restart-matrix). When a restart occurs, drift compensation resets because the new device likely has a different clock source.

---

## Health watchdog

Runs on the decode thread (already periodic), reads only atomics — no allocation, no locks.

```
       ┌── feedData() after start() ──► Buffering
       │                                    │  buffer filled + sync <45ms
       │                                    ▼
   start() ────────────────────────────► Healthy ◄── recovered
       │                                    │   sustained underruns /
       │                                    │   decode errors
       │                                    ▼
       │                                Degraded
       │                                    │   feed gap >500ms
       │                                    ▼
       │                                 Stalled
       │                                    │   decode thread detached /
       │                                    │   audio output stopped
       └────────────────────────────────► Failed
```

Events fire on **transitions only** — not repeatedly while in a state. Prevents JS flooding during sustained degradation. Rate-limited to 1/500ms for additional protection during cascading failures.

Callbacks dispatch to JS via `weak_from_this` + `CallInvoker::invokeAsync` — avoids use-after-free if the pipeline is destroyed with a callback in flight.

State definitions and transition conditions: [TECHNICAL.md → Health watchdog](../TECHNICAL.md#health-watchdog).

---

## Platform audio bridge details

Common: float32 mono, restart with exponential backoff (peak sleep 1600ms at the 4th failure; the 5th failure triggers the fatal-exit branch, so the 3200ms cap is never reached), Speex resampler when HW rate ≠ 48kHz, 4096 × 6 = 24,576-sample pre-allocated source buffer.

**iOS (AVAudioOutputBridge):** RemoteIO AudioUnit, output-only. Hardware sample rate. Callback-drain: atomic counter of in-flight callbacks; waits for drain before AudioUnit disposal (prevents UAF on callback context). Warm-up with silence for zero-latency first-frame. Category `.playback` + `.allowBluetoothA2DP`.

**Android (AAudioOutputBridge):** AAudio, `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`. Exclusive mode preferred (lower latency), Shared fallback if unavailable or sample rate mismatch. 48kHz requested. Error callback stores via atomic (no I/O on RT thread), schedules restart via atomic flag.

**Resample worst case:** 4096 HW frames × 6× upsample ratio (8kHz → 48kHz) = 24,576 samples. Covers Bluetooth SCO (8kHz).
