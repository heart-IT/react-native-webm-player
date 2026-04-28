// Lock-free SPSC ring buffer for tapping audio PCM from the RT callback thread.
//
// Producer: audio callback thread (push only, RT-safe — no locks, no allocs)
// Consumer: transcript thread (read only, may block)
//
// Overflow overwrites oldest samples by design — transcript quality may
// degrade but audio playback is never affected. The producer never blocks
// the RT thread to wait for the consumer. An overflow counter is bumped
// (relaxed) so triage can detect when whisper.cpp falls behind production.
// Capacity must be a power of 2 for efficient bitmask indexing.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace media::transcript {

class TranscriptRingBuffer {
public:
    // capacity must be power of 2
    explicit TranscriptRingBuffer(size_t capacity)
        : mask_(capacity - 1)
        , capacity_(capacity)
        , buffer_(new float[capacity]) {
        assert((capacity & (capacity - 1)) == 0 && capacity > 0);
    }

    ~TranscriptRingBuffer() { delete[] buffer_; }

    TranscriptRingBuffer(const TranscriptRingBuffer&) = delete;
    TranscriptRingBuffer& operator=(const TranscriptRingBuffer&) = delete;

    // RT-safe: called from audio callback. No locks, no allocs.
    // Overwrites oldest data on overflow.
    void push(const float* data, size_t frames) noexcept {
        // Defensive bound: a pathological frame count > capacity would overflow
        // the second memcpy. In practice audio-callback frame counts are far
        // below capacity; clamping drops the overage (keeps the tail) and
        // bumps overflowCount_ so the consumer can see we lost samples here.
        if (frames > capacity_) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            data += frames - capacity_;
            frames = capacity_;
        }
        size_t wp = writePos_.load(std::memory_order_relaxed);
        size_t rp = readPos_.load(std::memory_order_acquire);
        // If the producer is about to lap the consumer, bump the counter so the
        // overflow is visible even though we still write (the read path will
        // observe the lap and discard, but correlation needs a producer-side
        // counter — by the time read() observes it the cause is already past).
        if (wp - rp + frames > capacity_) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
        }
        size_t offset = wp & mask_;
        size_t firstChunk = std::min(frames, capacity_ - offset);
        std::memcpy(buffer_ + offset, data, firstChunk * sizeof(float));
        if (firstChunk < frames) {
            std::memcpy(buffer_, data + firstChunk, (frames - firstChunk) * sizeof(float));
        }
        writePos_.store(wp + frames, std::memory_order_release);
    }

    // Called from transcript thread. Returns number of frames read.
    // Sets overflowed to true if samples were skipped, or if the producer lapped us
    // during the memcpy and we had to discard the read.
    size_t read(float* output, size_t maxFrames, bool* overflowed = nullptr) noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_relaxed);

        size_t available = wp - rp;
        bool overflow = available > capacity_;
        if (overflow) {
            rp = wp - capacity_;
            available = capacity_;
        }

        size_t toRead = std::min(available, maxFrames);
        size_t offset = rp & mask_;
        size_t firstChunk = std::min(toRead, capacity_ - offset);
        std::memcpy(output, buffer_ + offset, firstChunk * sizeof(float));
        if (firstChunk < toRead) {
            std::memcpy(output + firstChunk, buffer_, (toRead - firstChunk) * sizeof(float));
        }

        // Torn-read guard: if the producer wrote past (rp + toRead + capacity_) during
        // our memcpy, the region we just read was overwritten. Discard, signal overflow.
        size_t wpAfter = writePos_.load(std::memory_order_acquire);
        if (wpAfter - rp > capacity_) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            if (overflowed) *overflowed = true;
            readPos_.store(wpAfter - capacity_, std::memory_order_release);
            return 0;
        }

        if (overflowed) *overflowed = overflow;
        readPos_.store(rp + toRead, std::memory_order_release);
        return toRead;
    }

    // Triage signal: how many times the producer detected (or the consumer
    // observed) that whisper.cpp inference fell behind audio production.
    // Monotonic; resets only on object reconstruction.
    [[nodiscard]] uint64_t overflowCount() const noexcept {
        return overflowCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t available() const noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_acquire);
        size_t avail = wp - rp;
        return avail > capacity_ ? capacity_ : avail;
    }

    void reset() noexcept {
        readPos_.store(writePos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    const size_t mask_;
    const size_t capacity_;
    float* const buffer_;
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
    std::atomic<uint64_t> overflowCount_{0};
};

}  // namespace media::transcript
