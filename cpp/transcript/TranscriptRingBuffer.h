// Lock-free SPSC ring buffer for tapping audio PCM from the RT callback thread.
//
// Producer: audio callback thread (push only, RT-safe — no locks, no allocs)
// Consumer: transcript thread (read only, may block)
//
// Overflow silently overwrites oldest samples. This is intentional:
// transcript quality may degrade but audio playback is never affected.
// Capacity must be a power of 2 for efficient bitmask indexing.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
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
        size_t wp = writePos_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            buffer_[(wp + i) & mask_] = data[i];
        }
        writePos_.store(wp + frames, std::memory_order_release);
    }

    // Called from transcript thread. Returns number of frames read.
    // Sets overflowed to true if samples were skipped.
    size_t read(float* output, size_t maxFrames, bool* overflowed = nullptr) noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_;

        size_t available = wp - rp;
        bool overflow = available > capacity_;
        if (overflow) {
            rp = wp - capacity_;
            available = capacity_;
        }
        if (overflowed) *overflowed = overflow;

        size_t toRead = std::min(available, maxFrames);
        for (size_t i = 0; i < toRead; ++i) {
            output[i] = buffer_[(rp + i) & mask_];
        }
        readPos_ = rp + toRead;
        return toRead;
    }

    [[nodiscard]] size_t available() const noexcept {
        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t avail = wp - readPos_;
        return avail > capacity_ ? capacity_ : avail;
    }

    void reset() noexcept {
        readPos_ = writePos_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    const size_t mask_;
    const size_t capacity_;
    float* const buffer_;
    std::atomic<size_t> writePos_{0};
    size_t readPos_ = 0;
};

}  // namespace media::transcript
