// Lock-free SPSC byte ring buffer for WebM stream data.
// Producer (JS via feedData) writes; consumer (ExoPlayer DataSource on Android,
// libwebm demuxer on iOS) reads. Power-of-two capacity for fast modular indexing.
//
// Ported from call-doctor-mobile (battle-tested in production). Namespace and
// logging adjusted to webm-player conventions; behavior unchanged.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace media {

class WebMStreamBuffer {
public:
    struct Config {
        size_t minCapacityBytes;
        uint64_t producerStallMs;
        uint64_t consumerStallMs;
        double severeBackpressureRatio;
        size_t batchReadThreshold;
        uint64_t shutdownGraceMs;
        uint64_t logMinIntervalMs;
        size_t statsFlushMinBytes;

        Config()
            : minCapacityBytes(16 * 1024 * 1024),
              producerStallMs(10000),
              consumerStallMs(10000),
              severeBackpressureRatio(0.95),
              batchReadThreshold(4096),
              shutdownGraceMs(500),
              logMinIntervalMs(5000),
              statsFlushMinBytes(64 * 1024) {}
    };

    enum class HealthStatus {
        Healthy,
        ProducerStalled,
        ConsumerStalled,
        SevereBackpressure,
        Dead
    };

    struct RecoveryStats {
        uint64_t softResets;
        uint64_t hardResets;
        uint64_t lastResetTimeMs;
    };

    struct Stats {
        uint64_t totalBytesWritten = 0;
        uint64_t totalBytesRead = 0;
        uint64_t droppedBytes = 0;
        uint64_t bufferOverflows = 0;
        uint64_t consumerLagEvents = 0;
        uint64_t estimatedBitrateBitsPerSec = 0;
        uint64_t currentSizeBytes = 0;
        size_t capacityBytes = 0;
        bool endOfStream = false;
        bool shutdown = false;
    };

    static size_t getDefaultCapacity();

    explicit WebMStreamBuffer(size_t capacityBytes, const Config& cfg = Config());
    ~WebMStreamBuffer();

    WebMStreamBuffer(const WebMStreamBuffer&) = delete;
    WebMStreamBuffer& operator=(const WebMStreamBuffer&) = delete;

    bool isDestroyed() const noexcept {
        return destroyed_.load(std::memory_order_acquire);
    }

    size_t write(const uint8_t* data, size_t length, bool isClusterBoundary = true);
    void setEndOfStream(bool eos = true);
    void clear();
    void shutdown();

    int read(uint8_t* dst, size_t maxLen, uint64_t timeoutMs = 50);
    int readBatch(uint8_t* dst, size_t maxLen, uint64_t timeoutMs = 50);

    Stats getStats() const;
    uint64_t sizeBytes() const noexcept;
    size_t capacity() const noexcept { return capacityBytes_; }
    bool isConsumerLagging() const;

    void softReset();

    HealthStatus getHealthStatus() const;
    RecoveryStats getRecoveryStats() const;

    void goToLive();
    bool isBehindLive(size_t thresholdBytes) const noexcept;

private:
    static constexpr size_t MIN_CAPACITY = 8 * 1024 * 1024;
    static constexpr size_t BITRATE_SKIP_THRESHOLD = 128;

    class ConsumerActiveGuard {
    public:
        explicit ConsumerActiveGuard(WebMStreamBuffer& owner);
        ~ConsumerActiveGuard();
    private:
        WebMStreamBuffer& owner_;
    };

    Config cfg_;
    size_t capacityBytes_;
    size_t mask_;
    std::unique_ptr<uint8_t[]> buffer_;

    std::atomic<bool> shutdown_{false};
    std::atomic<bool> endOfStream_{false};
    std::atomic<bool> destroyed_{false};

    alignas(64) std::atomic<uint64_t> headBytes_{0};
    alignas(64) std::atomic<uint64_t> tailBytes_{0};

    alignas(64) mutable std::atomic<uint64_t> totalBytesWritten_{0};
    alignas(64) mutable std::atomic<uint64_t> droppedBytes_{0};
    alignas(64) mutable std::atomic<uint64_t> bufferOverflows_{0};

    alignas(64) mutable std::atomic<uint64_t> totalBytesRead_{0};
    alignas(64) mutable std::atomic<uint64_t> consumerLagEvents_{0};

    alignas(64) mutable std::atomic<uint64_t> lastBitrateBitsPerSec_{0};
    alignas(64) mutable std::atomic<uint64_t> lastBitrateUpdateMs_{0};
    alignas(64) mutable std::atomic<uint64_t> bytesSinceLastUpdate_{0};

    alignas(64) mutable std::atomic<uint64_t> lastProducerActivityMs_{0};
    alignas(64) mutable std::atomic<uint64_t> lastConsumerActivityMs_{0};

    alignas(64) mutable std::atomic<uint64_t> softResets_{0};
    alignas(64) mutable std::atomic<uint64_t> hardResets_{0};
    alignas(64) mutable std::atomic<uint64_t> lastResetTimeMs_{0};

    alignas(64) mutable std::atomic<uint64_t> corruptionEvents_{0};
    alignas(64) mutable std::atomic<uint64_t> lastLogTimeMs_{0};

    alignas(64) std::atomic<uint32_t> consumerActiveCount_{0};

    mutable std::atomic<uint64_t> producerLocalBytesWritten_{0};
    mutable std::atomic<uint64_t> producerLocalDroppedBytes_{0};
    mutable std::atomic<uint64_t> producerLocalBufferOverflows_{0};
    mutable std::atomic<uint64_t> consumerLocalBytesRead_{0};
    mutable std::atomic<uint64_t> consumerLocalLagEvents_{0};
    mutable std::atomic<uint64_t> producerStatsLastFlushMs_{0};
    mutable std::atomic<uint64_t> consumerStatsLastFlushMs_{0};

    mutable std::mutex cvMutex_;
    std::condition_variable cv_;

#ifndef NDEBUG
    mutable std::atomic<size_t> nonClusterWrites_{0};
    mutable std::thread::id producerThreadId_{};
    mutable std::thread::id consumerThreadId_{};
    mutable std::mutex debugThreadValidationMutex_;
    bool validateThread(const char* op) const;
    bool validateWebMClusterBoundary(const uint8_t* data, size_t length) const;
#else
    inline bool validateThread(const char*) const { return true; }
    inline bool validateWebMClusterBoundary(const uint8_t*, size_t) const { return true; }
#endif

    inline size_t indexFor(uint64_t abs) const noexcept {
        return static_cast<size_t>(abs & static_cast<uint64_t>(mask_));
    }

    uint64_t sizeBytes(std::memory_order order) const noexcept;
    uint64_t sizeBytesRelaxed() const noexcept;

    void updateBitrate(size_t bytes);
    static uint64_t nowMs();
    bool shouldLog(uint64_t minIntervalMs) const;

    void flushProducerStatsIfNeeded(uint64_t now) const;
    void flushConsumerStatsIfNeeded(uint64_t now) const;

    int readSlow(uint8_t* dst, size_t maxLen, uint64_t timeoutMs);
};

}  // namespace media
