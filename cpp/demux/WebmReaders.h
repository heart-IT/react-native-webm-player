// IMkvReader implementations used by WebmDemuxer.
//
//   StreamReader — append-only vector backing for stream/sync mode.
//                  Used as a fallback when no IngestRingBuffer is attached.
//   RingReader   — zero-copy adapter over IngestRingBuffer for ring/ingest mode.
//
// Internal to cpp/demux/. The WebmDemuxer header forward-declares these and
// holds them via unique_ptr; the full definitions are visible only to .cpp
// files inside cpp/demux/ (where they're constructed and used).
#pragma once

#include "common/IngestRingBuffer.h"
#include "mkvparser/mkvparser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace media::demux {

// Streaming IMkvReader backed by an append-only vector (sync/fallback mode).
class StreamReader final : public mkvparser::IMkvReader {
public:
    static constexpr size_t kMaxBufferSize = 2 * 1024 * 1024;
    static constexpr size_t kCompactThreshold = 512 * 1024;

    StreamReader() { buffer_.reserve(65536); }

    size_t append(const uint8_t* data, size_t len) {
        size_t logicalSize = buffer_.size() - logicalStart_;
        size_t avail = (logicalSize < kMaxBufferSize) ? kMaxBufferSize - logicalSize : 0;
        size_t accepted = std::min(len, avail);
        if (accepted == 0) return 0;
        buffer_.insert(buffer_.end(), data, data + accepted);
        return accepted;
    }

    int Read(long long pos, long len, unsigned char* buf) override {
        if (len <= 0) return (len == 0) ? 0 : -1;
        long long localPos = pos - baseOffset_;
        if (localPos < static_cast<long long>(logicalStart_)) return -1;
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (ulen > buffer_.size() || upos > buffer_.size() - ulen) return -1;
        std::memcpy(buf, buffer_.data() + upos, ulen);
        return 0;
    }

    int Length(long long* total, long long* available) override {
        if (total) *total = -1;
        if (available) *available = baseOffset_ + static_cast<long long>(buffer_.size());
        return 0;
    }

    void compact(long long offset) {
        long long localPos = offset - baseOffset_;
        if (localPos <= static_cast<long long>(logicalStart_)) return;
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

    long long available() const { return baseOffset_ + static_cast<long long>(buffer_.size()); }
    size_t bufferSize() const { return buffer_.size() - logicalStart_; }

    const uint8_t* dataAt(long long absPos, long long len = 1) const {
        long long localPos = absPos - baseOffset_;
        if (localPos < static_cast<long long>(logicalStart_) || len <= 0) return nullptr;
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (ulen > buffer_.size() || upos > buffer_.size() - ulen) return nullptr;
        return buffer_.data() + upos;
    }

private:
    std::vector<uint8_t> buffer_;
    long long baseOffset_ = 0;
    size_t logicalStart_ = 0;
};

// IMkvReader backed by IngestRingBuffer — zero-copy demuxing from the ring.
// mkvparser reads directly from ring memory; no intermediate StreamReader copy.
class RingReader final : public mkvparser::IMkvReader {
public:
    explicit RingReader(IngestRingBuffer& ring) noexcept : ring_(ring) {}

    int Read(long long pos, long len, unsigned char* buf) override {
        return ring_.readAt(pos, len, buf);
    }

    int Length(long long* total, long long* available) override {
        if (total) *total = -1;
        if (available) *available = ring_.availableBytes();
        return 0;
    }

    const uint8_t* dataAt(long long absPos, long long len) const noexcept {
        return ring_.dataAt(absPos, len);
    }

    long long available() const noexcept { return ring_.availableBytes(); }

    void compact(long long offset) noexcept { ring_.compact(offset); }

private:
    IngestRingBuffer& ring_;
};

}  // namespace media::demux
