#pragma once

#include <cstdint>
#include <cstddef>

namespace media::video_config {

// VP9 defaults (broadcast playback)
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
constexpr int kDefaultFps = 30;
constexpr int kDefaultBitrate = 2'000'000;  // 2 Mbps

// Resolution limits
constexpr int kMinWidth = 160;
constexpr int kMaxWidth = 3840;
constexpr int kMinHeight = 120;
constexpr int kMaxHeight = 2160;
constexpr int kMinFps = 1;
constexpr int kMaxFps = 60;
constexpr int kMinBitrate = 100'000;     // 100 kbps
constexpr int kMaxBitrate = 20'000'000;  // 20 Mbps

// VP9 worst case: ~512KB covers 4K keyframes
constexpr size_t kMaxEncodedFrameSize = 512 * 1024;

// Per-participant encoded frame queue depth
constexpr size_t kDecodeQueueDepth = 8;

// Maximum concurrent video decode streams
constexpr int kMaxVideoSlots = 16;

// Late-frame threshold: half a 30fps frame interval
constexpr int64_t kLateFrameThresholdUs = 16667;

// Periodic re-anchor interval for VideoRenderClock (30s matches audio drift window)
constexpr int64_t kReanchorIntervalUs = 30'000'000;

// Video jitter buffer configuration
namespace jitter {
    constexpr int64_t kDefaultBufferUs = 66000;    // 2 frames @ 30fps
    constexpr int64_t kMinBufferUs = 33000;         // 1 frame
    constexpr int64_t kMaxBufferUs = 200000;        // ~6 frames
    constexpr int64_t kMaxNormalPtsDeltaUs = 66000; // 2× frame interval — discontinuity threshold
    constexpr int kMinSamplesForEstimate = 6;       // Need 6 arrivals before trusting estimate
}

namespace validation {
    static_assert(kMinWidth > 0 && kMaxWidth >= kMinWidth);
    static_assert(kMinHeight > 0 && kMaxHeight >= kMinHeight);
    static_assert(kMinFps > 0 && kMaxFps >= kMinFps);
    static_assert(kMinBitrate > 0 && kMaxBitrate >= kMinBitrate);
    static_assert(kDecodeQueueDepth >= 2);
    static_assert(kMaxVideoSlots > 0 && kMaxVideoSlots <= 64);
}

}  // namespace media::video_config
