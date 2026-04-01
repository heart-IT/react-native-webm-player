// Video jitter estimator + render clock for broadcast playback.
#pragma once

#include <atomic>
#include <algorithm>
#include "VideoConfig.h"
#include "common/JitterEstimatorBase.h"

namespace media {

inline constexpr JitterConfig kVideoJitterConfig{
    .defaultBufferUs = video_config::jitter::kDefaultBufferUs,
    .minBufferUs = video_config::jitter::kMinBufferUs,
    .maxBufferUs = video_config::jitter::kMaxBufferUs,
    .maxReasonableJitterUs = 120000,  // higher than audio — video jitter is burstier
    .maxNormalPtsDeltaUs = video_config::jitter::kMaxNormalPtsDeltaUs,
    .minSamplesForEstimate = video_config::jitter::kMinSamplesForEstimate,
};

using VideoJitterEstimator = JitterEstimatorBase<kVideoJitterConfig>;

// Presentation timing for video frames.
// Maps sender PTS to local render time using a fixed anchor + buffer target.
//
// renderTime = anchorLocal + (framePts - anchorPts) + bufferTarget
//
// Thread safety: anchor/updateBufferTarget called from decode thread only.
// isReadyToRender called from decode thread only.
class VideoRenderClock {
public:
    void anchor(int64_t firstPtsUs, int64_t localTimeUs, int64_t bufferTargetUs) noexcept {
        anchorPtsUs_ = firstPtsUs;
        anchorLocalUs_ = localTimeUs;
        bufferTargetUs_ = bufferTargetUs;
        anchored_ = true;
    }

    [[nodiscard]] int64_t scheduledRenderTime(int64_t ptsUs) const noexcept {
        if (!anchored_) return 0;
        float rate = playbackRate_.load(std::memory_order_relaxed);
        int64_t ptsDelta = ptsUs - anchorPtsUs_;
        int64_t scaledDelta = (rate != 1.0f)
            ? static_cast<int64_t>(static_cast<float>(ptsDelta) / rate)
            : ptsDelta;
        return anchorLocalUs_ + scaledDelta + bufferTargetUs_;
    }

    [[nodiscard]] bool isReadyToRender(int64_t ptsUs, int64_t nowUs) const noexcept {
        if (!anchored_) return true;
        return nowUs >= scheduledRenderTime(ptsUs);
    }

    void updateBufferTarget(int64_t newTargetUs) noexcept {
        bufferTargetUs_ = newTargetUs;
    }

    void setPlaybackRate(float rate) noexcept {
        playbackRate_.store(std::clamp(rate,
            config::playbackrate::kMinRate, config::playbackrate::kMaxRate),
            std::memory_order_relaxed);
    }

    [[nodiscard]] bool isAnchored() const noexcept { return anchored_; }

    void reset() noexcept {
        anchorPtsUs_ = 0;
        anchorLocalUs_ = 0;
        bufferTargetUs_ = video_config::jitter::kDefaultBufferUs;
        anchored_ = false;
        playbackRate_.store(1.0f, std::memory_order_relaxed);
    }

private:
    int64_t anchorPtsUs_{0};
    int64_t anchorLocalUs_{0};
    int64_t bufferTargetUs_{video_config::jitter::kDefaultBufferUs};
    bool anchored_{false};
    std::atomic<float> playbackRate_{1.0f};
};

}  // namespace media
