#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>

// Forward declarations for libwebm types
namespace mkvparser {
class IMkvReader;
struct EBMLHeader;
class Segment;
class Cluster;
class BlockEntry;
}  // namespace mkvparser


namespace media::demux {

// Opus audio packet extracted from WebM. Points into the demuxer's window:
// valid only until the next feedData() or reset() call.
struct AudioPacket {
    const uint8_t* data;
    size_t size;
    int64_t ptsUs;
};

// VP9 video packet extracted from WebM. Same lifetime rules as AudioPacket.
struct VideoPacket {
    const uint8_t* data;
    size_t size;
    int64_t ptsUs;
    bool isKeyFrame;
};

// Packets in DemuxResult point into an internal buffer owned by WebmDemuxer.
// They remain valid until the next call to feedData() or reset().
// Callers must consume or copy packet data before calling feedData() again.
struct DemuxResult {
    std::vector<AudioPacket> audioPackets;  // Opus packets extracted this call
    std::vector<VideoPacket> videoPackets;  // VP9 packets extracted this call
    std::string error;                      // Non-empty on parse error (logged, not fatal)
};

struct TrackInfo {
    int audioTrackNum = -1;
    int videoTrackNum = -1;
    int videoWidth = 0;
    int videoHeight = 0;
    // From the container's AudioTrack. WebM/Opus mandates 48 kHz, but the
    // decoder is configured from these rather than a hardcoded constant.
    int audioSampleRate = 0;
    int audioChannels = 0;
    // The video TrackEntry's DefaultDuration; 0 when the container omits it.
    int64_t videoFrameDurationNs = 0;
};

enum class ParseState {
    WaitingForEBML,
    WaitingForSegment,
    ParsingTracks,
    Streaming,
    Error
};

// Append-only reader for streaming WebM data into libwebm's mkvparser.
// Implements mkvparser::IMkvReader for incremental parsing.
class StreamReader;

// Single-threaded: every member runs on the demux thread that feeds it.
class WebmDemuxer {
public:
    WebmDemuxer();
    ~WebmDemuxer();

    WebmDemuxer(const WebmDemuxer&) = delete;
    WebmDemuxer& operator=(const WebmDemuxer&) = delete;

    // Feed raw WebM bytes incrementally. Returns extracted packets (if any).
    // Bytes are copied into the internal StreamReader window, which is trimmed
    // by deferred compaction on the following call. In the Error state the
    // bytes are discarded until reset(); the owner rejoins the stream.
    // Returned reference is valid until the next feedData() or reset() call.
    const DemuxResult& feedData(const uint8_t* data, size_t len);

    const TrackInfo& trackInfo() const noexcept { return trackInfo_; }

    ParseState parseState() const noexcept { return state_; }

    // Bytes resident in the parse window. Logged beside the ring occupancy:
    // a window that only grows is a compaction bug (iPhone 11, 2026-10-12).
    size_t bufferBytes() const noexcept;

    void reset();

private:
    bool parseEBMLHeader();
    bool parseSegment();
    bool parseTracks();
    void parseBlocks(DemuxResult& result);

    mkvparser::IMkvReader* activeReader() const noexcept;
    size_t readerBytesAvailable() const noexcept;
    long long readerTotalAvailable() const noexcept;
    const uint8_t* readerDataAt(long long absPos, long long len) const noexcept;

    DemuxResult cachedResult_;
    std::unique_ptr<StreamReader> reader_;
    std::unique_ptr<mkvparser::EBMLHeader> ebmlHeader_;
    std::unique_ptr<mkvparser::Segment> segment_;
    const mkvparser::Cluster* cluster_ = nullptr;
    const mkvparser::BlockEntry* blockEntry_ = nullptr;
    // True when cluster_'s blocks have been fully iterated but Segment::GetNext()
    // saw no further loaded clusters yet. parseBlocks() leaves cluster_ pointing
    // at the last real cluster (rather than overwriting it with the EOS sentinel)
    // and uses this flag to drive a Load()+GetNext() retry on the next call.
    bool clusterDrained_ = false;

    TrackInfo trackInfo_;
    ParseState state_ = ParseState::WaitingForEBML;
    std::string parseError_;
    long long ebmlHeaderEndPos_ = 0;
    int parseRetryCount_ = 0;

    // Track the lowest buffer offset still needed so we can compact
    long long compactOffset_ = 0;
    long long pendingCompactPos_ = 0;  // Deferred compaction target (applied at next feedData)

    // PTS dedup: skip the block parseBlocks() re-visits after a need-more-data
    // return from Cluster::GetNext().
    int64_t lastEmittedAudioPtsUs_ = -1;
    int64_t lastEmittedVideoPtsUs_ = -1;

    // The window-full warning fires on every feed while the stream is
    // backpressured, so it is throttled.
    int64_t lastWindowFullLogUs_ = 0;
};

}  // namespace media::demux
