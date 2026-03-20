#include "WebmDemuxer.h"

#include "mkvparser/mkvparser.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace media::demux {

// Streaming IMkvReader implementation backed by an append-only buffer.
// Supports compaction: bytes before a given offset can be discarded
// to prevent unbounded memory growth.
class StreamReader final : public mkvparser::IMkvReader {
public:
    static constexpr size_t kMaxBufferSize = 2 * 1024 * 1024;  // 2MB cap

    StreamReader() = default;

    // Returns false if buffer would exceed max size
    bool append(const uint8_t* data, size_t len) {
        if (buffer_.size() + len > kMaxBufferSize) {
            return false;
        }
        buffer_.insert(buffer_.end(), data, data + len);
        return true;
    }

    // IMkvReader interface
    int Read(long long pos, long len, unsigned char* buf) override {
        long long localPos = pos - baseOffset_;
        if (localPos < 0) {
            return -1;  // Data already compacted away
        }
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (upos + ulen > buffer_.size()) {
            return -1;  // Not enough data available yet
        }
        std::memcpy(buf, buffer_.data() + upos, ulen);
        return 0;
    }

    int Length(long long* total, long long* available) override {
        // Streaming: total is unknown
        if (total) {
            *total = -1;
        }
        if (available) {
            *available = baseOffset_ + static_cast<long long>(buffer_.size());
        }
        return 0;
    }

    // Discard all bytes before the given absolute position.
    // After this call, Read() for positions < offset will fail.
    void compact(long long offset) {
        long long localPos = offset - baseOffset_;
        if (localPos <= 0) {
            return;
        }
        auto discard = static_cast<size_t>(std::min(localPos,
            static_cast<long long>(buffer_.size())));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(discard));
        baseOffset_ += static_cast<long long>(discard);
    }

    long long available() const {
        return baseOffset_ + static_cast<long long>(buffer_.size());
    }

    long long baseOffset() const { return baseOffset_; }

    const uint8_t* bufferData() const { return buffer_.data(); }
    size_t bufferSize() const { return buffer_.size(); }

    // Get pointer to data at absolute position (for zero-copy packet access)
    const uint8_t* dataAt(long long absPos) const {
        long long localPos = absPos - baseOffset_;
        if (localPos < 0 || static_cast<size_t>(localPos) >= buffer_.size()) {
            return nullptr;
        }
        return buffer_.data() + localPos;
    }

private:
    std::vector<uint8_t> buffer_;
    long long baseOffset_ = 0;
};

// Default audio frame duration for Opus: 20ms
static constexpr int64_t kDefaultAudioDurationUs = 20'000;

// Minimum bytes before declaring a parse failure (vs needing more data)
static constexpr size_t kMinBytesForEBML = 64;
static constexpr size_t kMinBytesForSegment = 256;

WebmDemuxer::WebmDemuxer()
    : reader_(std::make_unique<StreamReader>()) {}

WebmDemuxer::~WebmDemuxer() = default;

DemuxResult WebmDemuxer::feedData(const uint8_t* data, size_t len) {
    DemuxResult result;

    if (!data || len == 0) {
        return result;
    }

    if (!reader_->append(data, len)) {
        result.error = "demuxer buffer overflow";
        return result;
    }

    // Advance parse state as far as possible
    switch (state_) {
    case ParseState::WaitingForEBML:
        if (!parseEBMLHeader()) {
            if (!parseError_.empty()) result.error = parseError_;
            return result;
        }
        state_ = ParseState::WaitingForSegment;
        [[fallthrough]];

    case ParseState::WaitingForSegment:
        if (!parseSegment()) {
            if (!parseError_.empty()) result.error = parseError_;
            return result;
        }
        state_ = ParseState::ParsingTracks;
        [[fallthrough]];

    case ParseState::ParsingTracks:
        if (!parseTracks()) {
            return result;  // Need more data
        }
        state_ = ParseState::Streaming;
        [[fallthrough]];

    case ParseState::Streaming:
        parseBlocks(result);
        break;
    }

    return result;
}

bool WebmDemuxer::parseEBMLHeader() {
    ebmlHeader_ = std::make_unique<mkvparser::EBMLHeader>();
    long long pos = 0;
    long long ret = ebmlHeader_->Parse(reader_.get(), pos);
    if (ret < 0) {
        ebmlHeader_.reset();
        // Distinguish "need more data" from "bad stream"
        if (reader_->bufferSize() >= kMinBytesForEBML) {
            parseError_ = "invalid EBML header";
        }
        return false;
    }
    parseError_.clear();
    return true;
}

bool WebmDemuxer::parseSegment() {
    long long pos = 0;

    // Find the start of the Segment element
    // EBMLHeader::Parse leaves us at end of header; Segment starts after
    // Re-parse the header to get the position after it
    {
        mkvparser::EBMLHeader tmpHeader;
        long long ret = tmpHeader.Parse(reader_.get(), pos);
        if (ret < 0) {
            return false;
        }
    }

    mkvparser::Segment* rawSegment = nullptr;
    long long ret = mkvparser::Segment::CreateInstance(reader_.get(), pos, rawSegment);
    if (ret != 0 || !rawSegment) {
        if (reader_->bufferSize() >= kMinBytesForSegment) {
            parseError_ = "invalid WebM segment";
        }
        return false;
    }
    parseError_.clear();
    segment_.reset(rawSegment);
    return true;
}

bool WebmDemuxer::parseTracks() {
    if (!segment_) {
        return false;
    }

    // Load headers (tracks, seek head, info, etc.)
    // Returns 0 on success, negative on error/need more data
    long ret = segment_->Load();
    // E_BUFFER_NOT_FULL == mkvparser::E_BUFFER_NOT_FULL == -2
    if (ret < 0) {
        return false;  // Need more data or error
    }

    const mkvparser::Tracks* tracks = segment_->GetTracks();
    if (!tracks) {
        return false;
    }

    unsigned long trackCount = tracks->GetTracksCount();
    for (unsigned long i = 0; i < trackCount; ++i) {
        const mkvparser::Track* track = tracks->GetTrackByIndex(i);
        if (!track) {
            continue;
        }

        long long trackType = track->GetType();
        const char* codecId = track->GetCodecId();
        if (!codecId) {
            continue;
        }

        size_t codecPrivateSize = 0;
        const unsigned char* codecPrivate = track->GetCodecPrivate(codecPrivateSize);

        if (trackType == mkvparser::Track::kAudio &&
            std::string(codecId) == "A_OPUS") {
            trackInfo_.audioTrackNum = static_cast<int>(track->GetNumber());
            trackInfo_.audioCodecId = codecId;
            if (codecPrivate && codecPrivateSize > 0) {
                trackInfo_.audioCodecPrivate.assign(
                    codecPrivate, codecPrivate + codecPrivateSize);
            }
        } else if (trackType == mkvparser::Track::kVideo &&
                   std::string(codecId) == "V_VP9") {
            trackInfo_.videoTrackNum = static_cast<int>(track->GetNumber());
            trackInfo_.videoCodecId = codecId;
            if (codecPrivate && codecPrivateSize > 0) {
                trackInfo_.videoCodecPrivate.assign(
                    codecPrivate, codecPrivate + codecPrivateSize);
            }
            const mkvparser::VideoTrack* videoTrack =
                static_cast<const mkvparser::VideoTrack*>(track);
            trackInfo_.videoWidth = static_cast<int>(videoTrack->GetWidth());
            trackInfo_.videoHeight = static_cast<int>(videoTrack->GetHeight());
        }
    }

    // Initialize cluster iteration
    cluster_ = segment_->GetFirst();
    blockEntry_ = nullptr;
    return true;
}

void WebmDemuxer::parseBlocks(DemuxResult& result) {
    if (!segment_) {
        return;
    }

    long long latestConsumedPos = compactOffset_;

    while (cluster_ && !cluster_->EOS()) {
        // If we haven't started iterating this cluster's blocks, get the first entry
        if (!blockEntry_) {
            long status = cluster_->GetFirst(blockEntry_);
            if (status < 0) {
                // Need more data for this cluster
                break;
            }
        }

        while (blockEntry_ && !blockEntry_->EOS()) {
            const mkvparser::Block* block = blockEntry_->GetBlock();
            if (!block) {
                // Advance to next block entry
                long status = cluster_->GetNext(blockEntry_, blockEntry_);
                if (status < 0) {
                    break;  // Need more data
                }
                continue;
            }

            long long trackNum = block->GetTrackNumber();
            int frameCount = block->GetFrameCount();

            // Timestamp: cluster time + block time offset, in nanoseconds
            long long timeNs = block->GetTime(cluster_);
            int64_t ptsUs = timeNs / 1000;

            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                // Verify the data is readable from our buffer
                const uint8_t* frameData = reader_->dataAt(frame.pos);
                if (!frameData) {
                    // Frame data not yet available or compacted
                    return;
                }

                if (trackNum == trackInfo_.audioTrackNum) {
                    AudioPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    pkt.durationUs = kDefaultAudioDurationUs;
                    result.audioPackets.push_back(pkt);
                } else if (trackNum == trackInfo_.videoTrackNum) {
                    VideoPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    pkt.isKeyFrame = block->IsKey();
                    result.videoPackets.push_back(pkt);
                }

                // Track the end of this frame for compaction
                long long frameEnd = frame.pos + frame.len;
                if (frameEnd > latestConsumedPos) {
                    latestConsumedPos = frameEnd;
                }
            }

            // Advance to next block entry
            const mkvparser::BlockEntry* nextEntry = nullptr;
            long status = cluster_->GetNext(blockEntry_, nextEntry);
            if (status < 0) {
                // Need more data for next block
                return;
            }
            blockEntry_ = nextEntry;
        }

        // Move to the next cluster
        const mkvparser::Cluster* nextCluster = segment_->GetNext(cluster_);
        if (!nextCluster || nextCluster->EOS()) {
            cluster_ = nextCluster;
            blockEntry_ = nullptr;
            break;
        }

        // Check if the next cluster's data is available
        long long clusterPos = nextCluster->GetPosition();
        long long clusterSize = nextCluster->GetElementSize();
        // If element size is unknown yet, just try — GetFirst will fail if data is missing
        if (clusterSize > 0 && reader_->available() < clusterPos + clusterSize) {
            // Not enough data for this cluster yet; stay put
            cluster_ = nextCluster;
            blockEntry_ = nullptr;
            break;
        }

        cluster_ = nextCluster;
        blockEntry_ = nullptr;
    }

    // Compact consumed data from the buffer
    if (latestConsumedPos > compactOffset_) {
        compactOffset_ = latestConsumedPos;
        // Don't compact too aggressively — only compact data well before current parse pos.
        // The segment and cluster structures may reference earlier positions for headers,
        // so we compact conservatively up to the start of the current cluster.
        if (cluster_ && !cluster_->EOS()) {
            long long safePos = cluster_->GetPosition();
            if (safePos > 0) {
                reader_->compact(safePos);
            }
        }
    }
}

void WebmDemuxer::reset() {
    segment_.reset();
    ebmlHeader_.reset();
    reader_ = std::make_unique<StreamReader>();
    cluster_ = nullptr;
    blockEntry_ = nullptr;
    trackInfo_ = TrackInfo{};
    state_ = ParseState::WaitingForEBML;
    parseError_.clear();
    compactOffset_ = 0;
}

}  // namespace media::demux
