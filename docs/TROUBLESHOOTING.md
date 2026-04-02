# Troubleshooting — @holepunchto/webm-player

Symptom-driven guide for diagnosing and fixing playback issues. Check metrics with `MediaPipeline.getMetrics()` to narrow down the cause.

For architecture context, see [ARCHITECTURE.md](ARCHITECTURE.md). For metrics field definitions, see [TECHNICAL.md](../TECHNICAL.md).

---

## Audio Issues

### Silence or dropouts

**Check:** `quality.underruns` rising

**Cause:** Audio callback has no decoded frames to play — either feed starvation or decode failure.

**Fix:**
1. Check `demux.feedDataCalls` — if zero or stopped incrementing, data is not arriving. Verify your Hypercore stream is connected and producing blocks.
2. Check `quality.decodeErrors` — if rising, Opus decode is failing. This usually means corrupted data. Call `resetStream()` and resume feeding.
3. Check `pools.encodedUnderPressure` — if true, the decode thread is falling behind. Check CPU load on the device.
4. If `stall.state` is `"stalled"`, the player has detected a feed gap. Ensure `setKeyFrameNeededCallback` is wired up so the player can request recovery.

### Clicks or pops

**Check:** `quality.framesDrained` > 0

**Cause:** Fast-drain is discarding frames because a burst of data arrived too quickly. The crossfade should mask most artifacts, but extreme bursts can still produce audible clicks.

**Fix:**
1. Investigate why data is arriving in bursts rather than steadily. This usually indicates upstream buffering (Hypercore read-ahead or network-level batching).
2. If bursts are unavoidable, increase the jitter buffer target: `MediaPipeline.setBufferTarget(100, 100)` — trades latency for smoother absorption.

### Robotic or pitch-shifted audio

**Check:** `drift.driftPpm` > 200

**Cause:** Excessive clock drift between sender and receiver. The drift compensator is resampling too aggressively.

**Fix:**
1. Drift > 200 PPM is unusual — check if the sender's clock is stable.
2. If the issue started after a Bluetooth route change, the drift compensator should have reset automatically. Check `audioOutput.restartCount` — if it's not incrementing on route change, there may be a route detection issue.
3. Verify `drift.currentRatio` — it should be close to 1.0 (normal range: 0.9995 - 1.005).

### Gradual latency increase

**Check:** `jitter.bufferTargetUs` > 100,000

**Cause:** Jitter spikes are inflating the adaptive buffer target. The buffer grows to absorb jitter but may not shrink quickly enough.

**Fix:**
1. Check `quality.gapsOver100ms` and `quality.gapsOver500ms` — if these are climbing, the source is sending data irregularly.
2. Set a manual buffer target to cap latency: `MediaPipeline.setBufferTarget(80, 80)` — accepts more underrun risk for lower latency.
3. If the source is Hypercore, check if read-ahead is batching blocks.

### Stuttery audio

**Check:** `quality.ptsDiscontinuities` > 0

**Cause:** Timestamp jumps in the source stream. The decoder resets at each discontinuity.

**Fix:**
1. This is a source-side issue — the encoder is producing non-monotonic timestamps.
2. If occasional, the player handles it automatically (decoder reset + crossfade).
3. If frequent, fix the encoding side.

---

## Video Issues

### Black screen (no video)

**Check:** `video.framesReceived` and `video.framesDecoded` both at 0

**Cause:** Waiting for the first keyframe. VP9 inter-frames cannot be decoded without a preceding keyframe.

**Fix:**
1. Wait ~500ms for the next natural keyframe in the broadcast.
2. Call `MediaPipeline.requestKeyFrame()` to explicitly request one from the source.
3. Ensure `setKeyFrameNeededCallback` is wired up — the player will request keyframes automatically during stalls.

### Video freezes

**Check:** `video.lateFrames` rising

**Cause:** Network jitter is pushing frames past their render deadline.

**Fix:**
1. Check `video.jitterUs` — if high, the network path is unstable.
2. Increase video buffer target: `MediaPipeline.setBufferTarget(60, 100)` — allows more buffering for video while keeping audio tight.
3. If `video.skippedFrames` is also rising, the player is dropping stale frames to stay near live — this is correct behavior under heavy jitter.

### Green or corrupt video

**Check:** `video.decodeErrors` > 0

**Cause:** VP9 decoder state is corrupted, usually from a missing keyframe or bitstream error.

**Fix:**
1. The player recovers automatically by requesting a keyframe. Check `video.keyFrameRequests`.
2. If corruption persists, call `MediaPipeline.resetStream()` to force a full pipeline reset.
3. If `video.decoderResets` is climbing rapidly (> 3 in 10 seconds), the health watchdog will report `Failed` — you'll need to `stop()` and `start()` the pipeline.

### Lip sync is off

**Check:** `video.avSyncOffsetUs` > 45,000

**Cause:** Audio and video clocks have diverged beyond the correction threshold.

**Fix:**
1. Check `drift.driftPpm` — if high, clock drift is pulling audio away from video. The system should correct automatically, but extreme drift (> 1000 PPM) triggers a `Degraded` health state.
2. Check if this started after a route change — drift compensation resets on route change, and re-convergence takes ~30 seconds.
3. If sustained, `resetStream()` forces both pipelines to re-synchronize from scratch.

---

## System Issues

### `feedData()` returns false

**Cause:** Malformed WebM data. The demuxer encountered a parse error.

**Fix:**
1. Continue feeding — the demuxer retries on the next call. A single parse error does not corrupt state.
2. Check `demux.parseState` — if stuck at 0 (WaitingForEBML), the stream never received a valid EBML header. The first `feedData()` call must include the header.
3. If returns false consistently, the data is not a valid WebM stream.

### Rising memory usage

**Check:** `pools.decodedUnderPressure` or `pools.encodedUnderPressure`

**Cause:** Decode thread is falling behind — frames are accumulating in pools.

**Fix:**
1. Check CPU load. VP9 hardware decode should not bottleneck, but if the device is under heavy load, decode submission may be delayed.
2. Check `health.responsive` — if false, the decode thread has stalled. `health.watchdogTripped` confirms this.
3. If `health.detached` is true, the decode thread was force-detached. This is unrecoverable — call `stop()` then `start()`.

### Audio stops after Bluetooth connect/disconnect

**Check:** `audioOutput.restartCount` rising

**Cause:** Audio output stream restart failed after route change.

**Fix:**
1. The player retries with exponential backoff (100ms to 3200ms, 5 attempts). Check if `audioOutput.lastError` is non-zero after retries exhaust.
2. On Android, verify `BLUETOOTH_CONNECT` permission is granted (required on API 31+). Without it, Bluetooth devices won't appear in `getAvailableAudioDevices()`.
3. If using a specific Bluetooth device, verify it's still in `getAvailableAudioDevices()` after reconnection.

### Health callback reports Failed

**Cause:** Unrecoverable error — decode thread detached, audio output stopped, or repeated decoder resets.

**Fix:**
1. Call `MediaPipeline.stop()` then `MediaPipeline.start()` to rebuild the entire pipeline.
2. After restarting, resume feeding data. The demuxer will need the EBML header again — call `resetStream()` before the first `feedData()` if you're resuming mid-stream.
3. If `Failed` recurs immediately, check device logs for platform-specific errors (MediaCodec/VTDecompressionSession failures, AAudio/AudioUnit errors).

---

## Stall Recovery Issues

### Player stuck in "stalled" state

**Check:** `stall.state` is `"stalled"` and `stall.keyFrameRequests` is incrementing

**Cause:** The player is requesting keyframes but none are arriving.

**Fix:**
1. Verify `setKeyFrameNeededCallback` is connected and the callback is actually requesting a keyframe from the source.
2. Check if the source is still producing data — use `setStreamStatus(StreamStatus.NoPeers)` to tell the health watchdog that the issue is source-side, not player-side.
3. If the source is live but keyframes aren't arriving, check the source's keyframe interval configuration.

### Frequent stall/recovery cycles

**Check:** `stall.stallCount` climbing with matching `stall.recoveryCount`

**Cause:** Intermittent feed gaps just above the 500ms stall threshold.

**Fix:**
1. Check `quality.gapsOver500ms` — these trigger stall detection.
2. Increase buffer target to absorb longer gaps: `MediaPipeline.setBufferTarget(120, 120)`.
3. Investigate the data source — is the Hypercore peer connection unstable?
