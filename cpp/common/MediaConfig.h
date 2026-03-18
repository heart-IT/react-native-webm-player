#pragma once

#include <cstdint>
#include <cstddef>
#include "CompilerHints.h"
#include "MediaTime.h"

#ifdef __cpp_lib_hardware_interference_size
    #include <new>
#endif

namespace media::config {

constexpr const char* kVersion = "1.0.0";

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t kCacheLineSize = 64;
#endif

constexpr size_t kSimdAlignment = 16;

namespace limits {
    constexpr int kMaxParticipants = 64;
    constexpr int kMaxAudioSlots = 16;
}

namespace audio {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 1;
    constexpr int kFrameDurationMs = 20;
    constexpr int64_t kFrameDurationUs = kFrameDurationMs * 1000;
    constexpr int kFrameSamples = (kSampleRate * kFrameDurationMs) / 1000;
    constexpr int kFrameSize = kFrameSamples * kChannels;
    constexpr size_t kMaxEncodedFrameSize = 1275;  // Opus spec maximum per packet

    constexpr size_t kDecodePoolSize = 64;
    constexpr size_t kDecodeQueueDepth = 32;
    constexpr size_t kPcmPoolSize = 64;
    constexpr size_t kPcmQueueDepth = 32;
    constexpr size_t kPendingFrameCapacity = 128;

    constexpr int kMaxConsecutiveDecodeErrors = 10;

    static_assert(kFrameSamples % 4 == 0);
    static_assert(kFrameSize % 4 == 0);
    static_assert((kDecodePoolSize & (kDecodePoolSize - 1)) == 0);
    static_assert((kDecodeQueueDepth & (kDecodeQueueDepth - 1)) == 0);
    static_assert((kPendingFrameCapacity & (kPendingFrameCapacity - 1)) == 0);

    constexpr int64_t kPLCThresholdUs = 40000;  // 2 frames proactive — generate PLC while 1 real frame remains
    // Burst absorption ceiling: sized to absorb worst-case Hypercore bursts
    // (75+ frames in <5ms) without front-door drops. Fast-drain in readSamples()
    // discards excess decoded frames to keep actual latency near jitter target.
    constexpr int64_t kMaxBufferedUs = 2560000;
}

namespace jitter {
    // 2 frames (40ms) minimum — spec allows 20ms but single-frame buffer causes
    // rapid Playing↔Underrun oscillation on devices with callback jitter > 1ms.
    constexpr int64_t kMinBufferUs = 40000;
    constexpr int64_t kMaxBufferUs = 200000;
    constexpr int64_t kDefaultBufferUs = 60000;  // 3 frames default for stable playback
    constexpr float kJitterAlpha = 0.125f;
    constexpr float kJitterMultiplier = 2.5f;
    constexpr int kMinSamplesForEstimate = 8;
}

// Bluetooth A2DP conservative latency fallback.
// Per-codec detection requires hidden Android APIs and is unavailable on iOS,
// so we use a conservative default covering worst-case SBC latency (~150ms).
namespace a2dp {
    constexpr int64_t kLatencyUnknownUs = 150000;  // 150ms - conservative SBC worst-case (cross-platform)
}

namespace mix {
    constexpr float kDefaultGain = 1.0f;
    constexpr float kMinGain = 0.0f;
    constexpr float kMaxGain = 2.0f;
}

namespace thread {
    constexpr int64_t kDecodeLoopSleepUs = 500;
    constexpr int64_t kAdmissionIntervalUs = 25000;

    // Idle cycle handling - increase sleep after consecutive idle cycles
    constexpr int kIdleCycleThreshold = 4;

    // Watchdog timeout - detect decode thread stalls
    constexpr int64_t kWatchdogTimeoutUs = 2'000'000;

    // Soft deadline: poll exitFlag up to this long before escalating
    constexpr int64_t kThreadJoinTimeoutMs = 500;

    // Hard deadline: after soft timeout, wait this much longer before detaching.
    // Total worst-case blocking time = kThreadJoinTimeoutMs + kThreadJoinHardTimeoutMs.
    constexpr int64_t kThreadJoinHardTimeoutMs = 500;

    // Health logging
    constexpr int64_t kHealthLogIntervalUs = 10'000'000;  // 10 seconds
    constexpr int kHealthHeartbeatInterval = 5;

    // Audio output liveness check interval (on pushAudioFrame path)
    constexpr int64_t kAudioOutputCheckIntervalUs = 5'000'000;  // 5 seconds
}

namespace epoch {
    constexpr int64_t kMaxForwardJumpUs = 500000;
    constexpr int64_t kMaxBackwardJumpUs = 200000;  // 200ms backward tolerance for packet reordering
    constexpr int kDurationMultiplier = 10;
}

namespace backpressure {
    constexpr int64_t kDecodeLagWarningUs = 30000;
    constexpr int64_t kDecodeLagCriticalUs = 60000;
    constexpr int64_t kNoFrameTimeoutUs = 150000;
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
    constexpr int64_t kExcessThresholdUs = 100000;  // 5 frames
}

namespace crossfade {
    // Underrun/resume crossfade length — prevents clicks on silence transitions.
    // 5ms matches Chromium WebRTC's CNG (comfort noise generator) fade length.
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

namespace logging {
    constexpr uint64_t kDropLogInterval = 64;
    static_assert((kDropLogInterval & (kDropLogInterval - 1)) == 0,
                  "Drop log interval must be power of 2");
}

namespace aaudio {
    constexpr int32_t kBufferBurstMultiplier = 2;
    constexpr int32_t kRestartDelayMs = 100;
}

namespace admission {
    constexpr int64_t kMinSlotHoldUs = 500000;
    constexpr int64_t kInactivityTimeoutUs = 30'000'000;       // 30s before slot release
    constexpr int64_t kInactivityCheckIntervalUs = 5'000'000;  // Check every 5s during idle
}

namespace pool {
    constexpr size_t kBackpressureThresholdPercent = 25;  // Signal backpressure when < 25% available
    // Per-slot decoded frame cap prevents any single slot from exhausting the
    // shared DecodedAudioPool. Without this, N slots in Buffering state could
    // each fill their decoded queue, consuming all pool tokens and stalling
    // decode for all other slots (liveness failure).
    // Value: pool / slots = 64 / 16 = 4 decoded frames (80ms) per slot.
    constexpr size_t kMaxDecodedPerSlot = audio::kDecodePoolSize / limits::kMaxAudioSlots;
    static_assert(kMaxDecodedPerSlot >= 2, "Per-slot cap must allow at least 2 decoded frames");
}

namespace sentinel {
    constexpr int kNoParticipant = -1;
    constexpr int kNoSlot = -1;
    constexpr int64_t kNoTimestamp = INT64_MIN;
}

namespace dtx {
    // DTX silence marker: application-level sentinel for discontinuous transmission.
    // Sent by encoder when DTX detects silence (0 bytes output), recognized by
    // decoder to output clean silence instead of PLC concealment.
    //
    // Collision analysis: {0xFC, 0xFF, 0xFE} IS a structurally valid Opus packet
    // (TOC=0xFC → config 31 = CELT 2.5ms mono, 1 frame, 2-byte payload). However,
    // this encoder only produces 20ms frames (config 15 at 48kHz), and real encoded
    // frames are typically 60-80 bytes at 32kbps. A 3-byte packet with TOC config 31
    // cannot originate from this encoder's configuration. Collision requires:
    //   1. Frame size = 3 bytes (extremely small for any real audio)
    //   2. First byte = 0xFC (wrong frame duration for this encoder)
    //   3. Exact payload bytes {0xFF, 0xFE}
    // Risk is negligible for this closed system; transport corruption is handled by
    // checksums at the Hypercore layer.
    constexpr uint8_t kDtxFrame[3] = { 0xFC, 0xFF, 0xFE };

    // Comfort noise level during DTX silence (replaces dead digital zeros)
    constexpr float kComfortNoiseLevel = 0.001f;        // -60dBFS peak amplitude
    constexpr float kComfortNoiseCutoffHz = 1000.0f;     // Lowpass shaping cutoff

    // Verify our frame duration never produces TOC config 31 (2.5ms CELT)
    static_assert(audio::kFrameDurationMs >= 10,
                  "DTX marker assumes frame duration >= 10ms (TOC config != 31)");
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
    static_assert(audio::kPcmPoolSize >= audio::kPcmQueueDepth + 4);
    static_assert((audio::kPcmPoolSize & (audio::kPcmPoolSize - 1)) == 0);
    static_assert((audio::kPcmQueueDepth & (audio::kPcmQueueDepth - 1)) == 0);

    static_assert(limits::kMaxParticipants > 0 && limits::kMaxParticipants <= 256);
    static_assert(limits::kMaxAudioSlots > 0 && limits::kMaxAudioSlots <= limits::kMaxParticipants);

    static_assert(mix::kMinGain >= 0.0f);
    static_assert(mix::kMaxGain > mix::kMinGain);
    static_assert(mix::kDefaultGain >= mix::kMinGain && mix::kDefaultGain <= mix::kMaxGain);

    static_assert(backpressure::kDecodeLagWarningUs < backpressure::kDecodeLagCriticalUs);

    static_assert(crossfade::kFadeSamples > 0 && crossfade::kFadeSamples <= audio::kFrameSize);
    static_assert(spike::kSpikeMultiplier >= 2);
}

}  // namespace media::config
