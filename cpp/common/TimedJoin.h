#pragma once

#include <thread>
#include <chrono>
#include <atomic>
#include "MediaLog.h"

namespace media {

// Joins a thread with a hard deadline to prevent caller hangs.
//
// Phase 1 (soft): Polls `exitFlag` up to `timeoutMs` with 1ms sleeps.
//         If the flag is set, joins immediately and returns true.
// Phase 2 (hard): Polls `exitFlag` up to `hardTimeoutMs` with coarser (50ms)
//         sleeps. C++20 atomic::wait has no timeout overload, so we poll.
//         If the flag is set, joins immediately and returns false.
// Phase 3 (detach): If the thread is still stuck, detaches it to unblock the
//         caller. The detached thread will eventually exit or be killed with
//         the process. Detaching risks a resource leak but prevents ANR on
//         Android and watchdog termination on iOS, which are worse outcomes.
//
// Returns true if joined within the soft timeout (phase 1).
// Returns false if joined after the soft timeout (phase 2) or detached (phase 3).
inline bool timedJoin(std::thread& t, std::atomic<bool>& exitFlag,
                      int64_t timeoutMs, const char* name,
                      int64_t hardTimeoutMs = 500) noexcept {
    if (!t.joinable()) return true;

    using clock = std::chrono::steady_clock;

    // Phase 1: Poll exitFlag with 1ms sleep granularity (soft deadline)
    auto softDeadline = clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!exitFlag.load(std::memory_order_acquire)) {
        if (clock::now() >= softDeadline) goto hardWait;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    t.join();
    return true;

hardWait:
    // Phase 2: Coarser polling with a hard deadline. (C++20 atomic::wait has
    // no timeout overload, so we poll with 50ms granularity below.)
    MEDIA_LOG_E("%s: thread did not exit within %lldms, entering hard wait",
                name, static_cast<long long>(timeoutMs));
    {
        auto hardDeadline = clock::now() + std::chrono::milliseconds(hardTimeoutMs);

        while (!exitFlag.load(std::memory_order_acquire)) {
            auto remaining = hardDeadline - clock::now();
            if (remaining <= clock::duration::zero()) goto detach;

            // C++20 atomic::wait() doesn't support timeout, so poll with
            // coarser granularity to reduce CPU while keeping bounded wait.
            auto sleepMs = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            if (sleepMs.count() > 50) sleepMs = std::chrono::milliseconds(50);
            std::this_thread::sleep_for(sleepMs);
        }

        // Thread exited during hard wait
        t.join();
        return false;
    }

detach:
    // Phase 3: Thread is genuinely stuck — detach to unblock the caller.
    // The thread will be cleaned up when the process exits. This is a resource
    // leak, but preferable to hanging the JS thread indefinitely (ANR/watchdog).
    MEDIA_LOG_E("%s: thread still stuck after %lldms hard wait, detaching (resource leak)",
                name, static_cast<long long>(hardTimeoutMs));
    t.detach();
    return false;
}

}  // namespace media
