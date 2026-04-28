// SPSC encoded-frame queue + pool with deferred clear coordination.
//
// Threading:
//   Producer (JS thread)        : acquire() → fill token → push()
//   Consumer (decode thread)    : pop() → decode → markWorkConsumed()
//                                 serviceDeferredClear() each cycle
//   Clear requester (any thread): requestClear() — sets a flag the decode
//                                 thread services on its next cycle. Direct
//                                 clear() from the JS thread would violate
//                                 the SPSC single-consumer contract.
//
// `workPending_` is a wake-up signal: set by push, cleared by the consumer
// when the queue drains. It is eventually-consistent — a missed wake-up is
// re-armed by the next push.
#pragma once

#include "common/CompilerHints.h"
#include "FramePool.h"

#include <atomic>

namespace media {

class EncodedFrameQueue {
public:
    using Token = PendingAudioToken;

    // -- Producer (JS thread) --

    // Acquire an empty frame slot from the pool. Returns null if pool is exhausted.
    // The caller fills the slot, then calls push() (or lets the token destruct
    // to release back to the pool on failure).
    [[nodiscard]] Token acquire() noexcept { return pool_.acquire(); }

    // Push a filled token. Returns false if the queue is full (rare — pool
    // capacity is sized to match queue capacity). On success, signals the
    // decode thread via the work-pending flag.
    [[nodiscard]] bool push(Token&& token) noexcept {
        if (!queue_.push(std::move(token))) return false;
        workPending_.store(true, std::memory_order_release);
        return true;
    }

    // -- Consumer (decode thread) --

    // Pop the next frame to decode. Returns null when empty.
    [[nodiscard]] Token pop() noexcept { return queue_.pop(); }

    // True if there may be unconsumed work. Eventually-consistent.
    [[nodiscard]] bool hasPendingWork() const noexcept {
        return workPending_.load(std::memory_order_acquire);
    }

    // Decode thread calls this after draining the queue to clear the wake-up
    // signal. If a producer pushes concurrently, the next push re-arms it.
    void markWorkConsumed() noexcept {
        workPending_.store(false, std::memory_order_release);
    }

    // Service a deferred clear request (set by requestClear()).
    // MUST be called from the decode thread only — the SPSC consumer.
    void serviceDeferredClear() noexcept {
        if (LIKELY(!clearRequested_.load(std::memory_order_acquire))) return;
        queue_.clear();
        clearRequested_.store(false, std::memory_order_release);
    }

    // -- Clear coordination (any thread; typically JS) --

    // Request a deferred clear. Serviced by the decode thread on its next cycle.
    void requestClear() noexcept {
        clearRequested_.store(true, std::memory_order_release);
    }

    // Cancel a pending clear (e.g. seekTo re-fed fresh frames before the
    // decode thread observed the request).
    void cancelClear() noexcept {
        clearRequested_.store(false, std::memory_order_release);
    }

    // -- Status (any thread) --

    [[nodiscard]] size_t count() const noexcept { return queue_.count(); }
    [[nodiscard]] bool isPoolUnderPressure() const noexcept { return pool_.isUnderPressure(); }

private:
    PendingAudioPool pool_;
    PendingAudioQueue queue_;
    std::atomic<bool> workPending_{false};
    std::atomic<bool> clearRequested_{false};
};

}  // namespace media
