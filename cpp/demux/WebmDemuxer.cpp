#include "WebmDemuxer.h"

#include "mkvparser/mkvparser.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include "common/MediaTime.h"

namespace media::demux {

// Streaming IMkvReader implementation backed by an append-only buffer.
// Supports compaction: bytes before a given offset can be discarded
// to prevent unbounded memory growth.
class StreamReader final : public mkvparser::IMkvReader {
public:
    static constexpr size_t kMaxBufferSize = 2 * 1024 * 1024;  // 2MB cap

    // Physical erase threshold — amortizes O(n) memmove to O(1) average
    static constexpr size_t kCompactThreshold = 512 * 1024;

    StreamReader() { buffer_.reserve(65536); }

    // Returns number of bytes accepted (may be < len if buffer nearly full)
    size_t append(const uint8_t* data, size_t len) {
        size_t logicalSize = buffer_.size() - logicalStart_;
        size_t avail = (logicalSize < kMaxBufferSize) ? kMaxBufferSize - logicalSize : 0;
        size_t accepted = std::min(len, avail);
        if (accepted == 0) return 0;
        buffer_.insert(buffer_.end(), data, data + accepted);
        return accepted;
    }

    // IMkvReader interface
    int Read(long long pos, long len, unsigned char* buf) override {
        long long localPos = pos - baseOffset_;
        if (localPos < static_cast<long long>(logicalStart_)) {
            return -1;  // Data already compacted away
        }
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (ulen > buffer_.size() || upos > buffer_.size() - ulen) {
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

    // Logically discard all bytes before the given absolute position.
    // Defers the physical erase until consumed bytes exceed kCompactThreshold,
    // amortizing the O(n) memmove cost to O(1) average per call.
    void compact(long long offset) {
        long long localPos = offset - baseOffset_;
        if (localPos <= static_cast<long long>(logicalStart_)) {
            return;
        }
        auto newStart = static_cast<size_t>(std::min(localPos,
            static_cast<long long>(buffer_.size())));
        logicalStart_ = newStart;

        if (logicalStart_ >= kCompactThreshold) {
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<ptrdiff_t>(logicalStart_));
            baseOffset_ += static_cast<long long>(logicalStart_);
            logicalStart_ = 0;
        }
    }

    long long available() const {
        return baseOffset_ + static_cast<long long>(buffer_.size());
    }

    long long baseOffset() const { return baseOffset_; }

    const uint8_t* bufferData() const { return buffer_.data() + logicalStart_; }
    size_t bufferSize() const { return buffer_.size() - logicalStart_; }

    // Get pointer to data at absolute position (for zero-copy packet access).
    // Validates that [absPos, absPos+len) is fully within the buffer.
    const uint8_t* dataAt(long long absPos, long long len = 1) const {
        long long localPos = absPos - baseOffset_;
        if (localPos < static_cast<long long>(logicalStart_) || len <= 0) {
            return nullptr;
        }
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (ulen > buffer_.size() || upos > buffer_.size() - ulen) {
            return nullptr;
        }
        return buffer_.data() + upos;
    }

private:
    std::vector<uint8_t> buffer_;
    long long baseOffset_ = 0;
    size_t logicalStart_ = 0;  // Bytes at front of buffer_ consumed but not yet erased
};

// Default audio frame duration for Opus: 20ms
static constexpr int64_t kDefaultAudioDurationUs = 20'000;

// Minimum bytes before declaring a parse failure (vs needing more data)
static constexpr size_t kMinBytesForEBML = 64;
static constexpr size_t kMinBytesForSegment = 256;

// Max retries for header parsing before entering permanent error state
static constexpr int kMaxParseRetries = 3;

// Consecutive unparseable blocks before reporting a stall
static constexpr int kMaxConsecutiveBlockErrors = 3;

// Reject video frames larger than this (matches VideoConfig::kMaxEncodedFrameSize)
static constexpr size_t kMaxVideoFrameSize = 512 * 1024;

// Maximum allowed stream header (EBML + Segment + Tracks, before first cluster).
// Normal WebM headers are 1-10KB. Cap defends against malformed cluster positions.
static constexpr size_t kMaxStreamHeaderSize = 64 * 1024;

WebmDemuxer::WebmDemuxer()
    : reader_(std::make_unique<StreamReader>()) {
    cachedResult_.audioPackets.reserve(10);
    cachedResult_.videoPackets.reserve(4);
}

WebmDemuxer::~WebmDemuxer() = default;

const DemuxResult& WebmDemuxer::feedData(const uint8_t* data, size_t len) {
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
    cachedResult_.newClusterCount = 0;

    ++feedDataCalls_;

    // Compact data from the previous feedData call. Deferred here so that
    // packet pointers returned in the previous DemuxResult remain valid
    // until the caller invokes feedData again (contract in WebmDemuxer.h:34-36).
    if (pendingCompactPos_ > 0) {
        reader_->compact(pendingCompactPos_);
        pendingCompactPos_ = 0;
    }

    if (!data || len == 0) {
        return cachedResult_;
    }

    // Track inter-feedData arrival jitter (network-layer signal).
    // Uses the same EWMA approach as JitterEstimator but at the byte-feed level.
    // This detects network instability before packet-level jitter manifests.
    {
        int64_t now = nowUs();
        if (lastFeedTimeUs_ > 0) {
            int64_t interval = now - lastFeedTimeUs_;
            if (lastFeedIntervalUs_ > 0 && interval > 0) {
                int64_t deviation = interval - lastFeedIntervalUs_;
                int64_t absDeviation = deviation < 0 ? -deviation : deviation;
                // EWMA alpha = 0.125 (matches JitterEstimator)
                feedJitterUs_ += (absDeviation - feedJitterUs_) / 8;
            }
            lastFeedIntervalUs_ = interval;
        }
        lastFeedTimeUs_ = now;
    }

    totalBytesFed_ += len;
    size_t totalAccepted = 0;
    size_t accepted = reader_->append(data, len);
    totalAccepted += accepted;

    if (totalAccepted < len && state_ == ParseState::Streaming) {
        // Buffer full or partially full — parse existing data to advance
        // compaction, then retry remaining bytes.
        parseBlocks(cachedResult_);
        if (pendingCompactPos_ > 0) {
            reader_->compact(pendingCompactPos_);
            pendingCompactPos_ = 0;
        }
        accepted = reader_->append(data + totalAccepted, len - totalAccepted);
        totalAccepted += accepted;
    }

    if (totalAccepted == 0) {
        cachedResult_.error = "demuxer buffer overflow";
        ++overflowCount_;
        return cachedResult_;
    }

    // Advance parse state as far as possible
    switch (state_) {
    case ParseState::WaitingForEBML:
        if (!parseEBMLHeader()) {
            if (!parseError_.empty()) {
                if (++parseRetryCount_ >= kMaxParseRetries) {
                    state_ = ParseState::Error;
                    cachedResult_.error = "permanent parse failure: " + parseError_;
                } else {
                    cachedResult_.error = parseError_;
                }
            }
            return cachedResult_;
        }
        parseRetryCount_ = 0;
        state_ = ParseState::WaitingForSegment;
        [[fallthrough]];

    case ParseState::WaitingForSegment:
        if (!parseSegment()) {
            if (!parseError_.empty()) {
                if (++parseRetryCount_ >= kMaxParseRetries) {
                    state_ = ParseState::Error;
                    cachedResult_.error = "permanent parse failure: " + parseError_;
                } else {
                    cachedResult_.error = parseError_;
                }
            }
            return cachedResult_;
        }
        parseRetryCount_ = 0;
        state_ = ParseState::ParsingTracks;
        [[fallthrough]];

    case ParseState::ParsingTracks:
        if (!parseTracks()) {
            if (state_ == ParseState::Error) {
                cachedResult_.error = "permanent parse failure: " + parseError_;
            }
            return cachedResult_;
        }
        state_ = ParseState::Streaming;
        [[fallthrough]];

    case ParseState::Streaming:
        parseBlocks(cachedResult_);
        break;

    case ParseState::Error:
        cachedResult_.error = "demuxer in error state, call reset() to recover";
        return cachedResult_;
    }

    return cachedResult_;
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
    ebmlHeaderEndPos_ = pos;
    return true;
}

bool WebmDemuxer::parseSegment() {
    long long pos = ebmlHeaderEndPos_;

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
    // For streaming WebM (unknown segment size), Load() may return
    // E_BUFFER_NOT_FULL even after tracks and clusters are parsed.
    // Check if tracks are available regardless of return value.
    long ret = segment_->Load();

    const mkvparser::Tracks* tracks = segment_->GetTracks();
    if (!tracks || tracks->GetTracksCount() == 0) {
        if (ret < 0) {
            if (ret == mkvparser::E_BUFFER_NOT_FULL) {
                return false;  // Need more data
            }
            parseError_ = "segment load failed";
            if (++parseRetryCount_ >= kMaxParseRetries) {
                state_ = ParseState::Error;
            }
        }
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

    if (trackInfo_.audioTrackNum < 0 && trackInfo_.videoTrackNum < 0) {
        parseError_ = "no supported tracks (need A_OPUS or V_VP9)";
        state_ = ParseState::Error;
        return false;
    }

    // Initialize cluster iteration
    cluster_ = segment_->GetFirst();
    blockEntry_ = nullptr;

    // Capture stream header bytes (EBML + Segment + Tracks) for clip extraction.
    // Everything from byte 0 to the first cluster position forms a valid WebM
    // header that can be prepended to cluster data for standalone playback.
    if (cluster_ && !cluster_->EOS()) {
        long long headerEnd = cluster_->GetPosition();
        if (headerEnd > 0 && static_cast<size_t>(headerEnd) <= kMaxStreamHeaderSize
            && streamHeader_.empty()) {
            streamHeader_.resize(static_cast<size_t>(headerEnd));
            int readResult = reader_->Read(0, static_cast<long>(headerEnd),
                                           streamHeader_.data());
            if (readResult != 0) {
                streamHeader_.clear();
            }
        }
    }

    return true;
}

void WebmDemuxer::parseBlocks(DemuxResult& result) {
    if (!segment_) {
        return;
    }

    long long latestConsumedPos = compactOffset_;
    int consecutiveBlockErrors = 0;

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
                ++consecutiveBlockErrors;
                // Advance to next block entry
                long status = cluster_->GetNext(blockEntry_, blockEntry_);
                if (status < 0) {
                    break;  // Need more data
                }
                continue;
            }
            consecutiveBlockErrors = 0;

            long long trackNum = block->GetTrackNumber();
            int frameCount = block->GetFrameCount();

            // Timestamp: cluster time + block time offset, in nanoseconds
            long long timeNs = block->GetTime(cluster_);
            int64_t ptsUs = timeNs / 1000;

            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                // Verify the data is readable from our buffer
                const uint8_t* frameData = reader_->dataAt(frame.pos, frame.len);
                if (!frameData) {
                    // Frame data beyond readable range — likely compacted or
                    // gap in stream. Skip this block to prevent permanent stall.
                    ++consecutiveBlockErrors;
                    break;
                }

                if (trackNum == trackInfo_.audioTrackNum) {
                    AudioPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    pkt.durationUs = kDefaultAudioDurationUs;
                    result.audioPackets.push_back(pkt);
                    ++audioPacketsEmitted_;
                } else if (trackNum == trackInfo_.videoTrackNum) {
                    auto frameSize = static_cast<size_t>(frame.len);
                    if (frameSize > kMaxVideoFrameSize) {
                        continue;  // Reject oversized frames
                    }
                    VideoPacket pkt;
                    pkt.data = frameData;
                    pkt.size = frameSize;
                    pkt.ptsUs = ptsUs;
                    pkt.isKeyFrame = block->IsKey();
                    result.videoPackets.push_back(pkt);
                    ++videoPacketsEmitted_;
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
        ++result.newClusterCount;
    }

    // Report if blocks are consistently unparseable despite data being available
    if (consecutiveBlockErrors >= kMaxConsecutiveBlockErrors && result.audioPackets.empty() && result.videoPackets.empty()) {
        result.error = "block parse stall: consecutive unparseable blocks";
        ++blockStallCount_;
    }

    // Schedule compaction for the start of the next feedData() call.
    // Deferring compaction ensures packet data pointers in the returned
    // DemuxResult remain valid until the caller processes them.
    if (latestConsumedPos > compactOffset_) {
        compactOffset_ = latestConsumedPos;
        if (cluster_ && !cluster_->EOS()) {
            long long safePos = cluster_->GetPosition();
            if (safePos > 0) {
                pendingCompactPos_ = safePos;
            }
        } else {
            pendingCompactPos_ = latestConsumedPos;
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
    streamHeader_.clear();
    state_ = ParseState::WaitingForEBML;
    parseError_.clear();
    parseRetryCount_ = 0;
    ebmlHeaderEndPos_ = 0;
    compactOffset_ = 0;
    pendingCompactPos_ = 0;
    overflowCount_ = 0;
    totalBytesFed_ = 0;
    feedDataCalls_ = 0;
    audioPacketsEmitted_ = 0;
    videoPacketsEmitted_ = 0;
    blockStallCount_ = 0;
    lastFeedTimeUs_ = 0;
    lastFeedIntervalUs_ = 0;
    feedJitterUs_ = 0;
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
    cachedResult_.newClusterCount = 0;
}

size_t WebmDemuxer::bufferBytes() const noexcept {
    return reader_ ? reader_->bufferSize() : 0;
}

}  // namespace media::demux
