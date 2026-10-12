#include "WebMStreamBuffer.h"
#include "MediaLog.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

namespace media {

using Clock = std::chrono::steady_clock;

uint64_t WebMStreamBuffer::nowMs() {
    auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool WebMStreamBuffer::shouldLog() const {
    uint64_t now = nowMs();
    uint64_t last = lastLogTimeMs_.load(std::memory_order_relaxed);
    if (now - last >= kLogIntervalMs) {
        return lastLogTimeMs_.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }
    return false;
}

uint64_t WebMStreamBuffer::sizeBytes() const noexcept {
    uint64_t head = headBytes_.load(std::memory_order_acquire);
    uint64_t tail = tailBytes_.load(std::memory_order_acquire);
    if (head <= tail) return 0;
    uint64_t diff = head - tail;
    if (diff > capacityBytes_) diff = capacityBytes_;
    return diff;
}

WebMStreamBuffer::WebMStreamBuffer(size_t capacityBytes)
    : capacityBytes_(std::bit_ceil(capacityBytes)) {
    mask_ = capacityBytes_ - 1;
    buffer_ = std::make_unique<uint8_t[]>(capacityBytes_);
    MEDIA_LOG_D("WebMStreamBuffer initialized capacity=%.2fMB", capacityBytes_ / 1024.0 / 1024.0);
}

size_t WebMStreamBuffer::write(const uint8_t* data, size_t length) {
    if (!data || length == 0) return 0;
    if (shutdown_.load(std::memory_order_acquire)) return 0;

    uint64_t head = headBytes_.load(std::memory_order_relaxed);
    uint64_t tail = tailBytes_.load(std::memory_order_acquire);
    uint64_t used = (head >= tail) ? (head - tail) : 0;

    // All-or-nothing. These bytes feed a container parser, so accepting only
    // part of a chunk leaves a truncated element in the stream: the next write
    // concatenates straight onto the fragment and the demuxer misparses from
    // there. Rejecting the chunk keeps the stream on a boundary the parser can
    // still make sense of, at the cost of a clean, counted gap.
    uint64_t freeSpace = (used >= capacityBytes_) ? 0 : (capacityBytes_ - used);
    if (freeSpace < static_cast<uint64_t>(length)) {
        if (shouldLog()) {
            MEDIA_LOG_W("WebMStreamBuffer overflow: rejected %zub used=%llu/%zu",
                        length, static_cast<unsigned long long>(used), capacityBytes_);
        }
        return 0;
    }

    const size_t toWrite = length;

    size_t writePos = indexFor(head);
    size_t first = std::min(toWrite, capacityBytes_ - writePos);
    std::memcpy(buffer_.get() + writePos, data, first);
    size_t secondPart = toWrite - first;
    if (secondPart > 0) {
        std::memcpy(buffer_.get(), data + first, secondPart);
    }

    headBytes_.store(head + toWrite, std::memory_order_release);

    // Notify on every publish, not only on an empty→non-empty snapshot: that
    // snapshot was taken before the copy, so a consumer that drained the ring
    // and blocked in the meantime would never be woken — and once the ring was
    // non-empty in every later snapshot, never again. The empty lock section
    // orders this publish against a waiter between its predicate check and its
    // wait; notifying outside the lock avoids waking it into a held mutex.
    { std::lock_guard<std::mutex> lock(cvMutex_); }
    cv_.notify_one();

    return toWrite;
}

void WebMStreamBuffer::setEndOfStream(bool eos) {
    endOfStream_.store(eos, std::memory_order_release);
    if (eos) {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
}

void WebMStreamBuffer::clear() {
    if (shutdown_.load(std::memory_order_acquire)) {
        if (shouldLog()) {
            MEDIA_LOG_W("WebMStreamBuffer::clear() ignored on terminal buffer");
        }
        return;
    }

    // Discard by advancing tail to head, never by zeroing the counters:
    // positions are monotonic, and the consumer republishes tail with a CAS, so
    // a read in flight across this call fails its CAS and reports nothing read.
    // Zeroing instead let that read land its stale tail after the clear, leaving
    // tail ahead of head — the consumer then saw an empty ring until head
    // re-earned the gap: a permanent, silent stall of playback.
    uint64_t head = headBytes_.load(std::memory_order_relaxed);
    tailBytes_.store(head, std::memory_order_release);
    endOfStream_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
}

void WebMStreamBuffer::shutdown() {
    bool already = shutdown_.exchange(true, std::memory_order_acq_rel);
    endOfStream_.store(true, std::memory_order_release);
    if (already) return;
    std::lock_guard<std::mutex> lock(cvMutex_);
    cv_.notify_all();
}

int WebMStreamBuffer::read(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    if (!dst || maxLen == 0) return 0;

    if (shutdown_.load(std::memory_order_acquire)) {
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t tail = tailBytes_.load(std::memory_order_acquire);
        if (head == tail) return -1;
    }

    uint64_t tail = tailBytes_.load(std::memory_order_relaxed);
    uint64_t head = headBytes_.load(std::memory_order_acquire);
    if (head > tail) return copyOut(tail, head, dst, maxLen);
    return readSlow(dst, maxLen, timeoutMs);
}

// Clamps to what is available, copies out, and publishes the new tail.
// Requires head > tail; both read() paths share this so the publish contract
// cannot drift between them again.
int WebMStreamBuffer::copyOut(uint64_t tail, uint64_t head, uint8_t* dst, size_t maxLen) {
    size_t toRead = static_cast<size_t>(std::min<uint64_t>(head - tail, maxLen));

    size_t readPos = indexFor(tail);
    size_t first = std::min(toRead, capacityBytes_ - readPos);
    std::memcpy(dst, buffer_.get() + readPos, first);
    size_t second = toRead - first;
    if (second > 0) std::memcpy(dst + first, buffer_.get(), second);

    // CAS, not a blind store: clear() advances tail from another thread, and a
    // clear that lands during the copy above discards exactly the bytes just
    // copied — losing the CAS and reporting nothing read is the correct
    // outcome. Release still orders the copy-out before the producer's acquire
    // sees the space as free.
    if (!tailBytes_.compare_exchange_strong(tail, tail + toRead,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
        return 0;
    }

    return static_cast<int>(toRead);
}

int WebMStreamBuffer::readSlow(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(cvMutex_);
    auto predicate = [this]() {
        if (shutdown_.load(std::memory_order_acquire)) return true;
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t tail = tailBytes_.load(std::memory_order_acquire);
        if (head != tail) return true;
        if (endOfStream_.load(std::memory_order_acquire)) return true;
        return false;
    };

    if (!predicate()) {
        if (timeoutMs > 0) cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate);
        else cv_.wait(lock, predicate);
    }

    if (shutdown_.load(std::memory_order_acquire)) {
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t tail = tailBytes_.load(std::memory_order_acquire);
        if (head == tail) return -1;
    }

    uint64_t head = headBytes_.load(std::memory_order_acquire);
    uint64_t tail = tailBytes_.load(std::memory_order_acquire);
    if (head <= tail) {
        if (endOfStream_.load(std::memory_order_acquire)) return -1;
        return 0;
    }
    return copyOut(tail, head, dst, maxLen);
}

}  // namespace media
