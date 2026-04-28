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

// Cap packets/clusters per feedData() call to bound vector growth from malformed input.
inline constexpr size_t kMaxAudioPacketsPerFeed = 256;
inline constexpr size_t kMaxVideoPacketsPerFeed = 128;
inline constexpr size_t kMaxClustersPerFeed = 64;

// Maximum allowed stream header (EBML + Segment + Tracks, before first cluster).
// Normal WebM headers are 1-10 KB. Cap defends against malformed cluster positions.
inline constexpr size_t kMaxStreamHeaderSize = 64 * 1024;

// Max tracks to iterate during parseTracks(). A valid broadcast WebM has 1–2
// (one Opus, one VP9). Defends against pathological inputs forcing a long
// linear scan in the track-classification loop.
inline constexpr unsigned long kMaxTracks = 32;

}  // namespace media::demux
