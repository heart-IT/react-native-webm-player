// VP9 video pipeline constants for broadcast playback.
// See docs/ARCHITECTURE.md for design rationale.
#pragma once

#include <cstdint>
#include <cstddef>

namespace media::video_config {

// VP9 defaults (broadcast playback)
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
// VP9 worst case: ~512KB covers 4K keyframes
constexpr size_t kMaxEncodedFrameSize = 512 * 1024;

// Encoded frame queue depth
constexpr size_t kDecodeQueueDepth = 8;

// Late-frame threshold: half a 30fps frame interval
constexpr int64_t kLateFrameThresholdUs = 16667;

// Periodic re-anchor interval for VideoRenderClock (30s matches audio drift window)
constexpr int64_t kReanchorIntervalUs = 30'000'000;

// Video jitter buffer configuration
namespace jitter {
    constexpr int64_t kDefaultBufferUs = 66000;    // 2 frames @ 30fps — balances latency vs robustness
    constexpr int64_t kMinBufferUs = 33000;         // 1 frame — minimum for decode pipeline
    constexpr int64_t kMaxBufferUs = 200000;        // ~6 frames — caps latency under heavy jitter
    constexpr int64_t kMaxNormalPtsDeltaUs = 66000; // 2× frame interval — beyond this is a PTS discontinuity
    constexpr int kMinSamplesForEstimate = 6;       // Need 6 arrivals before trusting jitter estimate
}

// Video decode thread idle sleep (base interval, microseconds)
constexpr int64_t kDecodeIdleSleepUs = 2000;

namespace validation {
    static_assert(kDecodeQueueDepth >= 2);
    static_assert(jitter::kMinBufferUs <= jitter::kDefaultBufferUs);
    static_assert(jitter::kDefaultBufferUs <= jitter::kMaxBufferUs);
    static_assert(kLateFrameThresholdUs < jitter::kMinBufferUs);
}

}  // namespace media::video_config
