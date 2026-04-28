// Catchup behavior policy + drain-decision computation.
//
// When the audio buffer's depth exceeds (target + policyThreshold), the RT
// thread discards decoded frames to snap latency back toward target. The
// policy enum chooses how aggressive that excess threshold is:
//
//   PlayThrough  — large threshold; tolerate buffer growth, never drain.
//   Accelerate   — moderate; drain when sustained excess builds up.
//   DropToLive   — small; drain aggressively to stay near live.
//
// This class is RT-safe: setPolicy() is called from any thread (atomic store);
// evaluate() is called from the audio callback (atomic load). Stateless aside
// from the policy enum.
#pragma once

#include "MediaTypes.h"
#include "common/MediaConfig.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace media {

class FastDrainPolicy {
public:
    struct Decision {
        size_t framesToDrain;  // 0 = no drain needed this callback
        bool burstEnded;       // excess <= 0 — caller should mark drain idle
    };

    void setPolicy(CatchupPolicy p) noexcept {
        policy_.store(p, std::memory_order_relaxed);
    }

    // Compute drain decision from current buffer depth.
    // `decodedFrameCount` is the count in the decoded queue; `hasPartial` is
    // true if a partial frame is being read. The caller is responsible for
    // actually pulling frames out of its queues — this only decides how many.
    [[nodiscard]] Decision evaluate(int64_t totalBufferedUs,
                                    int64_t targetUs,
                                    size_t decodedFrameCount,
                                    bool hasPartial) const noexcept {
        int64_t threshold;
        switch (policy_.load(std::memory_order_relaxed)) {
            case CatchupPolicy::Accelerate:
                threshold = config::fastdrain::kExcessThresholdAccelerate;
                break;
            case CatchupPolicy::DropToLive:
                threshold = config::fastdrain::kExcessThresholdDropToLive;
                break;
            default:
                threshold = config::fastdrain::kExcessThresholdPlayThrough;
                break;
        }
        int64_t excess = totalBufferedUs - targetUs - threshold;
        if (excess <= 0) return {0, true};

        size_t framesToDrain = static_cast<size_t>(excess / config::audio::kFrameDurationUs);
        // Reserve at least 1 decoded frame for output to prevent underrun.
        size_t available = decodedFrameCount + (hasPartial ? 1 : 0);
        size_t maxDrain = available > 1 ? available - 1 : 0;
        if (framesToDrain > maxDrain) framesToDrain = maxDrain;
        return {framesToDrain, false};
    }

private:
    std::atomic<CatchupPolicy> policy_{CatchupPolicy::PlayThrough};
};

}  // namespace media
