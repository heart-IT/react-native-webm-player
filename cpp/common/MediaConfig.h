// Central timing and sizing constants for the broadcast playback pipeline.
// All values are compile-time constants validated by static_assert at the bottom.
// See docs/ARCHITECTURE.md for design rationale behind key values.
#pragma once

#include <cstdint>
#include <cstddef>
#include "CompilerHints.h"
#include "MediaTime.h"

#ifdef __cpp_lib_hardware_interference_size
    #include <new>
#endif

namespace media {

enum class CatchupPolicy : uint8_t { PlayThrough = 0, Accelerate = 1, DropToLive = 2 };

// Mirrors src/types/playback.ts — values are an ABI contract with JS.
enum class PlaybackState : uint8_t {
    Idle = 0,
    Buffering = 1,
    Playing = 2,
    Paused = 3,
    Stalled = 4,
    Failed = 5
};

}  // namespace media

namespace media::config {

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t kCacheLineSize = 64;
#endif

constexpr size_t kSimdAlignment = 16;

namespace audio {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 1;
    constexpr int kFrameDurationMs = 20;
    constexpr int64_t kFrameDurationUs = kFrameDurationMs * 1000;
    constexpr int kFrameSamples = (kSampleRate * kFrameDurationMs) / 1000;
    constexpr int kFrameSize = kFrameSamples * kChannels;
    constexpr size_t kMaxEncodedFrameSize = 1275;  // Opus spec maximum per packet

    constexpr size_t kDecodePoolSize = 16;
    constexpr size_t kDecodeQueueDepth = 8;
    constexpr size_t kPendingFrameCapacity = 16;

    constexpr int kMaxConsecutiveDecodeErrors = 10;

    static_assert(kFrameSamples % 4 == 0);
    static_assert(kFrameSize % 4 == 0);
    static_assert((kDecodePoolSize & (kDecodePoolSize - 1)) == 0);
    static_assert((kDecodeQueueDepth & (kDecodeQueueDepth - 1)) == 0);
    static_assert((kPendingFrameCapacity & (kPendingFrameCapacity - 1)) == 0);

    constexpr int64_t kPLCThresholdUs = 60000;  // 3 frames proactive — generate PLC when buffer drops below 60ms
    // Burst absorption ceiling: upper bound on buffered audio duration.
    // Fast-drain in readSamples() discards excess decoded frames to keep
    // actual latency near jitter target.
    constexpr int64_t kMaxBufferedUs = 400000;  // 400ms — 20 frames
}

namespace jitter {
    // 2 frames (40ms) minimum — spec allows 20ms but single-frame buffer causes
    // rapid Playing↔Underrun oscillation on devices with callback jitter > 1ms.
    constexpr int64_t kMinBufferUs = 40000;
    constexpr int64_t kMaxBufferUs = 200000;     // 200ms upper bound prevents runaway latency
    constexpr int64_t kDefaultBufferUs = 60000;  // 3 frames — stable on WiFi, converges down on good links
    // The EWMA smoothing coefficient itself lives as fixed-point `kJitterAlpha = 8192`
    // (0.125) in JitterEstimatorBase — that's the one the estimator actually consumes.
    constexpr int kMinSamplesForEstimate = 8;    // Need 8 arrivals before trusting the estimate
}

namespace mix {
    constexpr float kDefaultGain = 1.0f;
    constexpr float kMinGain = 0.0f;
    constexpr float kMaxGain = 2.0f;
}

namespace thread {
    constexpr int64_t kDecodeLoopSleepUs = 500;

    // Idle cycle handling - increase sleep after consecutive idle cycles
    constexpr int kIdleCycleThreshold = 4;

    // Soft deadline: poll exitFlag up to this long before escalating
    constexpr int64_t kThreadJoinTimeoutMs = 500;

    // Health logging
    constexpr int64_t kHealthLogIntervalUs = 10'000'000;  // 10 seconds
    constexpr int kHealthHeartbeatInterval = 5;

    // Audio output liveness check interval (on pushAudioFrame path)
    constexpr int64_t kAudioOutputCheckIntervalUs = 5'000'000;  // 5 seconds
}

namespace ingest {
    // Sized for bursty Hypercore delivery: a 4s burst at 2Mbps is ~1MB. Below this, burst arrivals
    // overflow the ring, silently drop bytes (ingestBytesDropped), and force a keyframe request.
    constexpr size_t kRingBufferCapacity = 2 * 1024 * 1024;  // 2MB, ~8s at 2Mbps
    constexpr size_t kRingBufferCapacityWithClip = 8 * 1024 * 1024;  // 8MB, ~30s at 2Mbps
    static_assert((kRingBufferCapacity & (kRingBufferCapacity - 1)) == 0,
                  "ring buffer capacity must be power of 2");
    static_assert((kRingBufferCapacityWithClip & (kRingBufferCapacityWithClip - 1)) == 0,
                  "ring buffer capacity with clip must be power of 2");
}

namespace epoch {
    constexpr int64_t kMaxForwardJumpUs = 500000;
    constexpr int64_t kMaxBackwardJumpUs = 200000;  // 200ms backward tolerance for packet reordering
}

// Inter-frame arrival gap histogram buckets (shared between audio + video metrics).
// Health watchdog compares deltas against these to distinguish jitter spikes (50–100ms)
// from stalls (500ms+).
namespace gap {
    constexpr int64_t kBucket50msUs = 50'000;
    constexpr int64_t kBucket100msUs = 100'000;
    constexpr int64_t kBucket500msUs = 500'000;
}

// A/V sync thresholds. ITU-R BT.1359 dead zone ±15ms, emergency ±45ms.
// These also double as the HealthWatchdog A/V-sync-degraded threshold —
// kept here so the two live in lockstep.
namespace avsync {
    constexpr int64_t kDeadZoneUs = 15000;
    constexpr int64_t kEmergencyThresholdUs = 45000;
    constexpr int64_t kCorrectionRangeUs = kEmergencyThresholdUs - kDeadZoneUs;
    constexpr int64_t kMaxAdjustmentUs = 200000;  // matches jitter::kMaxBufferUs
    constexpr int64_t kStaleDecayThresholdUs = 2'000'000;
}

namespace catchup {
    // Buffer-depth-driven accelerated playout to drain burst backlog.
    // When buffered duration exceeds jitter target, speed up playback slightly
    // to bring latency back down — matching Chromium WebRTC jitter buffer behavior.

    // Excess threshold: only speed up when buffered - target exceeds this.
    // Prevents micro-adjustments from normal jitter variation.
    constexpr int64_t kExcessThresholdUs = 40000;  // 2 frames

    // Maximum speedup ratio for catchup (e.g. 1.05 = 5% faster)
    constexpr float kMaxSpeedupRatio = 1.05f;

    // Gain per microsecond of excess: maps excess duration to ratio bias.
    // At 200ms excess: 200000 * 0.00000025 = 0.05 → 1.05x (max).
    // At 100ms excess: 100000 * 0.00000025 = 0.025 → 1.025x.
    constexpr float kExcessToRatioGain = 0.00000025f;

    // Smoothing alpha for catchup ratio changes (per audio callback, ~20ms).
    // Slower than drift smoothing to avoid audible pitch wobble during catchup.
    // 0.02 chosen to reduce ratio jitter on Bluetooth where buffer depth
    // fluctuates more due to higher system latency.
    constexpr float kSmoothingAlpha = 0.02f;

    // Decay alpha: return to unity 3x faster than ramp-up.
    // Reduces pitch wobble when buffer normalizes after burst.
    constexpr float kDecayAlpha = 0.06f;

    // Dead-zone: snap catchup ratio to unity when within this threshold.
    // Prevents micro-oscillation that triggers fast-path/resampler switching.
    // Must equal kFastPathUnityThreshold so both dead-zone and hysteresis
    // agree on what "near unity" means.
    constexpr float kDeadZoneThreshold = 0.001f;

    // Fast-path unity threshold: combined ratio within this band uses direct
    // copy instead of Speex resampler. Shared with kDeadZoneThreshold to
    // prevent a gap where dead-zone doesn't snap but hysteresis oscillates.
    constexpr float kFastPathUnityThreshold = 0.001f;
}

namespace fastdrain {
    // Post-decode accelerated drain: discard decoded frames when buffer depth
    // far exceeds jitter target. Crossfade splice at the skip boundary prevents
    // audible clicks. Reserved for genuine burst backlogs that the 5% catchup
    // resampler cannot drain in reasonable time.
    //
    // Must be significantly higher than catchup::kExcessThresholdUs so the
    // resampler handles normal fluctuations (1-3 extra frames) inaudibly,
    // and fast-drain only fires for large backlogs (5+ extra frames).
    // At 5% speedup, the resampler drains 100ms excess in ~2 seconds —
    // fast enough for normal jitter, no audible time skip.
    constexpr int64_t kExcessThresholdPlayThrough = 100000;  // 5 frames
    constexpr int64_t kExcessThresholdAccelerate = 60000;    // 3 frames
    constexpr int64_t kExcessThresholdDropToLive = 20000;    // 1 frame
}

namespace silenceskip {
    // Silence-skip catchup: when the buffer has moderate excess and a decoded
    // frame is silence, skip it entirely — 20ms of latency erased instantly
    // with zero audible artifact.  Complementary to rate-based catchup (which
    // handles music) and fast-drain (which handles large backlogs).
    // Peak amplitude below this threshold is considered silence.
    // -50 dBFS ≈ 0.00316 linear — well below perceptible audio.
    constexpr float kSilencePeakThreshold = 0.00316f;
    // Only skip when decoded buffer exceeds target by at least this much.
    // Set to 1 frame so silence-skip fires before the catchup resampler
    // kicks in, resolving excess faster when speech pauses are available.
    constexpr int64_t kExcessThresholdUs = 20000;  // 1 frame
    // Maximum consecutive silence frames to skip per callback, to avoid
    // draining the entire buffer during a long pause.
    constexpr size_t kMaxSkipsPerCallback = 4;  // 80ms max skip per callback
}

namespace crossfade {
    // Underrun/resume crossfade length — prevents clicks on silence transitions.
    // 5ms matches Chromium WebRTC's fade length.
    constexpr size_t kFadeSamples = 240;  // 5ms at 48kHz
}

namespace spike {
    // Jitter spike detection: bypass EWMA for sudden jitter increases.
    // A single late packet should immediately raise the buffer target.
    constexpr int64_t kSpikeMultiplier = 3;       // Spike if deviation > 3x current estimate
    constexpr int64_t kSpikeHoldTimeUs = 2'000'000;  // Hold elevated target for 2 seconds

    // Burst memory: track recurring spike patterns to prevent jitter estimate
    // from decaying back to minimum between periodic bursts (e.g., Hypercore
    // delivers frames in bursts every 6-8 seconds on BT SCO).
    constexpr int kBurstHistorySize = 4;                  // Ring buffer of recent spike records
    constexpr int64_t kBurstPatternWindowUs = 30'000'000; // 30s — spikes within this window form a pattern
    constexpr int kMinSpikesForPattern = 2;               // Minimum spikes to activate burst floor
    constexpr float kBurstFloorDamping = 0.75f;           // Floor = min(recent spike jitters) * 0.75
}

namespace speculative {
    constexpr size_t kConfidenceWindowSize = 50;         // 1 second of packets at 20ms/frame
    constexpr int64_t kOnTimeThresholdUs = 5000;         // 5ms deviation tolerance
    constexpr float kConfidenceThreshold = 0.90f;        // 90% on-time to enable speculative mode
    constexpr uint32_t kSpeculativePLCHoldFrames = 3;    // Hold Playing for 3 PLC frames before Underrun

    static_assert(kConfidenceWindowSize >= 10, "window too small for stable estimate");
    static_assert(kSpeculativePLCHoldFrames <= 8,
                  "PLC hold must not exceed max consecutive PLC");
}

namespace logging {
    constexpr uint64_t kDropLogInterval = 64;
    static_assert((kDropLogInterval & (kDropLogInterval - 1)) == 0,
                  "Drop log interval must be power of 2");
}

namespace playbackrate {
    constexpr float kMinRate = 0.5f;
    constexpr float kMaxRate = 2.0f;
    constexpr float kDefaultRate = 1.0f;
}

namespace audio_output {
    constexpr int32_t kRestartDelayMs = 100;
}

namespace aaudio {
    constexpr int32_t kBufferBurstMultiplier = 2;
}

namespace pool {
    constexpr size_t kBackpressureThresholdPercent = 25;
}

namespace sentinel {
    constexpr int64_t kNoTimestamp = INT64_MIN;
}

namespace plc {
    // After this many consecutive PLC frames, Opus PLC quality degrades severely.
    // Reset decoder and let the mixer's CNG handle sustained gaps instead.
    // 8 frames * 20ms = 160ms — well within acceptable concealment duration.
    constexpr uint32_t kMaxConsecutivePLC = 8;
}

namespace decoder {
    // OSCE (Deep PLC) activates at decoder complexity >= 6 (LACE) or >= 7 (NoLACE).
    // NoLACE provides better concealment quality than LACE at +1.1MB binary cost.
    // DNN inference runs only during packet loss, not on every decoded frame.
    constexpr int kComplexity = 7;
}

namespace validation {
    static_assert(kCacheLineSize >= 64);
    static_assert((kCacheLineSize & (kCacheLineSize - 1)) == 0);
    static_assert(kSimdAlignment >= 16);

    static_assert(audio::kSampleRate == 48000 || audio::kSampleRate == 24000 ||
                  audio::kSampleRate == 16000 || audio::kSampleRate == 12000 ||
                  audio::kSampleRate == 8000);
    static_assert(audio::kChannels >= 1 && audio::kChannels <= 2);
    static_assert(audio::kFrameDurationMs == 10 || audio::kFrameDurationMs == 20 ||
                  audio::kFrameDurationMs == 40 || audio::kFrameDurationMs == 60);
    static_assert(audio::kFrameSamples == (audio::kSampleRate * audio::kFrameDurationMs) / 1000);
    static_assert(audio::kMaxEncodedFrameSize <= 1500);

    static_assert(audio::kPLCThresholdUs <= jitter::kDefaultBufferUs);
    static_assert(jitter::kDefaultBufferUs < audio::kMaxBufferedUs);

    static_assert(audio::kDecodePoolSize >= audio::kDecodeQueueDepth + 4);

    static_assert(mix::kMinGain >= 0.0f);
    static_assert(mix::kMaxGain > mix::kMinGain);
    static_assert(mix::kDefaultGain >= mix::kMinGain && mix::kDefaultGain <= mix::kMaxGain);

    static_assert(crossfade::kFadeSamples > 0 && crossfade::kFadeSamples <= audio::kFrameSize);
    static_assert(spike::kSpikeMultiplier >= 2);

    static_assert(jitter::kMinBufferUs >= audio::kFrameDurationUs);
    static_assert(jitter::kMinBufferUs <= jitter::kDefaultBufferUs);
    static_assert(jitter::kDefaultBufferUs <= jitter::kMaxBufferUs);
    static_assert(jitter::kMaxBufferUs < audio::kMaxBufferedUs,
                  "max jitter target must be below hard drop ceiling");
    static_assert(fastdrain::kExcessThresholdPlayThrough > fastdrain::kExcessThresholdAccelerate);
    static_assert(fastdrain::kExcessThresholdAccelerate > fastdrain::kExcessThresholdDropToLive);
    static_assert(fastdrain::kExcessThresholdPlayThrough > catchup::kExcessThresholdUs,
                  "PlayThrough fast-drain must fire above catchup threshold");
    static_assert(fastdrain::kExcessThresholdAccelerate > catchup::kExcessThresholdUs,
                  "Accelerate fast-drain must fire above catchup threshold");
    // DropToLive intentionally fires below catchup — aggressive latency reduction
}

}  // namespace media::config

namespace media::drift_config {
    constexpr int64_t kMeasurementWindowUs = 30'000'000;
    constexpr int32_t kMaxDriftPpm = 5000;
}  // namespace media::drift_config
