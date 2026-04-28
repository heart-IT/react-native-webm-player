// VP9 video pipeline constants for broadcast playback.
// See docs/ARCHITECTURE.md for design rationale.
#pragma once

#include <cstdint>
#include <cstddef>
#include "common/MediaConfig.h"

namespace media::video_config {

// VP9 defaults (broadcast playback)
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
// VP9 worst case: ~512KB covers 4K keyframes
constexpr size_t kMaxEncodedFrameSize = 512 * 1024;

// Encoded frame queue depth
constexpr size_t kDecodeQueueDepth = 8;

// Late-frame threshold: half a 30fps frame interval (16666.67us, rounded up).
// Truncation error vs the exact 100000us/6 is <1us — negligible vs the 33333us
// frame cadence and the decoder's own jitter.
constexpr int64_t kLateFrameThresholdUs = 16667;

// Periodic re-anchor interval for VideoRenderClock (matches audio drift window)
constexpr int64_t kReanchorIntervalUs = media::drift_config::kMeasurementWindowUs;

// Gap-driven re-anchor: reset render clock if no frame decoded for > 5s
// Prevents frame burst after medium-length gaps (5-30s)
constexpr int64_t kGapReanchorThresholdUs = 5'000'000;

// Video jitter buffer configuration
namespace jitter {
    constexpr int64_t kDefaultBufferUs = 66000;    // 2 frames @ 30fps — balances latency vs robustness
    constexpr int64_t kMinBufferUs = 33000;         // 1 frame — minimum for decode pipeline
    constexpr int64_t kMaxBufferUs = 200000;        // ~6 frames — caps latency under heavy jitter
    // 3× a 30fps frame interval (3 × 33_333us ≈ 100_000us). Single-frame drops
    // at 30fps produce ~66_666us and must still count as jitter, not discontinuity.
    constexpr int64_t kMaxNormalPtsDeltaUs = 100000;
    // Need 6 arrivals before trusting jitter estimate. Audio uses 8 (20ms × 8 =
    // 160ms warmup); video's 6 × 33ms ≈ 200ms warmup. Both windows are on the
    // same order of magnitude by design; the asymmetry reflects each track's
    // frame cadence, not a tuning oversight.
    constexpr int kMinSamplesForEstimate = 6;
}

// Video decode thread idle sleep (base interval, microseconds)
constexpr int64_t kDecodeIdleSleepUs = 2000;

// Decoder reset backoff (applies uniformly to VideoDecodeThread and platform decoders).
// Exponential: starts at initial, doubles per failure, caps at max. Reset to initial on success.
namespace decoder_reset {
    constexpr int kMaxConsecutiveErrors = 5;
    constexpr int64_t kInitialBackoffUs = 200'000;    // 200ms
    constexpr int64_t kMaxBackoffUs = 2'000'000;      // 2s
}

namespace validation {
    static_assert(kDecodeQueueDepth >= 2);
    static_assert(jitter::kMinBufferUs <= jitter::kDefaultBufferUs);
    static_assert(jitter::kDefaultBufferUs <= jitter::kMaxBufferUs);
    static_assert(kLateFrameThresholdUs < jitter::kMinBufferUs);
}

}  // namespace media::video_config
