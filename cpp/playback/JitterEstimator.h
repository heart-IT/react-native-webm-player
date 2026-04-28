// Audio jitter estimator — thin alias over JitterEstimatorBase with audio config.
// Imported by MediaSessionBase and the sanitizer test suite; staying as a small
// wrapper keeps tests from pulling the full MediaSessionBase transitive graph.
#pragma once

#include "common/JitterEstimatorBase.h"

namespace media {

inline constexpr JitterConfig kAudioJitterConfig{
    .defaultBufferUs = config::jitter::kDefaultBufferUs,
    .minBufferUs = config::jitter::kMinBufferUs,
    .maxBufferUs = config::jitter::kMaxBufferUs,
    .maxReasonableJitterUs = 80000,
    .maxNormalPtsDeltaUs = 3 * config::audio::kFrameDurationUs,
    .minSamplesForEstimate = config::jitter::kMinSamplesForEstimate,
};

using JitterEstimator = JitterEstimatorBase<kAudioJitterConfig>;

}  // namespace media
