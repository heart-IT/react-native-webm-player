// WebmDemuxer top-level orchestration: ctor/dtor, ring-buffer attach,
// feedData() (state machine over EBML → Segment → Tracks → Streaming),
// parseEBMLHeader(), parseSegment(), reset(), and reader helpers.
//
// parseTracks() and parseBlocks() are split into sibling .cpp files for
// readability. The IMkvReader implementations (StreamReader / RingReader)
// live in WebmReaders.h. Internal limits live in DemuxLimits.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"
#include "WebmReaders.h"
#include "common/IngestRingBuffer.h"

#include "mkvparser/mkvparser.h"

#include "common/MediaLog.h"
#include "common/MediaTime.h"

namespace media::demux {

WebmDemuxer::WebmDemuxer()
    : reader_(std::make_unique<StreamReader>()) {
    cachedResult_.audioPackets.reserve(kMaxAudioPacketsPerFeed);
    cachedResult_.videoPackets.reserve(kMaxVideoPacketsPerFeed);
    cachedResult_.newClusterPositions.reserve(kMaxClustersPerFeed);
    // Pre-reserve a typical worst case so outer vector growth is rare. The
    // reservation is a hint, not a safety contract: even if scratchCursor_
    // exceeds the reservation and the outer vector reallocates, std::vector's
    // move ctor for the inner std::vector<uint8_t> elements transfers heap-
    // pointer ownership without copying, so frameData = slot.data() taken
    // before the reallocation remains valid for the rest of the feedData() call.
    scratchBuffers_.reserve(kMaxAudioPacketsPerFeed + kMaxVideoPacketsPerFeed);
}

WebmDemuxer::~WebmDemuxer() = default;

void WebmDemuxer::setRingBuffer(IngestRingBuffer* ring) noexcept {
    ringBuffer_ = ring;
    if (ring) {
        ringReader_ = std::make_unique<RingReader>(*ring);
    } else {
        ringReader_.reset();
    }
}

mkvparser::IMkvReader* WebmDemuxer::activeReader() const noexcept {
    return ringReader_ ? static_cast<mkvparser::IMkvReader*>(ringReader_.get())
                       : static_cast<mkvparser::IMkvReader*>(reader_.get());
}

const DemuxResult& WebmDemuxer::feedData(const uint8_t* data, size_t len) {
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
    cachedResult_.newClusterCount = 0;
    cachedResult_.newClusterPositions.clear();

    feedDataCalls_.fetch_add(1, std::memory_order_relaxed);

    bool ringMode = ringReader_ != nullptr;

    // Compact data from the previous feedData call. Deferred here so that
    // packet pointers returned in the previous DemuxResult remain valid
    // until the caller invokes feedData again.
    if (pendingCompactPos_ > 0) {
        if (ringMode) {
            ringReader_->compact(pendingCompactPos_);
        } else {
            reader_->compact(pendingCompactPos_);
        }
        pendingCompactPos_ = 0;
    }

    // Track inter-feedData arrival jitter in both ring and stream mode.
    {
        int64_t now = nowUs();
        if (lastFeedTimeUs_ > 0) {
            int64_t interval = now - lastFeedTimeUs_;
            if (lastFeedIntervalUs_ > 0 && interval > 0) {
                int64_t deviation = interval - lastFeedIntervalUs_;
                int64_t absDeviation = deviation < 0 ? -deviation : deviation;
                int64_t prev = feedJitterUs_.load(std::memory_order_relaxed);
                feedJitterUs_.store(prev + (absDeviation - prev) / 8, std::memory_order_relaxed);
            }
            lastFeedIntervalUs_ = interval;
        }
        lastFeedTimeUs_ = now;
    }

    totalBytesFed_.fetch_add(len, std::memory_order_relaxed);

    // Ring mode: bytes are already in the ring buffer from JS write().
    // Stream mode: append bytes to internal StreamReader.
    if (ringMode) {
        // Data already in ring, skip append
    } else {
        if (!data || len == 0) {
            return cachedResult_;
        }
        size_t totalAccepted = 0;
        size_t accepted = reader_->append(data, len);
        totalAccepted += accepted;

        if (totalAccepted < len) {
            if (state_ == ParseState::Streaming) {
                parseBlocks(cachedResult_);
            }
            if (pendingCompactPos_ > 0) {
                reader_->compact(pendingCompactPos_);
                pendingCompactPos_ = 0;
            }
            accepted = reader_->append(data + totalAccepted, len - totalAccepted);
            totalAccepted += accepted;

            if (totalAccepted < len) {
                partialDropCount_.fetch_add(1, std::memory_order_relaxed);
                appendBackpressureDrops_.fetch_add(1, std::memory_order_relaxed);
                MEDIA_LOG_W("WebmDemuxer: partial drop %zu/%zu bytes (count=%llu)",
                            len - totalAccepted, len,
                            static_cast<unsigned long long>(partialDropCount_.load(std::memory_order_relaxed)));
            }
        }

        if (totalAccepted == 0) {
            cachedResult_.error = "demuxer buffer overflow";
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            return cachedResult_;
        }
    }

    // Advance parse state as far as possible
    int64_t parseStart = nowUs();
    switch (state_) {
    case ParseState::WaitingForEBML:
        if (!parseEBMLHeader()) {
            if (!parseError_.empty()) {
                if (++parseRetryCount_ >= kMaxParseRetries) {
                    state_ = ParseState::Error;
                    errorEntryUs_.store(nowUs(), std::memory_order_relaxed);
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
                    errorEntryUs_.store(nowUs(), std::memory_order_relaxed);
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
        // Record first cluster position for ClipIndex
        if (cluster_ && !cluster_->EOS()) {
            cachedResult_.newClusterPositions.push_back(cluster_->GetPosition());
            ++cachedResult_.newClusterCount;
        }
        [[fallthrough]];

    case ParseState::Streaming:
        parseBlocks(cachedResult_);
        break;

    case ParseState::Error:
        // In ring mode the EBML header has been compacted away — auto-reset cannot
        // re-parse from the ring.  Surface the error so the ingest thread can
        // trigger automatic recovery using the cached stream header.
        if (ringReader_) {
            cachedResult_.error = "demuxer in error state (ring mode)";
            break;
        }
        // Stream mode: auto-reset and re-feed the incoming data immediately.
        MEDIA_LOG_W("WebmDemuxer: auto-resetting from error state on new feedData");
        reset();
        reader_->append(data, len);
        if (parseEBMLHeader()) {
            state_ = ParseState::WaitingForSegment;
            if (parseSegment()) {
                state_ = ParseState::ParsingTracks;
                if (parseTracks()) {
                    state_ = ParseState::Streaming;
                    parseBlocks(cachedResult_);
                }
            }
        }
        // If the re-parse did not reach Streaming, surface that explicitly so the
        // JS-thread feedData caller can distinguish "no data parsed yet" from
        // "auto-reset is stalled awaiting a valid header".
        if (state_ != ParseState::Streaming) {
            cachedResult_.error = "auto-reset awaiting valid stream (state=" +
                                  std::to_string(static_cast<int>(state_.load())) + ")";
        }
        break;
    }

    {
        int64_t elapsed = nowUs() - parseStart;
        int64_t prevLatency = feedDataLatencyUs_.load(std::memory_order_relaxed);
        feedDataLatencyUs_.store(prevLatency + (elapsed - prevLatency) / 8, std::memory_order_relaxed);
    }

    cachedBufferBytes_.store(ringReader_ ? ringBuffer_->liveBytes()
                                          : (reader_ ? reader_->bufferSize() : 0),
                             std::memory_order_relaxed);

    return cachedResult_;
}

// Helper: get available bytes/data from whichever reader is active.
size_t WebmDemuxer::readerBytesAvailable() const noexcept {
    return ringReader_ ? ringBuffer_->liveBytes() : (reader_ ? reader_->bufferSize() : 0);
}

long long WebmDemuxer::readerTotalAvailable() const noexcept {
    return ringReader_ ? ringReader_->available() : (reader_ ? reader_->available() : 0);
}

const uint8_t* WebmDemuxer::readerDataAt(long long absPos, long long len) const noexcept {
    return ringReader_ ? ringReader_->dataAt(absPos, len) : (reader_ ? reader_->dataAt(absPos, len) : nullptr);
}

bool WebmDemuxer::parseEBMLHeader() {
    ebmlHeader_ = std::make_unique<mkvparser::EBMLHeader>();
    long long pos = 0;
    long long ret = ebmlHeader_->Parse(activeReader(), pos);
    if (ret < 0) {
        ebmlHeader_.reset();
        // Distinguish "need more data" from "bad stream"
        if (readerBytesAvailable() >= kMinBytesForEBML) {
            parseError_ = "invalid EBML header";
            parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
            cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
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
    long long ret = mkvparser::Segment::CreateInstance(activeReader(), pos, rawSegment);
    if (ret != 0 || !rawSegment) {
        if (readerBytesAvailable() >= kMinBytesForSegment) {
            parseError_ = "invalid WebM segment";
            parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
            cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    parseError_.clear();
    segment_.reset(rawSegment);
    return true;
}

void WebmDemuxer::reset() {
    segment_.reset();
    ebmlHeader_.reset();
    if (!ringReader_) {
        reader_ = std::make_unique<StreamReader>();
    }
    cluster_ = nullptr;
    blockEntry_ = nullptr;
    {
        std::lock_guard<std::mutex> lk(trackMtx_);
        trackInfo_ = TrackInfo{};
        streamHeader_.clear();
    }
    state_ = ParseState::WaitingForEBML;
    parseError_.clear();
    parseRetryCount_ = 0;
    ebmlHeaderEndPos_ = 0;
    compactOffset_ = 0;
    pendingCompactPos_ = 0;
    overflowCount_ = 0;
    partialDropCount_ = 0;
    oversizedFrameDrops_ = 0;
    packetCapDrops_ = 0;
    appendBackpressureDrops_ = 0;
    errorEntryUs_ = 0;
    totalBytesFed_ = 0;
    feedDataCalls_ = 0;
    audioPacketsEmitted_ = 0;
    videoPacketsEmitted_ = 0;
    blockStallCount_ = 0;
    parseErrorCount_ = 0;
    lastEmittedAudioPtsUs_ = -1;
    lastEmittedVideoPtsUs_ = -1;
    lastFeedTimeUs_ = 0;
    lastFeedIntervalUs_ = 0;
    feedJitterUs_ = 0;
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
    cachedResult_.newClusterCount = 0;
    cachedResult_.newClusterPositions.clear();
    scratchBuffers_.clear();
    // Preserve reserved capacity — constructor reserved kMaxAudioPacketsPerFeed +
    // kMaxVideoPacketsPerFeed; shrink_to_fit would let the next feedData reallocate
    // mid-parse and invalidate the slot reference held in the block loop.
    scratchCursor_ = 0;
    // Monotonic across the session: cumulativeParseErrorCount_ and
    // sessionResetCount_ are NOT zeroed here.
    sessionResetCount_.fetch_add(1, std::memory_order_relaxed);
}

size_t WebmDemuxer::bufferBytes() const noexcept {
    return cachedBufferBytes_.load(std::memory_order_relaxed);
}

}  // namespace media::demux
