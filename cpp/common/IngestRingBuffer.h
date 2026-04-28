// Lock-free SPSC byte ring buffer for passing raw WebM bytes from JS to the
// ingest thread, doubling as the demuxer's IMkvReader backing store.
//
// Producer: JS thread (write only — single memcpy per feedData call)
// Consumer: mkvparser (random-access reads within the live window)
// Compactor: ingest thread (advances compactPos to free space for producer)
//
// No-overwrite: write() returns false when full (backpressure to JS).
// Capacity must be a power of 2 for efficient bitmask indexing.
//
// Retention: when clip capture / DVR is enabled, retainPos_ prevents
// compaction past the oldest retained cluster, keeping historical data
// readable for extraction without a separate buffer copy.
//
// Implements mkvparser::IMkvReader so the demuxer reads directly from this
// buffer without an intermediate StreamReader copy.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "MediaConfig.h"

// Forward declaration — full include in .cpp files only
namespace mkvparser { class IMkvReader; }

namespace media {

struct ReadRegion {
    const uint8_t* data1 = nullptr;
    size_t len1 = 0;
    const uint8_t* data2 = nullptr;
    size_t len2 = 0;
    [[nodiscard]] size_t total() const noexcept { return len1 + len2; }
};

class IngestRingBuffer {
public:
    explicit IngestRingBuffer(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(new uint8_t[capacity]) {
        assert((capacity & (capacity - 1)) == 0 && "capacity must be power of 2");
        assert(capacity > 0);
    }

    ~IngestRingBuffer() { delete[] buffer_; }

    IngestRingBuffer(const IngestRingBuffer&) = delete;
    IngestRingBuffer& operator=(const IngestRingBuffer&) = delete;

    // ---- Producer API (JS thread) ----

    // Copy bytes into ring buffer. Returns false if insufficient space.
    [[nodiscard]] bool write(const uint8_t* data, size_t len) noexcept {
        if (len == 0) return true;
        size_t wp = writePos_.load(std::memory_order_relaxed);
        size_t cp = compactPos_.load(std::memory_order_acquire);
        size_t used = wp - cp;
        if (len > capacity_ - used) {
            writeRejects_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        size_t offset = wp & mask_;
        size_t firstChunk = std::min(len, capacity_ - offset);
        std::memcpy(buffer_ + offset, data, firstChunk);
        if (firstChunk < len) {
            std::memcpy(buffer_, data + firstChunk, len - firstChunk);
        }
        writePos_.store(wp + len, std::memory_order_release);
        return true;
    }

    // Reports how many write() calls were rejected due to insufficient space.
    // Monotonic across reset() — session-long rejects are a useful triage signal.
    [[nodiscard]] uint64_t writeRejects() const noexcept {
        return writeRejects_.load(std::memory_order_relaxed);
    }

    // ---- Demuxer/IMkvReader API (ingest thread) ----

    // Read bytes at absolute stream position into buf.
    // Returns 0 on success, -1 if data is unavailable (compacted or not yet written).
    int readAt(long long absPos, long len, unsigned char* buf) const noexcept {
        if (len <= 0) return (len == 0) ? 0 : -1;
        long long localPos = absPos - baseOffset_.load(std::memory_order_relaxed);
        if (localPos < 0) return -1;

        size_t cp = compactPos_.load(std::memory_order_relaxed);
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t windowStart = cp;
        size_t windowEnd = wp;

        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (upos < windowStart || ulen > windowEnd || upos > windowEnd - ulen) return -1;

        size_t ringOffset = upos & mask_;
        size_t firstChunk = std::min(ulen, capacity_ - ringOffset);
        std::memcpy(buf, buffer_ + ringOffset, firstChunk);
        if (firstChunk < ulen) {
            std::memcpy(buf + firstChunk, buffer_, ulen - firstChunk);
        }
        return 0;
    }

    // Get direct pointer to data at absolute position (zero-copy packet access).
    // Only valid if the region does not wrap around the ring boundary.
    // Returns nullptr if data wraps, is compacted, or not yet available.
    const uint8_t* dataAt(long long absPos, long long len) const noexcept {
        if (len <= 0) return nullptr;
        long long localPos = absPos - baseOffset_.load(std::memory_order_relaxed);
        if (localPos < 0) return nullptr;

        size_t cp = compactPos_.load(std::memory_order_relaxed);
        size_t wp = writePos_.load(std::memory_order_acquire);
        auto upos = static_cast<size_t>(localPos);
        auto ulen = static_cast<size_t>(len);
        if (upos < cp || ulen > wp || upos > wp - ulen) return nullptr;

        size_t ringOffset = upos & mask_;
        // Check if the region wraps around the ring boundary
        if (ringOffset + ulen > capacity_) return nullptr;

        return buffer_ + ringOffset;
    }

    // Get a two-part read region for an absolute byte range (handles wrap-around).
    // Returns empty region if data is unavailable (compacted or not yet written).
    [[nodiscard]] ReadRegion readRegion(long long absPos, size_t len) const noexcept {
        ReadRegion region{};
        if (len == 0) return region;
        long long localPos = absPos - baseOffset_.load(std::memory_order_relaxed);
        if (localPos < 0) return region;

        size_t cp = compactPos_.load(std::memory_order_relaxed);
        size_t wp = writePos_.load(std::memory_order_acquire);
        auto upos = static_cast<size_t>(localPos);
        if (upos < cp || len > wp || upos > wp - len) return region;

        size_t ringOffset = upos & mask_;
        size_t firstChunk = std::min(len, capacity_ - ringOffset);
        region.data1 = buffer_ + ringOffset;
        region.len1 = firstChunk;
        if (firstChunk < len) {
            region.data2 = buffer_;
            region.len2 = len - firstChunk;
        }
        return region;
    }

    // Total bytes available to mkvparser (from compact position to write position).
    long long availableBytes() const noexcept {
        return baseOffset_.load(std::memory_order_relaxed) + static_cast<long long>(
            writePos_.load(std::memory_order_acquire));
    }

    long long baseOffset() const noexcept { return baseOffset_.load(std::memory_order_relaxed); }

    // How many bytes are in the live window (not yet compacted).
    // Saturates to 0 if a torn reset() interleaves a fresh writePos_ with
    // a stale compactPos_ — the diagnostic must not flash 2^64 when the
    // invariant wp >= cp is briefly broken during reset().
    size_t liveBytes() const noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t cp = compactPos_.load(std::memory_order_relaxed);
        return wp >= cp ? wp - cp : 0;
    }

    // ---- Compaction API (ingest thread) ----

    // Advance the compact position, freeing space for the producer.
    // absOffset is the absolute stream byte position to compact up to.
    // Respects retainPos_: will not compact past retained data.
    void compact(long long absOffset) noexcept {
        long long localPos = absOffset - baseOffset_.load(std::memory_order_relaxed);
        if (localPos <= 0) return;
        size_t cp = compactPos_.load(std::memory_order_relaxed);
        auto newCp = static_cast<size_t>(localPos);
        if (newCp <= cp) return;

        // Respect retention: never compact past either retain position
        size_t rp = retainPos_.load(std::memory_order_acquire);
        size_t drp = decodeRetainPos_.load(std::memory_order_acquire);
        size_t minRetain = 0;
        if (rp > 0 && drp > 0) minRetain = std::min(rp, drp);
        else if (rp > 0) minRetain = rp;
        else if (drp > 0) minRetain = drp;
        if (minRetain > 0 && newCp > minRetain) {
            newCp = minRetain;
        }
        if (newCp <= cp) {
            // Caller asked to advance, but a retain pin (clip / decode floor)
            // held us back. Surface this so triage can correlate ring pressure
            // with retention rather than just slow consumers.
            compactionStalls_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        compactPos_.store(newCp, std::memory_order_release);
    }

    // Reports how many compact() calls were held back by a retain pin
    // (retainPos_ / decodeRetainPos_) when there was data to compact.
    [[nodiscard]] uint64_t compactionStalls() const noexcept {
        return compactionStalls_.load(std::memory_order_relaxed);
    }

    // ---- Retention API (clip capture / DVR) ----

    // Set the earliest byte position that must not be compacted.
    // 0 = retention disabled (compact freely).
    void setRetainPos(size_t pos) noexcept {
        retainPos_.store(pos, std::memory_order_release);
    }

    [[nodiscard]] size_t retainPos() const noexcept {
        return retainPos_.load(std::memory_order_acquire);
    }

    // Decode retention: prevents compaction past oldest undecoded packet.
    void setDecodeRetainPos(size_t pos) noexcept {
        decodeRetainPos_.store(pos, std::memory_order_release);
    }

    [[nodiscard]] size_t decodeRetainPos() const noexcept {
        return decodeRetainPos_.load(std::memory_order_acquire);
    }

    // Bytes retained for clip capture (from retainPos to writePos).
    [[nodiscard]] size_t retainedBytes() const noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = retainPos_.load(std::memory_order_relaxed);
        if (rp == 0) return 0;
        return (wp > rp) ? wp - rp : 0;
    }

    // ---- General ----

    [[nodiscard]] size_t compactPos() const noexcept {
        return compactPos_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t freeSpace() const noexcept {
        size_t wp = writePos_.load(std::memory_order_relaxed);
        size_t cp = compactPos_.load(std::memory_order_acquire);
        // Saturate to capacity_ if a torn reset() leaves cp > wp briefly —
        // matches liveBytes() (line 168). The diagnostic must not flash 0
        // (or wrap) when the wp >= cp invariant is transiently broken.
        return wp >= cp ? capacity_ - (wp - cp) : capacity_;
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    // Check if new data has arrived since the last check.
    [[nodiscard]] bool hasNewData(size_t lastKnownWritePos) const noexcept {
        return writePos_.load(std::memory_order_acquire) > lastKnownWritePos;
    }

    [[nodiscard]] size_t currentWritePos() const noexcept {
        return writePos_.load(std::memory_order_acquire);
    }

    // Caller SHOULD pause the ingest thread before calling reset(). If pause
    // times out and readers (audio/video decode) are concurrently reading,
    // they may see a torn mid-reset state — all reader paths handle this by
    // returning -1/nullptr when the window is inconsistent (the bounds-check
    // arithmetic rejects any mismatch between baseOffset and compactPos/writePos).
    //
    // All stores are release-ordered so any reader that observes the new
    // writePos_ also sees the new baseOffset_ / compactPos_ / retain positions.
    void reset() noexcept {
        compactPos_.store(0, std::memory_order_release);
        retainPos_.store(0, std::memory_order_release);
        decodeRetainPos_.store(0, std::memory_order_release);
        baseOffset_.store(0, std::memory_order_release);
        writePos_.store(0, std::memory_order_release);
    }

private:
    const size_t capacity_;
    const size_t mask_;
    uint8_t* const buffer_;
    alignas(config::kCacheLineSize) std::atomic<size_t> writePos_{0};
    alignas(config::kCacheLineSize) std::atomic<size_t> compactPos_{0};
    alignas(config::kCacheLineSize) std::atomic<size_t> retainPos_{0};
    alignas(config::kCacheLineSize) std::atomic<size_t> decodeRetainPos_{0};
    alignas(config::kCacheLineSize) std::atomic<long long> baseOffset_{0};
    std::atomic<uint64_t> writeRejects_{0};
    std::atomic<uint64_t> compactionStalls_{0};
};

}  // namespace media
