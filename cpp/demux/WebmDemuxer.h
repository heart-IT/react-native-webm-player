#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>

// Forward declarations for libwebm types
namespace mkvparser {
class IMkvReader;
class EBMLHeader;
class Segment;
class Cluster;
class BlockEntry;
}  // namespace mkvparser

namespace media::demux {

struct AudioPacket {
    const uint8_t* data;
    size_t size;
    int64_t ptsUs;
    int64_t durationUs;
};

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
    std::vector<AudioPacket> audioPackets;
    std::vector<VideoPacket> videoPackets;
    std::string error;
    int newClusterCount = 0;
};

struct TrackInfo {
    int audioTrackNum = -1;
    int videoTrackNum = -1;
    int videoWidth = 0;
    int videoHeight = 0;
    std::string audioCodecId;
    std::string videoCodecId;
    std::vector<uint8_t> audioCodecPrivate;  // OpusHead
    std::vector<uint8_t> videoCodecPrivate;  // VP9 CodecPrivate
};

enum class ParseState {
    WaitingForEBML,
    WaitingForSegment,
    ParsingTracks,
    Streaming,
    Error
};

struct DemuxerMetrics {
    uint64_t totalBytesFed = 0;
    uint64_t feedDataCalls = 0;
    uint64_t audioPacketsEmitted = 0;
    uint64_t videoPacketsEmitted = 0;
    uint64_t overflowCount = 0;
    uint64_t blockStallCount = 0;  // consecutive block parse failures triggering stall error
    size_t bufferBytes = 0;        // current StreamReader buffer size
    ParseState parseState = ParseState::WaitingForEBML;
};

// Append-only reader for streaming WebM data into libwebm's mkvparser.
// Implements mkvparser::IMkvReader for incremental parsing.
class StreamReader;

class WebmDemuxer {
public:
    WebmDemuxer();
    ~WebmDemuxer();

    WebmDemuxer(const WebmDemuxer&) = delete;
    WebmDemuxer& operator=(const WebmDemuxer&) = delete;

    // Feed raw WebM bytes incrementally. Returns extracted packets (if any).
    // Returned reference is valid until the next feedData() or reset() call.
    const DemuxResult& feedData(const uint8_t* data, size_t len);

    const TrackInfo& trackInfo() const noexcept { return trackInfo_; }

    const std::vector<uint8_t>& streamHeader() const noexcept { return streamHeader_; }
    bool hasStreamHeader() const noexcept { return !streamHeader_.empty(); }

    ParseState parseState() const noexcept { return state_; }
    uint64_t overflowCount() const noexcept { return overflowCount_; }

    size_t bufferBytes() const noexcept;

    // Network-layer feed jitter in microseconds (EWMA of inter-feedData arrival deviation).
    // Updates on every feedData() call. Provides a leading indicator of network
    // instability before packet-level jitter estimators detect it.
    [[nodiscard]] int64_t feedJitterUs() const noexcept { return feedJitterUs_; }

    DemuxerMetrics demuxerMetrics() const noexcept {
        DemuxerMetrics m{};
        m.totalBytesFed = totalBytesFed_;
        m.feedDataCalls = feedDataCalls_;
        m.audioPacketsEmitted = audioPacketsEmitted_;
        m.videoPacketsEmitted = videoPacketsEmitted_;
        m.overflowCount = overflowCount_;
        m.blockStallCount = blockStallCount_;
        m.bufferBytes = bufferBytes();
        m.parseState = state_;
        return m;
    }

    void reset();

private:
    bool parseEBMLHeader();
    bool parseSegment();
    bool parseTracks();
    void parseBlocks(DemuxResult& result);

    DemuxResult cachedResult_;
    std::unique_ptr<StreamReader> reader_;
    std::unique_ptr<mkvparser::EBMLHeader> ebmlHeader_;
    std::unique_ptr<mkvparser::Segment> segment_;
    const mkvparser::Cluster* cluster_ = nullptr;
    const mkvparser::BlockEntry* blockEntry_ = nullptr;

    TrackInfo trackInfo_;
    std::vector<uint8_t> streamHeader_;
    ParseState state_ = ParseState::WaitingForEBML;
    std::string parseError_;
    long long ebmlHeaderEndPos_ = 0;
    int parseRetryCount_ = 0;

    // Track the lowest buffer offset still needed so we can compact
    long long compactOffset_ = 0;
    long long pendingCompactPos_ = 0;  // Deferred compaction target (applied at next feedData)
    uint64_t overflowCount_ = 0;

    // Metrics counters
    uint64_t totalBytesFed_ = 0;
    uint64_t feedDataCalls_ = 0;
    uint64_t audioPacketsEmitted_ = 0;
    uint64_t videoPacketsEmitted_ = 0;
    uint64_t blockStallCount_ = 0;

    // Feed arrival jitter tracking (network-layer signal)
    int64_t lastFeedTimeUs_ = 0;
    int64_t lastFeedIntervalUs_ = 0;
    int64_t feedJitterUs_ = 0;
};

}  // namespace media::demux
