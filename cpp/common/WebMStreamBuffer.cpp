#include "WebMStreamBuffer.h"
#include "MediaLog.h"

#include <algorithm>
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

size_t WebMStreamBuffer::getDefaultCapacity() {
    return 32 * 1024 * 1024;
}

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

void WebMStreamBuffer::updateBitrate(size_t bytes) {
    if (bytes < BITRATE_SKIP_THRESHOLD) {
        bytesSinceLastUpdate_.fetch_add(bytes, std::memory_order_relaxed);
        return;
    }
    bytesSinceLastUpdate_.fetch_add(bytes, std::memory_order_relaxed);

    uint64_t now = nowMs();
    uint64_t expected = lastBitrateUpdateMs_.load(std::memory_order_relaxed);
    if (expected == 0) {
        if (lastBitrateUpdateMs_.compare_exchange_strong(expected, now, std::memory_order_relaxed)) return;
        expected = lastBitrateUpdateMs_.load(std::memory_order_relaxed);
    }
    uint64_t delta = now - expected;
    if (delta >= 1000) {
        if (lastBitrateUpdateMs_.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
            uint64_t windowBytes = bytesSinceLastUpdate_.exchange(0, std::memory_order_relaxed);
            double bits = static_cast<double>(windowBytes) * 8.0;
            double bps = (bits * 1000.0) / std::max<double>(static_cast<double>(delta), 1.0);
            lastBitrateBitsPerSec_.store(static_cast<uint64_t>(bps), std::memory_order_relaxed);
        }
    }
}

WebMStreamBuffer::ConsumerActiveGuard::ConsumerActiveGuard(WebMStreamBuffer& owner) : owner_(owner) {
    owner_.consumerActiveCount_.fetch_add(1, std::memory_order_relaxed);
}

WebMStreamBuffer::ConsumerActiveGuard::~ConsumerActiveGuard() {
    owner_.consumerActiveCount_.fetch_sub(1, std::memory_order_relaxed);
}

WebMStreamBuffer::WebMStreamBuffer(size_t capacityBytes, const Config& cfg)
    : cfg_(cfg), capacityBytes_(capacityBytes) {
    if (capacityBytes_ < cfg_.minCapacityBytes) capacityBytes_ = cfg_.minCapacityBytes;
    if (capacityBytes_ < MIN_CAPACITY) capacityBytes_ = MIN_CAPACITY;

    size_t pow2 = 1;
    while (pow2 < capacityBytes_) pow2 <<= 1;
    capacityBytes_ = pow2;
    mask_ = capacityBytes_ - 1;

    buffer_ = std::make_unique<uint8_t[]>(capacityBytes_);

    uint64_t now = nowMs();
    lastProducerActivityMs_.store(now, std::memory_order_relaxed);
    lastConsumerActivityMs_.store(now, std::memory_order_relaxed);

    MEDIA_LOG_D("WebMStreamBuffer initialized capacity=%.2fMB", capacityBytes_ / 1024.0 / 1024.0);
}

WebMStreamBuffer::~WebMStreamBuffer() {
    shutdown_.store(true, std::memory_order_release);
    endOfStream_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_all();
    }
    uint64_t grace = cfg_.shutdownGraceMs;
    if (grace > 0) {
        uint64_t start = nowMs();
        uint64_t waitTime = 1;
        while (consumerActiveCount_.load(std::memory_order_relaxed) > 0 && (nowMs() - start) < grace) {
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

    if (used >= capacityBytes_) {
        bufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        droppedBytes_.fetch_add(length, std::memory_order_relaxed);
        producerLocalDroppedBytes_.fetch_add(length, std::memory_order_relaxed);
        producerLocalBufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        uint64_t now = nowMs();
        if (producerLocalDroppedBytes_ >= cfg_.statsFlushMinBytes || producerLocalBufferOverflows_ > 10) {
            flushProducerStatsIfNeeded(now);
        }
        consumerLagEvents_.fetch_add(1, std::memory_order_relaxed);
        if (shouldLog(cfg_.logMinIntervalMs)) {
            MEDIA_LOG_W("WebMStreamBuffer overflow: dropped %zub used=%llu/%zu",
                        length, static_cast<unsigned long long>(used), capacityBytes_);
        }
        return 0;
    }

    uint64_t freeSpace = (used >= capacityBytes_) ? 0 : (capacityBytes_ - used);
    size_t toWrite = static_cast<size_t>(std::min<uint64_t>(freeSpace, static_cast<uint64_t>(length)));
    if (toWrite == 0) {
        consumerLagEvents_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    bool wasEmpty = (used == 0);

    if (toWrite < length) {
        size_t dropped = length - toWrite;
        droppedBytes_.fetch_add(dropped, std::memory_order_relaxed);
        bufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        producerLocalDroppedBytes_.fetch_add(dropped, std::memory_order_relaxed);
        producerLocalBufferOverflows_.fetch_add(1, std::memory_order_relaxed);
        consumerLagEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t writePos = indexFor(head);
    size_t first = std::min(toWrite, capacityBytes_ - writePos);
    std::memcpy(buffer_.get() + writePos, data, first);
    size_t secondPart = toWrite - first;
    if (secondPart > 0) {
        std::memcpy(buffer_.get(), data + first, secondPart);
    }

    lastProducerActivityMs_.store(nowMs(), std::memory_order_relaxed);
    headBytes_.store(head + toWrite, std::memory_order_release);

    auto localWritten = producerLocalBytesWritten_.fetch_add(toWrite, std::memory_order_relaxed) + toWrite;
    if (localWritten >= cfg_.statsFlushMinBytes) {
        flushProducerStatsIfNeeded(nowMs());
    }

    updateBitrate(toWrite);

    if (wasEmpty) {
        std::lock_guard<std::mutex> lock(cvMutex_);
        cv_.notify_one();
    }

    return toWrite;
}

void WebMStreamBuffer::setEndOfStream(bool eos) {
    if (destroyed_.load(std::memory_order_relaxed)) return;
    endOfStream_.store(eos, std::memory_order_release);
    lastProducerActivityMs_.store(nowMs(), std::memory_order_relaxed);
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

    headBytes_.store(0, std::memory_order_relaxed);
    tailBytes_.store(0, std::memory_order_relaxed);
    endOfStream_.store(false, std::memory_order_release);

    totalBytesWritten_.store(0, std::memory_order_relaxed);
    totalBytesRead_.store(0, std::memory_order_relaxed);
    droppedBytes_.store(0, std::memory_order_relaxed);
    bufferOverflows_.store(0, std::memory_order_relaxed);
    consumerLagEvents_.store(0, std::memory_order_relaxed);
    lastBitrateBitsPerSec_.store(0, std::memory_order_relaxed);
    lastBitrateUpdateMs_.store(0, std::memory_order_relaxed);
    bytesSinceLastUpdate_.store(0, std::memory_order_relaxed);
    corruptionEvents_.store(0, std::memory_order_relaxed);

    softResets_.store(0, std::memory_order_relaxed);
    hardResets_.store(0, std::memory_order_relaxed);
    lastResetTimeMs_.store(0, std::memory_order_relaxed);

    producerLocalBytesWritten_.store(0, std::memory_order_relaxed);
    producerLocalDroppedBytes_.store(0, std::memory_order_relaxed);
    producerLocalBufferOverflows_.store(0, std::memory_order_relaxed);
    consumerLocalBytesRead_.store(0, std::memory_order_relaxed);
    consumerLocalLagEvents_.store(0, std::memory_order_relaxed);

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
    auto localLag = consumerLocalLagEvents_.exchange(0, std::memory_order_relaxed);
    if (localLag > 0) consumerLagEvents_.fetch_add(localLag, std::memory_order_release);
}

int WebMStreamBuffer::read(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    (void)validateThread("read");
    if (!dst || maxLen == 0) return 0;
    if (destroyed_.load(std::memory_order_acquire)) return -1;
    if (shutdown_.load(std::memory_order_acquire)) {
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t tail = tailBytes_.load(std::memory_order_acquire);
        if (head == tail) return -1;
    }

    while (true) {
        uint64_t tail = tailBytes_.load(std::memory_order_relaxed);
        uint64_t head = headBytes_.load(std::memory_order_acquire);
        uint64_t available = (head >= tail) ? (head - tail) : 0;
        if (available == 0) break;

        size_t toRead = static_cast<size_t>(std::min<uint64_t>(available, maxLen));
        if (toRead < cfg_.batchReadThreshold && available > cfg_.batchReadThreshold * 4) {
            size_t batch = cfg_.batchReadThreshold;
            if (batch > maxLen) batch = maxLen;
            toRead = batch;
        }

        size_t readPos = indexFor(tail);
        size_t first = std::min(toRead, capacityBytes_ - readPos);
        std::memcpy(dst, buffer_.get() + readPos, first);
        size_t second = toRead - first;
        if (second > 0) std::memcpy(dst + first, buffer_.get(), second);

        tailBytes_.store(tail + toRead, std::memory_order_release);

        auto localRead = consumerLocalBytesRead_.fetch_add(toRead, std::memory_order_relaxed) + toRead;
        if (localRead >= cfg_.statsFlushMinBytes) {
            flushConsumerStatsIfNeeded(nowMs());
        }
        lastConsumerActivityMs_.store(nowMs(), std::memory_order_relaxed);
        return static_cast<int>(toRead);
    }

    return readSlow(dst, maxLen, timeoutMs);
}

int WebMStreamBuffer::readSlow(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    if (!dst || maxLen == 0) return 0;

    ConsumerActiveGuard guard(*this);
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
    uint64_t available = (head >= tail) ? (head - tail) : 0;
    if (available == 0) {
        if (endOfStream_.load(std::memory_order_acquire)) return -1;
        return 0;
    }

    size_t toRead = static_cast<size_t>(std::min<uint64_t>(available, maxLen));
    if (toRead < cfg_.batchReadThreshold && available > cfg_.batchReadThreshold * 4) {
        size_t batch = cfg_.batchReadThreshold;
        if (batch > maxLen) batch = maxLen;
        toRead = batch;
    }

    size_t readPos = indexFor(tail);
    size_t first = std::min(toRead, capacityBytes_ - readPos);
    std::memcpy(dst, buffer_.get() + readPos, first);
    size_t second = toRead - first;
    if (second > 0) std::memcpy(dst + first, buffer_.get(), second);

    tailBytes_.store(tail + toRead, std::memory_order_release);

    auto localRead = consumerLocalBytesRead_.fetch_add(toRead, std::memory_order_relaxed) + toRead;
    if (localRead >= cfg_.statsFlushMinBytes) {
        flushConsumerStatsIfNeeded(nowMs());
    }
    lastConsumerActivityMs_.store(nowMs(), std::memory_order_relaxed);
    return static_cast<int>(toRead);
}

int WebMStreamBuffer::readBatch(uint8_t* dst, size_t maxLen, uint64_t timeoutMs) {
    return readSlow(dst, maxLen, timeoutMs);
}

void WebMStreamBuffer::softReset() {
    if (shutdown_.load(std::memory_order_acquire)) return;

    uint64_t head = headBytes_.load(std::memory_order_relaxed);
    uint64_t tail = tailBytes_.load(std::memory_order_relaxed);
    uint64_t used = (head >= tail) ? (head - tail) : 0;
    if (used == 0) return;

    uint64_t targetUsed = capacityBytes_ / 2;
    uint64_t newTail = (used > targetUsed) ? (head - targetUsed) : tail;
    if (newTail < tail) newTail = tail;

    tailBytes_.store(newTail, std::memory_order_release);
    softResets_.fetch_add(1, std::memory_order_relaxed);
    lastResetTimeMs_.store(nowMs(), std::memory_order_relaxed);
    consumerLagEvents_.fetch_add(1, std::memory_order_relaxed);

    if (shouldLog(cfg_.logMinIntervalMs)) {
        MEDIA_LOG_W("WebMStreamBuffer soft reset used=%llu newTail=%llu",
                    static_cast<unsigned long long>(used), static_cast<unsigned long long>(newTail));
    }
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
    s.estimatedBitrateBitsPerSec = lastBitrateBitsPerSec_.load(std::memory_order_relaxed);
    s.currentSizeBytes = sizeBytesRelaxed();
    s.capacityBytes = capacityBytes_;
    s.endOfStream = endOfStream_.load(std::memory_order_acquire);
    s.shutdown = shutdown_.load(std::memory_order_acquire);
    return s;
}

bool WebMStreamBuffer::isConsumerLagging() const {
    uint64_t used = sizeBytesRelaxed();
    return used > static_cast<uint64_t>(capacityBytes_ * 0.8);
}

WebMStreamBuffer::HealthStatus WebMStreamBuffer::getHealthStatus() const {
    if (shutdown_.load(std::memory_order_acquire) || destroyed_.load(std::memory_order_acquire)) {
        return HealthStatus::Dead;
    }
    uint64_t now = nowMs();
    uint64_t lastProd = lastProducerActivityMs_.load(std::memory_order_relaxed);
    uint64_t lastCons = lastConsumerActivityMs_.load(std::memory_order_relaxed);
    if (now - lastProd > cfg_.producerStallMs) return HealthStatus::ProducerStalled;
    if (now - lastCons > cfg_.consumerStallMs) return HealthStatus::ConsumerStalled;

    uint64_t used = sizeBytesRelaxed();
    double fillRatio = (capacityBytes_ > 0)
        ? static_cast<double>(used) / static_cast<double>(capacityBytes_) : 0.0;
    if (fillRatio > cfg_.severeBackpressureRatio) return HealthStatus::SevereBackpressure;
    return HealthStatus::Healthy;
}

WebMStreamBuffer::RecoveryStats WebMStreamBuffer::getRecoveryStats() const {
    RecoveryStats r;
    r.softResets = softResets_.load(std::memory_order_relaxed);
    r.hardResets = hardResets_.load(std::memory_order_relaxed);
    r.lastResetTimeMs = lastResetTimeMs_.load(std::memory_order_relaxed);
    return r;
}

void WebMStreamBuffer::goToLive() {
    std::lock_guard<std::mutex> lock(cvMutex_);
    if (shutdown_.load(std::memory_order_acquire) || destroyed_.load(std::memory_order_acquire)) return;
    uint64_t head = headBytes_.load(std::memory_order_acquire);
    tailBytes_.store(head, std::memory_order_release);
    cv_.notify_all();
    if (shouldLog(cfg_.logMinIntervalMs)) {
        MEDIA_LOG_W("WebMStreamBuffer goToLive head=%llu", static_cast<unsigned long long>(head));
    }
}

bool WebMStreamBuffer::isBehindLive(size_t thresholdBytes) const noexcept {
    uint64_t head = headBytes_.load(std::memory_order_acquire);
    uint64_t tail = tailBytes_.load(std::memory_order_acquire);
    uint64_t used = (head >= tail) ? (head - tail) : 0;
    return used > thresholdBytes;
}

}  // namespace media
