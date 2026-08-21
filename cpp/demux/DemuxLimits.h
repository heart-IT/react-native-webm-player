// Internal limits + defaults for WebM demuxing. Shared between WebmDemuxer.cpp,
// WebmTrackParser.cpp, and WebmBlockParser.cpp. Not part of the public API —
// no consumer outside cpp/demux/ should include this header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace media::demux {

// Default audio frame duration for Opus: 20ms.
inline constexpr int64_t kDefaultAudioDurationUs = 20'000;

// Minimum bytes before declaring a parse failure (vs needing more data).
inline constexpr size_t kMinBytesForEBML = 64;
inline constexpr size_t kMinBytesForSegment = 256;

// Max retries for header parsing before entering permanent Error state.
inline constexpr int kMaxParseRetries = 3;

// Consecutive unparseable blocks before reporting a stall.
inline constexpr int kMaxConsecutiveBlockErrors = 3;

// Reject video frames larger than this (matches VideoConfig::kMaxEncodedFrameSize).
inline constexpr size_t kMaxVideoFrameSize = 512 * 1024;

// Cap packets per feedData() call to bound vector growth from malformed input.
inline constexpr size_t kMaxAudioPacketsPerFeed = 256;
inline constexpr size_t kMaxVideoPacketsPerFeed = 128;

// Highest container SamplingFrequency accepted as valid (192 kHz covers every
// real-world rate; WebM/Opus mandates 48 kHz). Values outside (0, this] are
// attacker-chosen doubles whose int cast would be UB — treated as invalid.
inline constexpr double kMaxAudioSampleRate = 192'000.0;

// Minimum interval between repeats of a per-feed warning. feedData() runs at
// stream bitrate, so an unratelimited warning on a backpressured path floods
// the log. Matches the ring's logMinIntervalMs precedent (5 s).
inline constexpr int64_t kThrottledLogIntervalUs = 5'000'000;

// Max tracks to iterate during parseTracks(). A valid broadcast WebM has 1–2
// (one Opus, one VP9). Defends against pathological inputs forcing a long
// linear scan in the track-classification loop.
inline constexpr unsigned long kMaxTracks = 32;

// Backward PTS tolerance before a block is treated as a discontinuity rather
// than packet reordering. Was config::epoch::kMaxBackwardJumpUs in v1; it is
// the only value the demuxer ever needed from the pipeline-wide config, so it
// lives here instead of dragging that header along.
inline constexpr int64_t kMaxBackwardJumpUs = 200'000;

}  // namespace media::demux
