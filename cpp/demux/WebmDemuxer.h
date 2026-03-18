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
    Streaming
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
    // Returned packet data pointers are valid until the next feedData() call.
    DemuxResult feedData(const uint8_t* data, size_t len);

    const TrackInfo& trackInfo() const noexcept { return trackInfo_; }

    ParseState parseState() const noexcept { return state_; }

    void reset();

private:
    bool parseEBMLHeader();
    bool parseSegment();
    bool parseTracks();
    void parseBlocks(DemuxResult& result);

    std::unique_ptr<StreamReader> reader_;
    std::unique_ptr<mkvparser::EBMLHeader> ebmlHeader_;
    std::unique_ptr<mkvparser::Segment> segment_;
    const mkvparser::Cluster* cluster_ = nullptr;
    const mkvparser::BlockEntry* blockEntry_ = nullptr;

    TrackInfo trackInfo_;
    ParseState state_ = ParseState::WaitingForEBML;

    // Track the lowest buffer offset still needed so we can compact
    long long compactOffset_ = 0;
};

}  // namespace media::demux
