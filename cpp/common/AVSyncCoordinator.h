#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace media {

// Tracks audio and video render events to compute A/V sync offset.
// Audio is the master clock — video adjusts its buffer target to align.
//
// Sync formula:
//   offset = (T_video - T_audio) - (PTS_video - PTS_audio)
//   positive → video late (behind audio) → decrease video buffer
//   negative → video early (ahead of audio) → increase video buffer
//
// Dead zones (ITU-R BT.1359):
//   |offset| < 15ms → no correction
//   15-45ms → gradual proportional correction
//   > 45ms → aggressive full correction
//
// Thread safety:
//   onAudioRender() — called from audio callback thread (writes atomics)
//   onVideoRender() — called from decode thread (reads audio atomics, writes video + offset)
//   videoBufferAdjustmentUs() — called from decode thread (reads smoothedOffset)
//   currentOffsetUs() — called from any thread (reads atomic)
class AVSyncCoordinator {
public:
    void onAudioRender(int64_t ptsUs, int64_t localTimeUs) noexcept {
        audioRenderPtsUs_.store(ptsUs, std::memory_order_release);
        audioRenderTimeUs_.store(localTimeUs, std::memory_order_release);
        hasAudioRef_.store(true, std::memory_order_release);
    }

    void onVideoRender(int64_t ptsUs, int64_t localTimeUs) noexcept {
        if (!hasAudioRef_.load(std::memory_order_acquire)) return;

        int64_t aPts = audioRenderPtsUs_.load(std::memory_order_acquire);
        int64_t aTime = audioRenderTimeUs_.load(std::memory_order_acquire);
        if (aPts == 0) return;

        int64_t actualTimeDelta = localTimeUs - aTime;
        int64_t expectedTimeDelta = ptsUs - aPts;
        int64_t offset = actualTimeDelta - expectedTimeDelta;

        // EWMA smooth (alpha ~= 0.125)
        smoothedOffsetUs_ += (offset - smoothedOffsetUs_) / 8;

        currentOffsetUs_.store(smoothedOffsetUs_, std::memory_order_relaxed);
    }

    // Returns video buffer adjustment in microseconds.
    // Positive = add delay (video is early), negative = reduce delay (video is late).
    [[nodiscard]] int64_t videoBufferAdjustmentUs() const noexcept {
        int64_t offset = smoothedOffsetUs_;
        int64_t absOffset = offset < 0 ? -offset : offset;

        if (absOffset < kDeadZoneUs) return 0;

        if (absOffset > kEmergencyThresholdUs) return -offset;

        // Gradual: proportional correction within 15-45ms range
        int64_t scale = absOffset - kDeadZoneUs;
        int64_t correction = (offset * scale) / kCorrectionRangeUs;
        return -correction;
    }

    [[nodiscard]] int64_t currentOffsetUs() const noexcept {
        return currentOffsetUs_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        audioRenderPtsUs_.store(0, std::memory_order_relaxed);
        audioRenderTimeUs_.store(0, std::memory_order_relaxed);
        hasAudioRef_.store(false, std::memory_order_relaxed);
        smoothedOffsetUs_ = 0;
        currentOffsetUs_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr int64_t kDeadZoneUs = 15000;          // ±15ms — no correction
    static constexpr int64_t kEmergencyThresholdUs = 45000; // ±45ms — full correction
    static constexpr int64_t kCorrectionRangeUs = 30000;    // 45ms - 15ms

    // Audio reference point (written from audio callback via atomic)
    std::atomic<int64_t> audioRenderPtsUs_{0};
    std::atomic<int64_t> audioRenderTimeUs_{0};
    std::atomic<bool> hasAudioRef_{false};

    // Smoothed offset (decode thread only — not atomic, only read via currentOffsetUs_ atomic)
    int64_t smoothedOffsetUs_{0};

    // Atomic snapshot for metrics/API access from any thread
    std::atomic<int64_t> currentOffsetUs_{0};
};

}  // namespace media
