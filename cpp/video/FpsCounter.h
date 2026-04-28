#pragma once

#include <cstdint>

namespace media {

// Lightweight 1-second sliding window FPS counter.
// Not thread-safe — use from a single thread (decode thread).
class FpsCounter {
public:
    // Call once per decoded frame. Returns current FPS estimate.
    int tick(int64_t nowUs) noexcept {
        ++count_;
        if (windowStartUs_ == 0) {
            windowStartUs_ = nowUs;
            return 0;
        }

        int64_t elapsed = nowUs - windowStartUs_;
        if (elapsed >= kWindowUs) {
            currentFps_ = static_cast<int>((static_cast<int64_t>(count_) * 1000000LL) / elapsed);
            count_ = 0;
            windowStartUs_ = nowUs;
        }
        return currentFps_;
    }

    int current() const noexcept { return currentFps_; }

    void reset() noexcept {
        windowStartUs_ = 0;
        count_ = 0;
        currentFps_ = 0;
    }

private:
    static constexpr int64_t kWindowUs = 1000000;  // 1 second

    int64_t windowStartUs_ = 0;
    uint32_t count_ = 0;
    int currentFps_ = 0;
};

}  // namespace media
