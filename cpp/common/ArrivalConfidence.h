// Sliding-window estimator of packet arrival reliability.
//
// Tracks what fraction of recent packets arrived "on time" (within a tight
// deviation tolerance). Used to gate speculative playback: when confidence
// is high, the jitter buffer target is lowered to 1 frame (20ms) instead
// of the full adaptive target, cutting latency by 20-40ms.
//
// Threading: onSample() called from push thread only (single writer).
//            confidence() returns atomic float, safe to read from any thread.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include "MediaConfig.h"

namespace media {

class ArrivalConfidence {
public:
    void onSample(int64_t signedDeviationUs) noexcept {
        int64_t absDeviation = signedDeviationUs < 0 ? -signedDeviationUs : signedDeviationUs;
        uint8_t onTime = absDeviation < config::speculative::kOnTimeThresholdUs ? 1 : 0;

        uint32_t idx = writeIndex_ % kWindowSize;

        if (count_ >= kWindowSize) {
            onTimeCount_ -= window_[idx];
        }

        window_[idx] = onTime;
        onTimeCount_ += onTime;
        writeIndex_++;
        if (count_ < kWindowSize) count_++;

        float c = static_cast<float>(onTimeCount_) / static_cast<float>(count_);
        confidence_.store(c, std::memory_order_relaxed);
    }

    [[nodiscard]] float confidence() const noexcept {
        return confidence_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        window_.fill(0);
        writeIndex_ = 0;
        count_ = 0;
        onTimeCount_ = 0;
        confidence_.store(0.0f, std::memory_order_relaxed);
    }

private:
    static constexpr size_t kWindowSize = config::speculative::kConfidenceWindowSize;

    std::array<uint8_t, kWindowSize> window_{};
    uint64_t writeIndex_{0};
    uint32_t count_{0};
    uint32_t onTimeCount_{0};
    std::atomic<float> confidence_{0.0f};
};

}  // namespace media
