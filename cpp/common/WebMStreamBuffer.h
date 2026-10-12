// SPSC byte ring buffer for WebM stream data. The worklet source's reader
// thread writes; the libwebm demux thread reads. Power-of-two capacity for
// fast modular indexing.
//
// Deliberately plain: this ring moves a few hundred KB/s, not audio-callback
// traffic, so it carries no stats batching, debug validators or cache-line
// padding on counters. The owner joins the reader before destroying the ring
// (ios/WebmDemuxPump.mm stop), so the ring guards no lifetime of its own.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace media {

class WebMStreamBuffer {
public:
    // Rounded up to a power of two.
    explicit WebMStreamBuffer(size_t capacityBytes);

    WebMStreamBuffer(const WebMStreamBuffer&) = delete;
    WebMStreamBuffer& operator=(const WebMStreamBuffer&) = delete;

    size_t write(const uint8_t* data, size_t length);
    void setEndOfStream(bool eos = true);
    void clear();
    void shutdown();

    int read(uint8_t* dst, size_t maxLen, uint64_t timeoutMs = 50);

    uint64_t sizeBytes() const noexcept;
    size_t capacity() const noexcept { return capacityBytes_; }

private:
    // One overflow warning per interval: a full ring rejects every chunk until
    // the reader catches up, and each rejection would otherwise log.
    static constexpr uint64_t kLogIntervalMs = 30000;

    size_t capacityBytes_;
    size_t mask_;
    std::unique_ptr<uint8_t[]> buffer_;

    std::atomic<bool> shutdown_{false};
    std::atomic<bool> endOfStream_{false};

    // The only fields both threads touch at rate; padded so producer publishes
    // and consumer publishes never share a line.
    alignas(64) std::atomic<uint64_t> headBytes_{0};
    alignas(64) std::atomic<uint64_t> tailBytes_{0};

    mutable std::atomic<uint64_t> lastLogTimeMs_{0};

    mutable std::mutex cvMutex_;
    std::condition_variable cv_;

    inline size_t indexFor(uint64_t abs) const noexcept {
        return static_cast<size_t>(abs & static_cast<uint64_t>(mask_));
    }

    static uint64_t nowMs();
    bool shouldLog() const;

    int copyOut(uint64_t tail, uint64_t head, uint8_t* dst, size_t maxLen);
    int readSlow(uint8_t* dst, size_t maxLen, uint64_t timeoutMs);
};

}  // namespace media
