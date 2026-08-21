// WebmDemuxer top-level orchestration: ctor/dtor, feedData() (state machine
// over EBML → Segment → Tracks → Streaming), parseEBMLHeader(), parseSegment(),
// reset(), and reader helpers.
//
// parseTracks() and parseBlocks() are split into sibling .cpp files for
// readability. The IMkvReader implementation (StreamReader) lives in
// WebmReaders.h. Internal limits live in DemuxLimits.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"
#include "WebmReaders.h"

#include "mkvparser/mkvparser.h"

#include "common/MediaLog.h"
#include "common/MediaTime.h"

namespace media::demux {

namespace {
// Ring-style shouldLog for the ingest thread (single-threaded, so a plain
// timestamp suffices where the ring needs a CAS).
bool throttledLog(int64_t& lastLogUs) {
    int64_t now = nowUs();
    if (now - lastLogUs < kThrottledLogIntervalUs) return false;
    lastLogUs = now;
    return true;
}
}  // namespace

WebmDemuxer::WebmDemuxer()
    : reader_(std::make_unique<StreamReader>()) {
    cachedResult_.audioPackets.reserve(kMaxAudioPacketsPerFeed);
    cachedResult_.videoPackets.reserve(kMaxVideoPacketsPerFeed);
}

WebmDemuxer::~WebmDemuxer() = default;

mkvparser::IMkvReader* WebmDemuxer::activeReader() const noexcept {
    return static_cast<mkvparser::IMkvReader*>(reader_.get());
}

const DemuxResult& WebmDemuxer::feedData(const uint8_t* data, size_t len) {
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();

    ++feedDataCalls_;

    // Compact data from the previous feedData call. Deferred here so that
    // packet pointers returned in the previous DemuxResult remain valid
    // until the caller invokes feedData again.
    if (pendingCompactPos_ > 0) {
        reader_->compact(pendingCompactPos_);
        pendingCompactPos_ = 0;
    }

    totalBytesFed_ += len;

    {
        if (!data || len == 0) {
            return cachedResult_;
        }
        size_t accepted = reader_->append(data, len);
        if (accepted < len) {
            // Drop-new, the same policy as the ring. There used to be a
            // parse-and-compact rescue here, but that parse emitted zero-copy
            // packets pointing into the window and the compact/append that
            // followed shifted or reallocated the very bytes those packets
            // referenced — the caller received dangling pointers in this
            // call's result. The window is trimmed by deferred compaction at
            // the top of the next call, so a full window recovers one feed
            // later at the cost of a counted gap.
            ++partialDropCount_;
            ++appendBackpressureDrops_;
            if (throttledLog(lastWindowFullLogUs_)) {
                MEDIA_LOG_W("WebmDemuxer: window full, dropped %zu/%zu bytes (count=%llu)",
                            len - accepted, len,
                            static_cast<unsigned long long>(partialDropCount_));
            }
        }

        if (accepted == 0) {
            cachedResult_.error = "demuxer buffer overflow";
            ++overflowCount_;
            return cachedResult_;
        }
    }

    // Advance parse state as far as possible
    switch (state_.load(std::memory_order_relaxed)) {
    case ParseState::WaitingForEBML:
        if (!parseEBMLHeader()) {
            if (!parseError_.empty()) {
                if (++parseRetryCount_ >= kMaxParseRetries) {
                    state_.store(ParseState::Error, std::memory_order_release);
                    errorEntryUs_ = nowUs();
                    cachedResult_.error = "permanent parse failure: " + parseError_;
                } else {
                    cachedResult_.error = parseError_;
                }
            }
            return cachedResult_;
        }
        parseRetryCount_ = 0;
        state_.store(ParseState::WaitingForSegment, std::memory_order_release);
        [[fallthrough]];

    case ParseState::WaitingForSegment:
        if (!parseSegment()) {
            if (!parseError_.empty()) {
                if (++parseRetryCount_ >= kMaxParseRetries) {
                    state_.store(ParseState::Error, std::memory_order_release);
                    errorEntryUs_ = nowUs();
                    cachedResult_.error = "permanent parse failure: " + parseError_;
                } else {
                    cachedResult_.error = parseError_;
                }
            }
            return cachedResult_;
        }
        parseRetryCount_ = 0;
        state_.store(ParseState::ParsingTracks, std::memory_order_release);
        [[fallthrough]];

    case ParseState::ParsingTracks:
        if (!parseTracks()) {
            if (state_.load(std::memory_order_relaxed) == ParseState::Error) {
                cachedResult_.error = "permanent parse failure: " + parseError_;
            }
            return cachedResult_;
        }
        state_.store(ParseState::Streaming, std::memory_order_release);
        [[fallthrough]];

    case ParseState::Streaming:
        parseBlocks(cachedResult_);
        break;

    case ParseState::Error: {
        // Auto-reset and re-feed the incoming data immediately. Throttled: a
        // stream that keeps failing (e.g. mid-cluster bytes after an
        // oversized-cluster error) loops error→reset→error once per few feeds.
        if (throttledLog(lastAutoResetLogUs_)) {
            MEDIA_LOG_W("WebmDemuxer: auto-resetting from error state on new feedData");
        }
        reset();
        // reset() emptied the window, so this only comes up short for a feed
        // larger than the whole window — still a counted drop, never silent.
        size_t reAccepted = reader_->append(data, len);
        if (reAccepted < len) {
            ++partialDropCount_;
            ++appendBackpressureDrops_;
        }
        if (parseEBMLHeader()) {
            state_.store(ParseState::WaitingForSegment, std::memory_order_release);
            if (parseSegment()) {
                state_.store(ParseState::ParsingTracks, std::memory_order_release);
                if (parseTracks()) {
                    state_.store(ParseState::Streaming, std::memory_order_release);
                    parseBlocks(cachedResult_);
                }
            }
        }
        // If the re-parse did not reach Streaming, surface that explicitly so the
        // JS-thread feedData caller can distinguish "no data parsed yet" from
        // "auto-reset is stalled awaiting a valid header".
        if (state_.load(std::memory_order_relaxed) != ParseState::Streaming) {
            cachedResult_.error = "auto-reset awaiting valid stream (state=" +
                                  std::to_string(static_cast<int>(
                                      state_.load(std::memory_order_relaxed))) + ")";
        }
        break;
    }
    }

    return cachedResult_;
}

size_t WebmDemuxer::readerBytesAvailable() const noexcept {
    return reader_ ? reader_->bufferSize() : 0;
}

long long WebmDemuxer::readerTotalAvailable() const noexcept {
    return reader_ ? reader_->available() : 0;
}

const uint8_t* WebmDemuxer::readerDataAt(long long absPos, long long len) const noexcept {
    return reader_ ? reader_->dataAt(absPos, len) : nullptr;
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
            ++parseErrorCount_;
            ++cumulativeParseErrorCount_;
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
            ++parseErrorCount_;
            ++cumulativeParseErrorCount_;
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
    reader_ = std::make_unique<StreamReader>();
    cluster_ = nullptr;
    blockEntry_ = nullptr;
    clusterDrained_ = false;
    {
        std::lock_guard<std::mutex> lk(trackMtx_);
        trackInfo_ = TrackInfo{};
    }
    state_.store(ParseState::WaitingForEBML, std::memory_order_release);
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
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
    // Monotonic across the session: cumulativeParseErrorCount_ and
    // sessionResetCount_ are NOT zeroed here.
    ++sessionResetCount_;
}

size_t WebmDemuxer::bufferBytes() const noexcept {
    return reader_ ? reader_->bufferSize() : 0;
}

}  // namespace media::demux
