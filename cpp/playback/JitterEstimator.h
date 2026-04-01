// Audio jitter estimator — thin alias over JitterEstimatorBase with audio config.
#pragma once

#include "common/JitterEstimatorBase.h"

namespace media {

inline constexpr JitterConfig kAudioJitterConfig{
    .defaultBufferUs = config::jitter::kDefaultBufferUs,
    .minBufferUs = config::jitter::kMinBufferUs,
    .maxBufferUs = config::jitter::kMaxBufferUs,
    .maxReasonableJitterUs = 80000,
    .maxNormalPtsDeltaUs = 60000,   // 3× audio frame duration (20ms)
    .minSamplesForEstimate = config::jitter::kMinSamplesForEstimate,
};

using JitterEstimator = JitterEstimatorBase<kAudioJitterConfig>;

}  // namespace media
