#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <utility>
#include <cassert>
#include <thread>
#include <functional>
#include "common/MediaConfig.h"
#include "common/CompilerHints.h"
#include "MediaTypes.h"

namespace media {

template<typename T, size_t PoolSize>
class FramePool;

// Thread safety: FrameToken is NOT thread-safe - must be accessed from single thread.
// Ownership transfer (move) must be properly synchronized if crossing thread boundaries.
// RAII release is safe because inUse_ atomic in pool guarantees visibility.
template<typename T, size_t PoolSize>
class FrameToken {
public:
    FrameToken() noexcept = default;

    FrameToken(FrameToken&& other) noexcept
        : pool_(other.pool_), index_(other.index_) {
        other.pool_ = nullptr;
    }

    FrameToken& operator=(FrameToken&& other) noexcept {
        if (this != &other) {
            release();
            pool_ = other.pool_;
            index_ = other.index_;
            other.pool_ = nullptr;
        }
        return *this;
    }

    ~FrameToken() noexcept { release(); }

    FrameToken(const FrameToken&) = delete;
    FrameToken& operator=(const FrameToken&) = delete;

    [[nodiscard]] T* get() noexcept { return pool_ ? pool_->get(index_) : nullptr; }
    [[nodiscard]] const T* get() const noexcept { return pool_ ? pool_->get(index_) : nullptr; }

    T* operator->() noexcept { return get(); }
    const T* operator->() const noexcept { return get(); }

    T& operator*() noexcept {
        T* ptr = get();
        assert(ptr && "Dereferencing invalid FrameToken");
        return *ptr;
    }

    const T& operator*() const noexcept {
        const T* ptr = get();
        assert(ptr && "Dereferencing invalid FrameToken");
        return *ptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return pool_ != nullptr; }

    void release() noexcept {
        if (pool_) {
            pool_->release(index_);
            pool_ = nullptr;
        }
    }

private:
    friend class FramePool<T, PoolSize>;

    FrameToken(FramePool<T, PoolSize>* pool, size_t index) noexcept
        : pool_(pool), index_(index) {}

    FramePool<T, PoolSize>* pool_ = nullptr;
    size_t index_ = 0;
};

// Thread safety: acquire() and release() are thread-safe via atomic CAS.
// - Multiple threads can acquire concurrently (lock-free)
// - release() uses atomic store with release ordering
// - freeCount_ provides approximate availability (relaxed ordering)
template<typename T, size_t PoolSize>
class FramePool {
    static_assert(PoolSize > 0 && (PoolSize & (PoolSize - 1)) == 0, "PoolSize must be power of 2");

    static size_t threadLocalHint() noexcept {
        // Seed with hash of thread ID to distribute initial probes across pool
        thread_local size_t hint = std::hash<std::thread::id>{}(std::this_thread::get_id());
        return hint++;
    }

public:
    using Token = FrameToken<T, PoolSize>;

    struct PoolStatus {
        size_t available;
        size_t total;
        bool nearExhaustion;  // < threshold% available

        [[nodiscard]] float utilizationPercent() const noexcept {
            return total > 0 ? 100.0f * (1.0f - static_cast<float>(available) / static_cast<float>(total)) : 0.0f;
        }
    };

    FramePool() noexcept {
        for (size_t i = 0; i < PoolSize; ++i) {
            inUse_[i].store(false, std::memory_order_relaxed);
        }
        freeCount_.store(PoolSize, std::memory_order_relaxed);
    }

    // Called from the decode thread (high-priority RT-adjacent). Must not perform
    // I/O, allocate, or block. Exhaustion is tracked via exhaustedCount_ and
    // surfaced through exhaustedCount() for diagnostics on a non-RT thread.
    [[nodiscard]] Token acquire() noexcept {
        size_t free = freeCount_.load(std::memory_order_relaxed);
        if (UNLIKELY(free == 0)) {
            exhaustedCount_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        // Linear scan from a thread-local starting point (TLS hint). Each thread
        // starts at a different offset and auto-increments, distributing probes
        // across the pool to avoid contention. Acceptable for pool sizes 16/64
        // on ARM64 (~1-4 cache lines per scan). A freelist would eliminate the
        // scan but adds per-slot pointer overhead and complicates the lock-free
        // protocol — not worthwhile at these pool sizes.
        size_t startHint = threadLocalHint();

        for (size_t i = 0; i < PoolSize; ++i) {
            size_t idx = (startHint + i) & (PoolSize - 1);
            bool expected = false;
            if (LIKELY(inUse_[idx].compare_exchange_strong(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed))) {
                // freeCount_ is approximate: between CAS and fetch_sub another
                // thread may read a transiently high value. This is safe because
                // freeCount_ is only used for fast-path early-exit and diagnostics.
                freeCount_.fetch_sub(1, std::memory_order_relaxed);
                return Token(this, idx);
            }

            // Early exit if pool became exhausted during scan (check every 8 slots)
            if (UNLIKELY((i & 7) == 7 && freeCount_.load(std::memory_order_relaxed) == 0)) {
                break;
            }
        }

        exhaustedCount_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    [[nodiscard]] T* get(size_t index) noexcept {
        return (index < PoolSize) ? &frames_[index] : nullptr;
    }

    [[nodiscard]] const T* get(size_t index) const noexcept {
        return (index < PoolSize) ? &frames_[index] : nullptr;
    }

    void release(size_t index) noexcept {
        if (index < PoolSize) {
            inUse_[index].store(false, std::memory_order_release);
            // Saturate at PoolSize: use CAS loop to prevent transient over-count
            // when multiple threads release concurrently
            size_t current = freeCount_.load(std::memory_order_relaxed);
            while (true) {
                if (current >= PoolSize) break;
                if (freeCount_.compare_exchange_weak(current, current + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            }
        }
    }

    // Approximate: may transiently over-report during concurrent acquire().
    // Safe for diagnostics and backpressure heuristics, not for exact accounting.
    [[nodiscard]] size_t available() const noexcept {
        return freeCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] PoolStatus status() const noexcept {
        size_t avail = available();
        size_t threshold = (PoolSize * config::pool::kBackpressureThresholdPercent) / 100;
        return {avail, PoolSize, avail < threshold};
    }

    [[nodiscard]] bool isUnderPressure() const noexcept {
        size_t threshold = (PoolSize * config::pool::kBackpressureThresholdPercent) / 100;
        return available() < threshold;
    }

    [[nodiscard]] uint64_t exhaustedCount() const noexcept {
        return exhaustedCount_.load(std::memory_order_relaxed);
    }

    void resetExhaustedCount() noexcept {
        exhaustedCount_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<T, PoolSize> frames_;
    std::array<std::atomic<bool>, PoolSize> inUse_;
    std::atomic<size_t> freeCount_{PoolSize};
    std::atomic<uint64_t> exhaustedCount_{0};
};

// THREAD SAFETY CONTRACT: Single-Producer Single-Consumer (SPSC) Queue
//
// This queue uses memory ordering optimized for exactly ONE producer thread
// and ONE consumer thread. Using multiple producers or consumers causes
// undefined behavior (data races, corruption, lost tokens).
//
// Intended usage pattern:
//   Producer: JS thread (pushEncodedFrame) or Decode thread (push decoded)
//   Consumer: Decode thread (pop encoded) or Audio callback (pop decoded)
//
// The queue does NOT support:
//   - Multiple producers calling push() concurrently
//   - Multiple consumers calling pop() concurrently
//   - Same thread being both producer and consumer (use separate queues)
template<typename T, size_t PoolSize, size_t QueueSize>
class FrameQueue {
    static_assert(QueueSize > 0 && (QueueSize & (QueueSize - 1)) == 0, "QueueSize must be power of 2");

public:
    using Token = FrameToken<T, PoolSize>;
    static constexpr size_t kMask = QueueSize - 1;

    [[nodiscard]] bool push(Token token) noexcept {
        if (UNLIKELY(!token)) return false;

        // Relaxed for own variable (head_), acquire for other thread's variable (tail_).
        // Ensures we see consumer's most recent tail update.
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);

        if (UNLIKELY(head - tail >= QueueSize)) {
            return false;
        }

        slots_[head & kMask] = std::move(token);
        // Release ensures slot write is visible before head increment
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] Token pop() noexcept {
        // Relaxed for own variable (tail_), acquire for other thread's variable (head_).
        // Ensures we see producer's most recent head update and the slot contents.
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) return {};

        Token result = std::move(slots_[tail & kMask]);
        // Release ensures slot read completes before tail increment
        tail_.store(tail + 1, std::memory_order_release);
        return result;
    }

    // Approximate count — may transiently over- or under-estimate by one frame.
    // Relaxed loads mean a concurrent push/pop between the two reads can skew
    // the result in either direction. Callers must tolerate this:
    //   - workRemains check: false positive just retries next cycle (~500us)
    //   - needsPLC(): approximate is fine for threshold comparison
    [[nodiscard]] size_t count() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return (head >= tail) ? (head - tail) : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return count() == 0; }
    [[nodiscard]] bool full() const noexcept { return count() >= QueueSize; }

    void clear() noexcept { while (pop()) {} }

    // Clears queue contents. Thread-safe when called with external synchronization
    // (e.g., when slot is unbound and audio callback has exited via boundParticipant_ check).
    // Caller must ensure no concurrent push/pop operations.
    void clearWithSync() noexcept {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_relaxed);
        while (tail < head) {
            slots_[tail & kMask].release();
            ++tail;
        }
        tail_.store(head, std::memory_order_release);
    }

private:
    std::array<Token, QueueSize> slots_;
    alignas(config::kCacheLineSize) std::atomic<size_t> head_{0};
    alignas(config::kCacheLineSize) std::atomic<size_t> tail_{0};
};

using DecodedAudioPool = FramePool<DecodedAudioFrame, config::audio::kDecodePoolSize>;
using DecodedAudioToken = DecodedAudioPool::Token;
using DecodedAudioQueue = FrameQueue<DecodedAudioFrame, config::audio::kDecodePoolSize, config::audio::kDecodeQueueDepth>;

using PendingAudioPool = FramePool<PendingAudioFrame, config::audio::kPendingFrameCapacity>;
using PendingAudioToken = PendingAudioPool::Token;
using PendingAudioQueue = FrameQueue<PendingAudioFrame, config::audio::kPendingFrameCapacity, config::audio::kPendingFrameCapacity>;

}  // namespace media
