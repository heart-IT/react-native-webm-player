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

#include <chrono>

namespace media::demux {

namespace {
// Single-threaded, so a plain timestamp suffices where the ring needs a CAS.
bool throttledLog(int64_t& lastLogUs) {
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (now - lastLogUs < kThrottledLogIntervalUs) return false;
    lastLogUs = now;
    return true;
}
}  // namespace

WebmDemuxer::WebmDemuxer()
    : reader_(std::make_unique<StreamReader>()) {}

WebmDemuxer::~WebmDemuxer() = default;

mkvparser::IMkvReader* WebmDemuxer::activeReader() const noexcept {
    return static_cast<mkvparser::IMkvReader*>(reader_.get());
}

const DemuxResult& WebmDemuxer::feedData(const uint8_t* data, size_t len) {
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();

    if (state_ == ParseState::Error) {
        cachedResult_.error = "permanent parse failure: " + parseError_;
        return cachedResult_;
    }

    // Compact data from the previous feedData call. Deferred here so that
    // packet pointers returned in the previous DemuxResult remain valid
    // until the caller invokes feedData again.
    if (pendingCompactPos_ > 0) {
        reader_->compact(pendingCompactPos_);
        pendingCompactPos_ = 0;
    }

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
            if (throttledLog(lastWindowFullLogUs_)) {
                MEDIA_LOG_W("WebmDemuxer: window full, dropped %zu/%zu bytes",
                            len - accepted, len);
            }
        }

        if (accepted == 0) {
            cachedResult_.error = "demuxer buffer overflow";
            return cachedResult_;
        }
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
        break;
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
    trackInfo_ = TrackInfo{};
    state_ = ParseState::WaitingForEBML;
    parseError_.clear();
    parseRetryCount_ = 0;
    ebmlHeaderEndPos_ = 0;
    compactOffset_ = 0;
    pendingCompactPos_ = 0;
    lastEmittedAudioPtsUs_ = -1;
    lastEmittedVideoPtsUs_ = -1;
    cachedResult_.audioPackets.clear();
    cachedResult_.videoPackets.clear();
    cachedResult_.error.clear();
}

size_t WebmDemuxer::bufferBytes() const noexcept {
    return reader_ ? reader_->bufferSize() : 0;
}

}  // namespace media::demux
