// Internal limits + defaults for WebM demuxing. Shared between WebmDemuxer.cpp,
// WebmTrackParser.cpp, and WebmBlockParser.cpp. Not part of the public API —
// no consumer outside cpp/demux/ should include this header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace media::demux {

// Minimum bytes before declaring a parse failure (vs needing more data).
inline constexpr size_t kMinBytesForEBML = 64;
inline constexpr size_t kMinBytesForSegment = 256;

// Max retries for header parsing before entering permanent Error state.
inline constexpr int kMaxParseRetries = 3;

// Highest container SamplingFrequency accepted as valid (192 kHz covers every
// real-world rate; WebM/Opus mandates 48 kHz). Values outside (0, this] are
// attacker-chosen doubles whose int cast would be UB — treated as invalid.
inline constexpr double kMaxAudioSampleRate = 192'000.0;

// Minimum interval between repeats of the window-full warning. feedData()
// runs at stream bitrate, so an unratelimited warning on a backpressured path
// floods the log.
inline constexpr int64_t kThrottledLogIntervalUs = 5'000'000;

}  // namespace media::demux
