#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include "MediaConfig.h"

namespace media {

// Tracks audio and video render events to compute A/V sync offset.
// Audio is the master clock — video adjusts its buffer target to align.
//
// Sync formula (playbackRate r):
//   expectedWallDelta = (PTS_v - PTS_a) / r
//   offset            = (T_v - T_a) - expectedWallDelta
//   positive → video late (behind audio) → decrease video buffer
//   negative → video early (ahead of audio) → increase video buffer
//
// Dead zones (ITU-R BT.1359):
//   |offset| < 15ms → no correction
//   15-45ms → gradual proportional correction
//   > 45ms → aggressive full correction (+ smoothed state reset so the same
//           correction is not re-applied on every frame)
//
// Thread safety:
//   onAudioRender() — audio callback thread (writes seqlock)
//   onVideoRender() — decode thread (reads seqlock, writes smoothed offset)
//   videoBufferAdjustmentUs() — decode thread (reads + may reset smoothed)
//   currentOffsetUs() — any thread (reads atomic)
class AVSyncCoordinator {
public:
    void onAudioRender(int64_t ptsUs, int64_t localTimeUs) noexcept {
        // Seqlock: odd = writer in progress, even = stable.
        // Avoids the ms-quantisation + int32 wrap of the prior packed approach.
        uint64_t s = seq_.load(std::memory_order_relaxed) + 1;
        seq_.store(s, std::memory_order_release);                    // → odd
        audioPtsUs_.store(ptsUs, std::memory_order_release);
        audioTimeUs_.store(localTimeUs, std::memory_order_release);
        seq_.store(s + 1, std::memory_order_release);                // → even
        hasAudioRef_.store(true, std::memory_order_release);
    }

    void onVideoRender(int64_t ptsUs, int64_t localTimeUs,
                       float playbackRate = 1.0f) noexcept {
        if (!hasAudioRef_.load(std::memory_order_acquire)) return;

        int64_t aPtsUs = 0, aTimeUs = 0;
        if (!readAudioRefSeqlock(aPtsUs, aTimeUs)) return;

        // Playback-rate-aware: at rate r, wall-clock elapses at ptsDelta/r.
        if (!(playbackRate > 0.0f)) playbackRate = 1.0f;
        int64_t actualTimeDeltaUs   = localTimeUs - aTimeUs;
        int64_t expectedPtsDeltaUs  = ptsUs - aPtsUs;
        int64_t expectedTimeDeltaUs = playbackRate == 1.0f
            ? expectedPtsDeltaUs
            : static_cast<int64_t>(static_cast<double>(expectedPtsDeltaUs) /
                                   static_cast<double>(playbackRate));
        int64_t offset = actualTimeDeltaUs - expectedTimeDeltaUs;

        // EWMA smooth (alpha ~= 0.125)
        int64_t smoothed = smoothedOffsetUs_.load(std::memory_order_relaxed);
        smoothed += (offset - smoothed) / 8;
        smoothedOffsetUs_.store(smoothed, std::memory_order_relaxed);
        currentOffsetUs_.store(smoothed, std::memory_order_relaxed);
        lastUpdateUs_.store(localTimeUs, std::memory_order_relaxed);

        int64_t absRaw = offset < 0 ? -offset : offset;
        int64_t peak = peakAbsOffsetUs_.load(std::memory_order_relaxed);
        while (absRaw > peak &&
               !peakAbsOffsetUs_.compare_exchange_weak(peak, absRaw, std::memory_order_relaxed)) {}
        if (absRaw > kEmergencyThresholdUs) {
            syncExceedCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Returns video buffer adjustment in microseconds.
    // Positive = add delay (video is early), negative = reduce delay (video is late).
    // Non-const: the aggressive branch resets smoothed state so the correction
    // is not re-applied every frame while the EWMA decays.
    [[nodiscard]] int64_t videoBufferAdjustmentUs() noexcept {
        int64_t offset = smoothedOffsetUs_.load(std::memory_order_relaxed);
        int64_t absOffset = offset < 0 ? -offset : offset;

        if (absOffset < kDeadZoneUs) return 0;

        int64_t result;
        if (absOffset > kEmergencyThresholdUs) {
            result = -offset;
            // After one-shot aggressive correction, re-measure from zero.
            smoothedOffsetUs_.store(0, std::memory_order_relaxed);
            currentOffsetUs_.store(0, std::memory_order_relaxed);
        } else {
            // Gradual: proportional correction within 15-45ms range
            int64_t scale = absOffset - kDeadZoneUs;
            int64_t correction = (offset * scale) / kCorrectionRangeUs;
            result = -correction;
        }
        return std::clamp(result, -kMaxAdjustmentUs, kMaxAdjustmentUs);
    }

    [[nodiscard]] int64_t currentOffsetUs() const noexcept {
        return currentOffsetUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t peakAbsOffsetUs() const noexcept {
        return peakAbsOffsetUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t syncExceedCount() const noexcept {
        return syncExceedCount_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        seq_.store(0, std::memory_order_relaxed);
        audioPtsUs_.store(0, std::memory_order_relaxed);
        audioTimeUs_.store(0, std::memory_order_relaxed);
        hasAudioRef_.store(false, std::memory_order_relaxed);
        smoothedOffsetUs_.store(0, std::memory_order_relaxed);
        currentOffsetUs_.store(0, std::memory_order_relaxed);
        peakAbsOffsetUs_.store(0, std::memory_order_relaxed);
        syncExceedCount_.store(0, std::memory_order_relaxed);
        lastUpdateUs_.store(0, std::memory_order_relaxed);
    }

    // Called periodically by the video decode thread when no new video frame has
    // rendered. Decays the smoothed offset toward 0 so a stale transient does
    // not lock the buffer target permanently (fixes P1-7 / stale-offset lock-in).
    void tickDecay(int64_t nowUs) noexcept {
        int64_t last = lastUpdateUs_.load(std::memory_order_relaxed);
        if (last == 0) return;
        if (nowUs - last < kStaleDecayThresholdUs) return;
        int64_t smoothed = smoothedOffsetUs_.load(std::memory_order_relaxed);
        if (smoothed == 0) return;
        // Halve toward zero each tick once stale.
        smoothed /= 2;
        smoothedOffsetUs_.store(smoothed, std::memory_order_relaxed);
        currentOffsetUs_.store(smoothed, std::memory_order_relaxed);
        lastUpdateUs_.store(nowUs, std::memory_order_relaxed);
    }

private:
    // Seqlock read; returns false if we could not get a stable snapshot.
    bool readAudioRefSeqlock(int64_t& ptsUs, int64_t& timeUs) const noexcept {
        for (int i = 0; i < 4; ++i) {
            uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;  // writer in progress
            ptsUs  = audioPtsUs_.load(std::memory_order_acquire);
            timeUs = audioTimeUs_.load(std::memory_order_acquire);
            uint64_t s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) return true;
        }
        return false;
    }

    static constexpr int64_t kDeadZoneUs = config::avsync::kDeadZoneUs;
    static constexpr int64_t kEmergencyThresholdUs = config::avsync::kEmergencyThresholdUs;
    static constexpr int64_t kCorrectionRangeUs = config::avsync::kCorrectionRangeUs;
    static constexpr int64_t kMaxAdjustmentUs = config::avsync::kMaxAdjustmentUs;
    static constexpr int64_t kStaleDecayThresholdUs = config::avsync::kStaleDecayThresholdUs;

    // Audio-writer group: RT callback writes these on every render.
    alignas(config::kCacheLineSize) std::atomic<uint64_t> seq_{0};
    std::atomic<int64_t> audioPtsUs_{0};
    std::atomic<int64_t> audioTimeUs_{0};
    std::atomic<bool> hasAudioRef_{false};

    // Video-writer group: decode thread writes these at fps rate.
    alignas(config::kCacheLineSize) std::atomic<int64_t> smoothedOffsetUs_{0};
    std::atomic<int64_t> currentOffsetUs_{0};
    std::atomic<int64_t> lastUpdateUs_{0};
    std::atomic<int64_t> peakAbsOffsetUs_{0};
    std::atomic<uint64_t> syncExceedCount_{0};
};

}  // namespace media
