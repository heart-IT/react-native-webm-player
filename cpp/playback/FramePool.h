// Lock-free frame pool and SPSC queue for the audio pipeline.
//
// FramePool<T, N>: Pre-allocated fixed-size pool. acquire() returns a FrameToken
// (RAII handle that auto-releases on destruction). Lock-free via atomic CAS.
//
// FrameQueue<T, PoolSize, QueueSize>: Single-Producer Single-Consumer queue of
// FrameTokens with acquire/release memory ordering. Used for JS→decode and
// decode→callback handoffs.
//
// Both are designed for the audio hot path: no allocations, no locks, no exceptions.
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
//
// Lifetime contract: a FrameToken MUST NOT outlive its FramePool.  ~FrameToken()
// dereferences the raw pool_ pointer to release the slot; if the pool was
// destroyed, that read is a UAF.  Teardown order is therefore:
//   1. Stop producers (decode thread, JS thread).
//   2. Stop consumers (audio callback) so no fresh Tokens are popped.
//   3. Drain queues holding Tokens (FrameQueue::clear()) so dtors run.
//   4. Destroy the FramePool.
// The orchestrator owns this ordering — see MediaSessionBase teardown.
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
    // I/O, allocate, or block. Returns an empty Token if the pool is exhausted.
    [[nodiscard]] Token acquire() noexcept {
        size_t free = freeCount_.load(std::memory_order_relaxed);
        if (UNLIKELY(free == 0)) return {};

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
            // Approximate counter — transient over-count from concurrent
            // acquire/release is acceptable (used for diagnostics and
            // backpressure heuristics only, not exact accounting).
            freeCount_.fetch_add(1, std::memory_order_relaxed);
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

private:
    std::array<T, PoolSize> frames_;
    std::array<std::atomic<bool>, PoolSize> inUse_;
    std::atomic<size_t> freeCount_{PoolSize};
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

        size_t head = head_.load(std::memory_order_relaxed);

        // Fast path: check cached tail first to avoid cross-core atomic load.
        // Only reload the real tail_ when the cached value says the queue is full.
        if (UNLIKELY(head - tailCached_ >= QueueSize)) {
            tailCached_ = tail_.load(std::memory_order_acquire);
            if (head - tailCached_ >= QueueSize) {
                return false;
            }
        }

        slots_[head & kMask] = std::move(token);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] Token pop() noexcept {
        size_t tail = tail_.load(std::memory_order_relaxed);

        // Fast path: check cached head first to avoid cross-core atomic load.
        // Only reload the real head_ when the cached value says the queue is empty.
        if (tail >= headCached_) {
            headCached_ = head_.load(std::memory_order_acquire);
            if (tail >= headCached_) return {};
        }

        Token result = std::move(slots_[tail & kMask]);
        tail_.store(tail + 1, std::memory_order_release);
        return result;
    }

    // Approximate count — may transiently over- or under-estimate by one frame.
    // Uses cached indices on fast path, falls back to atomic load.
    // Callers must tolerate approximation:
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

private:
    std::array<Token, QueueSize> slots_;
    // Producer cache line: head_ (written by producer) + tailCached_ (producer-local).
    // Colocated so producer only touches this one cache line on the fast path.
    alignas(config::kCacheLineSize) std::atomic<size_t> head_{0};
    size_t tailCached_{0};
    // Consumer cache line: tail_ (written by consumer) + headCached_ (consumer-local).
    // Colocated so consumer only touches this one cache line on the fast path.
    alignas(config::kCacheLineSize) std::atomic<size_t> tail_{0};
    size_t headCached_{0};
};

using DecodedAudioPool = FramePool<DecodedAudioFrame, config::audio::kDecodePoolSize>;
using DecodedAudioToken = DecodedAudioPool::Token;
using DecodedAudioQueue = FrameQueue<DecodedAudioFrame, config::audio::kDecodePoolSize, config::audio::kDecodeQueueDepth>;

using PendingAudioPool = FramePool<PendingAudioFrame, config::audio::kPendingFrameCapacity>;
using PendingAudioToken = PendingAudioPool::Token;
using PendingAudioQueue = FrameQueue<PendingAudioFrame, config::audio::kPendingFrameCapacity, config::audio::kPendingFrameCapacity>;

}  // namespace media
