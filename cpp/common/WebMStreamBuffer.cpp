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

bool WebMStreamBuffer::shouldLog(uint64_t minIntervalMs) const {
    uint64_t now = nowMs();
    uint64_t last = lastLogTimeMs_.load(std::memory_order_relaxed);
    if (now - last >= minIntervalMs) {
        return lastLogTimeMs_.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }
    return false;
}

#ifndef NDEBUG
bool WebMStreamBuffer::validateThread(const char* op) const {
    std::lock_guard<std::mutex> lock(debugThreadValidationMutex_);
    auto current = std::this_thread::get_id();
    if (producerThreadId_ == std::thread::id()) { producerThreadId_ = current; return true; }
    if (consumerThreadId_ == std::thread::id()) { consumerThreadId_ = current; return true; }
    if (current != producerThreadId_ && current != consumerThreadId_) {
        if (shouldLog(cfg_.logMinIntervalMs)) {
            MEDIA_LOG_W("WebMStreamBuffer thread validation failed in %s: unexpected thread", op);
        }
    }
    return true;
}

bool WebMStreamBuffer::validateWebMClusterBoundary(const uint8_t* data, size_t length) const {
    if (length < 4) return false;
    static const uint8_t EBML_HEADER[4] = {0x1A, 0x45, 0xDF, 0xA3};
    static const uint8_t CLUSTER_ID[4] = {0x1F, 0x43, 0xB6, 0x75};
    if (std::memcmp(data, EBML_HEADER, 4) == 0) return true;
    if (std::memcmp(data, CLUSTER_ID, 4) == 0) return true;
    return false;
}
#endif

uint64_t WebMStreamBuffer::sizeBytes(std::memory_order order) const noexcept {
    uint64_t head = headBytes_.load(order);
    uint64_t tail = tailBytes_.load(order);
    if (head <= tail) return 0;
    uint64_t diff = head - tail;
    if (diff > capacityBytes_) diff = capacityBytes_;
    return diff;
}

uint64_t WebMStreamBuffer::sizeBytesRelaxed() const noexcept {
    return sizeBytes(std::memory_order_relaxed);
}

uint64_t WebMStreamBuffer::sizeBytes() const noexcept {
    return sizeBytes(std::memory_order_acquire);
}

WebMStreamBuffer::ConsumerActiveGuard::ConsumerActiveGuard(WebMStreamBuffer& owner) : owner_(owner) {
    owner_.consumerActiveCount_.fetch_add(1, std::memory_order_acquire);
}

WebMStreamBuffer::ConsumerActiveGuard::~ConsumerActiveGuard() {
    // Release, paired with the destructor's acquire load. Counting alone is not
    // enough: without this edge the consumer's reads of buffer_ are not ordered
    // before the destructor observes zero and frees it, so the wait would still
    // be a data race even though it waits correctly.
    owner_.consumerActiveCount_.fetch_sub(1, std::memory_order_release);
}

WebMStreamBuffer::WebMStreamBuffer(size_t capacityBytes)
    : WebMStreamBuffer(capacityBytes, Config{}) {}

WebMStreamBuffer::WebMStreamBuffer(size_t capacityBytes, const Config& cfg)
    : cfg_(cfg), capacityBytes_(capacityBytes) {
    if (capacityBytes_ < cfg_.minCapacityBytes) capacityBytes_ = cfg_.minCapacityBytes;
    if (capacityBytes_ < MIN_CAPACITY) capacityBytes_ = MIN_CAPACITY;

    capacityBytes_ = std::bit_ceil(capacityBytes_);
    mask_ = capacityBytes_ - 1;

    buffer_ = std::make_unique<uint8_t[]>(capacityBytes_);

    MEDIA_LOG_D("WebMStreamBuffer initialized capacity=%.2fMB", capacityBytes_ / 1024.0 / 1024.0);
}

WebMStreamBuffer::~WebMStreamBuffer() {
    shutdown_.store(true, std::memory_order_release);
    endOfStream_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
    // Wait out any consumer currently inside read(). shutdown_ is already set
    // above, so a blocked reader wakes immediately and one in the copy path is
    // bounded by a single memcpy — this drains in bounded time.
    //
    // It does NOT time out. Giving up here means freeing the object while a
    // consumer is executing inside it, which is silent memory corruption;
    // shutdownGraceMs is the threshold past which we say so, not a deadline
    // after which we proceed anyway. A destructor that blocks is a visible bug;
    // one that frees under a live reader is not.
    {
        uint64_t start = nowMs();
        uint64_t waitTime = 1;
        bool warned = false;
        while (consumerActiveCount_.load(std::memory_order_acquire) > 0) {
            if (!warned && cfg_.shutdownGraceMs > 0 &&
                (nowMs() - start) >= cfg_.shutdownGraceMs) {
                warned = true;
                MEDIA_LOG_E("WebMStreamBuffer destroyed with %u consumer(s) still "
                            "active after %llums — the owner must join consumers "
                            "before destroying the ring",
                            consumerActiveCount_.load(std::memory_order_relaxed),
                            static_cast<unsigned long long>(cfg_.shutdownGraceMs));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(waitTime));
            if (waitTime < 16) waitTime *= 2;
        }
    }
    destroyed_.store(true, std::memory_order_release);
}

size_t WebMStreamBuffer::write(const uint8_t* data, size_t length, bool isClusterBoundary) {
    (void)validateThread("write");
    if (!data || length == 0) return 0;
    if (shutdown_.load(std::memory_order_acquire) || destroyed_.load(std::memory_order_acquire)) return 0;

#ifndef NDEBUG
    if (isClusterBoundary && !validateWebMClusterBoundary(data, length)) {
        nonClusterWrites_.fetch_add(1, std::memory_order_relaxed);
        if (shouldLog(cfg_.logMinIntervalMs)) {
            MEDIA_LOG_W("WebMStreamBuffer cluster-boundary mismatch (count=%zu)",
                        nonClusterWrites_.load(std::memory_order_relaxed));
        }
    }
#else
    (void)isClusterBoundary;
#endif

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
        bufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        droppedBytes_.fetch_add(length, std::memory_order_relaxed);
        producerLocalDroppedBytes_.fetch_add(length, std::memory_order_relaxed);
        producerLocalBufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        consumerLagEvents_.fetch_add(1, std::memory_order_relaxed);
        if (producerLocalDroppedBytes_.load(std::memory_order_relaxed) >= cfg_.statsFlushMinBytes ||
            producerLocalBufferOverflows_.load(std::memory_order_relaxed) > 10) {
            flushProducerStatsIfNeeded(nowMs());
        }
        if (shouldLog(cfg_.logMinIntervalMs)) {
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

    auto localWritten = producerLocalBytesWritten_.fetch_add(toWrite, std::memory_order_relaxed) + toWrite;
    if (localWritten >= cfg_.statsFlushMinBytes) {
        flushProducerStatsIfNeeded(nowMs());
    }

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
    if (destroyed_.load(std::memory_order_relaxed)) return;
    endOfStream_.store(eos, std::memory_order_release);
    if (eos) {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
}

void WebMStreamBuffer::clear() {
    if (shutdown_.load(std::memory_order_acquire)) {
        if (shouldLog(cfg_.logMinIntervalMs)) {
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

    totalBytesWritten_.store(0, std::memory_order_relaxed);
    totalBytesRead_.store(0, std::memory_order_relaxed);
    droppedBytes_.store(0, std::memory_order_relaxed);
    bufferOverflows_.store(0, std::memory_order_relaxed);
    consumerLagEvents_.store(0, std::memory_order_relaxed);

    producerLocalBytesWritten_.store(0, std::memory_order_relaxed);
    producerLocalDroppedBytes_.store(0, std::memory_order_relaxed);
    producerLocalBufferOverflows_.store(0, std::memory_order_relaxed);
    consumerLocalBytesRead_.store(0, std::memory_order_relaxed);

    producerStatsLastFlushMs_.store(0, std::memory_order_relaxed);
    consumerStatsLastFlushMs_.store(0, std::memory_order_relaxed);

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

void WebMStreamBuffer::flushProducerStatsIfNeeded(uint64_t now) const {
    uint64_t last = producerStatsLastFlushMs_.load(std::memory_order_relaxed);
    if (now - last < 50) return;
    if (!producerStatsLastFlushMs_.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;

    auto localWritten = producerLocalBytesWritten_.exchange(0, std::memory_order_relaxed);
    if (localWritten > 0) totalBytesWritten_.fetch_add(localWritten, std::memory_order_release);
    auto localDropped = producerLocalDroppedBytes_.exchange(0, std::memory_order_relaxed);
    if (localDropped > 0) droppedBytes_.fetch_add(localDropped, std::memory_order_release);
    auto localOverflows = producerLocalBufferOverflows_.exchange(0, std::memory_order_relaxed);
    if (localOverflows > 0) bufferOverflows_.fetch_add(localOverflows, std::memory_order_release);
}

void WebMStreamBuffer::flushConsumerStatsIfNeeded(uint64_t now) const {
    uint64_t last = consumerStatsLastFlushMs_.load(std::memory_order_relaxed);
    if (now - last < 50) return;
    if (!consumerStatsLastFlushMs_.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;

    auto localRead = consumerLocalBytesRead_.exchange(0, std::memory_order_relaxed);
    if (localRead > 0) totalBytesRead_.fetch_add(localRead, std::memory_order_release);
}

int WebMStreamBuffer::read(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    (void)validateThread("read");
    if (!dst || maxLen == 0) return 0;

    // Taken before any buffer access, and held for the whole call. The fast path
    // below copies straight out of buffer_, so leaving it unguarded made an
    // in-flight reader invisible to the destructor's drain loop.
    ConsumerActiveGuard guard(*this);

    if (destroyed_.load(std::memory_order_acquire)) return -1;
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

    auto localRead = consumerLocalBytesRead_.fetch_add(toRead, std::memory_order_relaxed) + toRead;
    if (localRead >= cfg_.statsFlushMinBytes) {
        flushConsumerStatsIfNeeded(nowMs());
    }
    return static_cast<int>(toRead);
}

// Caller must hold a ConsumerActiveGuard; read() does.
int WebMStreamBuffer::readSlow(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    if (!dst || maxLen == 0) return 0;

    if (destroyed_.load(std::memory_order_acquire)) return -1;
    if (shutdown_.load(std::memory_order_acquire)) {
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t tail = tailBytes_.load(std::memory_order_acquire);
        if (head == tail) return -1;
    }

    std::unique_lock<std::mutex> lock(cvMutex_);
    auto predicate = [this]() {
        if (shutdown_.load(std::memory_order_acquire)) return true;
        if (destroyed_.load(std::memory_order_acquire)) return true;
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

    if (destroyed_.load(std::memory_order_acquire)) return -1;
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

WebMStreamBuffer::Stats WebMStreamBuffer::getStats() const {
    flushProducerStatsIfNeeded(nowMs());
    flushConsumerStatsIfNeeded(nowMs());
    Stats s;
    s.totalBytesWritten = totalBytesWritten_.load(std::memory_order_relaxed);
    s.totalBytesRead = totalBytesRead_.load(std::memory_order_relaxed);
    s.droppedBytes = droppedBytes_.load(std::memory_order_relaxed);
    s.bufferOverflows = bufferOverflows_.load(std::memory_order_relaxed);
    s.consumerLagEvents = consumerLagEvents_.load(std::memory_order_relaxed);
    s.currentSizeBytes = sizeBytesRelaxed();
    s.capacityBytes = capacityBytes_;
    s.endOfStream = endOfStream_.load(std::memory_order_acquire);
    s.shutdown = shutdown_.load(std::memory_order_acquire);
    return s;
}

}  // namespace media
