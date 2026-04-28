// Reusable bounded sleep-loop worker thread with idle backoff.
//
// Extracts the lifecycle boilerplate shared by DecodeThread, VideoDecodeThread,
// and IngestThread into a single template. The type parameter Processor provides
// domain-specific work via process().
//
// Processor concept (duck-typed, resolved at compile time):
//   bool process() noexcept;          // Return true if useful work was done
//   void onDetached() noexcept {}     // Optional: called when thread is detached
//
// Threading:
//   start()/stop()/pause()/resume() called from owner thread only.
//   wake() safe from any thread.
//   isResponsive()/timeSinceLastHeartbeatUs() safe from any thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "MediaConfig.h"
#include "MediaLog.h"
#include "MediaTime.h"
#include "ThreadAffinity.h"
#include "TimedJoin.h"
#include "CompilerHints.h"
#include "HealthWatchdog.h"

namespace media {

struct WorkerThreadConfig {
    const char* name;
    int64_t baseSleepUs;
    int maxBackoffShift;
    bool highPriority;       // true = configureForDecode, false = configureForWorker
    bool supportsPause;      // true = enable pause gate
};

template<typename Processor>
class WorkerThread {
public:
    WorkerThread(Processor& processor, WorkerThreadConfig config) noexcept
        : processor_(processor), config_(config) {}

    ~WorkerThread() noexcept { stop(); }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    [[nodiscard]] bool start() noexcept {
        if (running_.load(std::memory_order_acquire)) return true;

        if (wasDetached_.load(std::memory_order_acquire)) {
            MEDIA_LOG_E("%s: previous thread was detached, cannot restart", config_.name);
            return false;
        }

        stopRequested_.store(false, std::memory_order_relaxed);
        threadExited_.store(false, std::memory_order_relaxed);
        pauseRequested_.store(false, std::memory_order_relaxed);
        paused_.store(false, std::memory_order_relaxed);
        wakeVersion_.store(0, std::memory_order_relaxed);
        startTimeUs_.store(nowUs(), std::memory_order_relaxed);
        // Clear per-run watchdog state so wasWatchdogTripped() doesn't leak across restart.
        watchdogTripped_.store(false, std::memory_order_relaxed);
        watchdogTripCount_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { threadLoop(); });
        return true;
    }

    void stop() noexcept {
        if (!running_.load(std::memory_order_acquire)) return;

        stopRequested_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(waitMtx_);
            wakeVersion_.fetch_add(1, std::memory_order_release);
        }
        waitCv_.notify_all();

        timedJoin(thread_, threadExited_, config::thread::kThreadJoinTimeoutMs, config_.name);
        running_.store(false, std::memory_order_release);

        if (!threadExited_.load(std::memory_order_acquire)) {
            wasDetached_.store(true, std::memory_order_release);
            // Release owned resources to prevent use-after-free from the detached thread.
            if (healthWatchdog_) (void)healthWatchdog_.release();
            processor_.onDetached();
            MEDIA_LOG_E("%s: thread detached (refused to exit)", config_.name);
        }
    }

    void wake() noexcept {
        {
            std::lock_guard<std::mutex> lk(waitMtx_);
            wakeVersion_.fetch_add(1, std::memory_order_release);
        }
        waitCv_.notify_one();
    }

    // Returns true if the worker acknowledged the pause (observed paused_=true)
    // within the deadline. False on timeout, stop, or no-pause-support — callers
    // must NOT proceed with mutations that rely on the worker being quiescent
    // when false is returned.
    [[nodiscard]] bool pause() noexcept {
        if (!config_.supportsPause) return false;
        if (!running_.load(std::memory_order_acquire)) return false;
        pauseRequested_.store(true, std::memory_order_release);
        wake();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!paused_.load(std::memory_order_acquire)) {
            if (stopRequested_.load(std::memory_order_acquire)) return false;
            if (!running_.load(std::memory_order_acquire)) return false;
            if (std::chrono::steady_clock::now() >= deadline) {
                MEDIA_LOG_W("%s: pause timed out (100ms)", config_.name);
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    void resume() noexcept {
        if (!config_.supportsPause) return;
        pauseRequested_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
        wake();
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool wasDetached() const noexcept {
        return wasDetached_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isResponsive() noexcept {
        if (!running_.load(std::memory_order_acquire)) return false;
        int64_t heartbeat = lastHeartbeatUs_.load(std::memory_order_relaxed);
        if (heartbeat == 0) return true;
        bool responsive = (nowUs() - heartbeat) < config::thread::kHealthLogIntervalUs * 3;
        if (!responsive && !watchdogTripped_.load(std::memory_order_relaxed)) {
            watchdogTripped_.store(true, std::memory_order_relaxed);
            watchdogTripCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return responsive;
    }

    [[nodiscard]] int64_t uptimeUs() const noexcept {
        int64_t start = startTimeUs_.load(std::memory_order_relaxed);
        return start > 0 ? nowUs() - start : 0;
    }

    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept {
        int64_t heartbeat = lastHeartbeatUs_.load(std::memory_order_relaxed);
        return heartbeat == 0 ? 0 : nowUs() - heartbeat;
    }

    [[nodiscard]] bool wasWatchdogTripped() const noexcept {
        return watchdogTripped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t watchdogTripCount() const noexcept {
        return watchdogTripCount_.load(std::memory_order_relaxed);
    }

    void resetWatchdogState() noexcept {
        watchdogTripped_.store(false, std::memory_order_relaxed);
        watchdogTripCount_.store(0, std::memory_order_relaxed);
    }

    void setHealthWatchdog(std::unique_ptr<HealthWatchdog> wd) noexcept {
        healthWatchdog_ = std::move(wd);
    }

    void resetHealthWindow() noexcept {
        if (healthWatchdog_) healthWatchdog_->requestWindowReset();
    }

private:
    void threadLoop() noexcept {
        thread_affinity::setThreadName(config_.name);
        if (config_.highPriority) {
            thread_affinity::configureCurrentThreadForDecode();
        } else {
            thread_affinity::configureCurrentThreadForWorker();
        }

        int consecutiveIdleCycles = 0;
        uint32_t lastWakeVersion = 0;

        MEDIA_LOG_I("%s: started", config_.name);

        while (!stopRequested_.load(std::memory_order_acquire)) {
            // Pause gate
            if (config_.supportsPause && UNLIKELY(pauseRequested_.load(std::memory_order_acquire))) {
                paused_.store(true, std::memory_order_release);
                {
                    std::unique_lock<std::mutex> lk(waitMtx_);
                    waitCv_.wait(lk, [&] {
                        return !pauseRequested_.load(std::memory_order_acquire) ||
                               stopRequested_.load(std::memory_order_acquire);
                    });
                    lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
                }
                paused_.store(false, std::memory_order_release);
                consecutiveIdleCycles = 0;
                continue;
            }

            bool didWork = processor_.process();

            // Heartbeat
            lastHeartbeatUs_.store(nowUs(), std::memory_order_relaxed);
            watchdogTripped_.store(false, std::memory_order_relaxed);

            // Health watchdog: evaluate after real work, but skip once we're deep into
            // idle backoff — no counters are changing so re-evaluation just burns nowUs()
            // syscalls and battery on sparse streams.
            if (healthWatchdog_ &&
                (didWork || consecutiveIdleCycles < config::thread::kIdleCycleThreshold)) {
                healthWatchdog_->evaluate();
            }

            if (LIKELY(didWork)) {
                consecutiveIdleCycles = 0;
                lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
                continue;
            }

            consecutiveIdleCycles++;

            int64_t sleepUs;
            if (consecutiveIdleCycles < config::thread::kIdleCycleThreshold) {
                sleepUs = config_.baseSleepUs;
            } else {
                int backoffShift = std::min(
                    consecutiveIdleCycles - config::thread::kIdleCycleThreshold,
                    config_.maxBackoffShift);
                sleepUs = config_.baseSleepUs << backoffShift;
            }

            waitForWakeOrTimeout(lastWakeVersion, sleepUs);
            lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
        }

        MEDIA_LOG_I("%s: stopped", config_.name);
        threadExited_.store(true, std::memory_order_release);
    }

    // C++20 std::atomic::wait has no timeout overload, so adaptive idle backoff
    // requires a condition_variable::wait_for. wake() and stop() bump
    // wakeVersion_ under waitMtx_ then notify the CV; the predicate reads the
    // atomic to detect both wake and stop.
    void waitForWakeOrTimeout(uint32_t lastVersion, int64_t timeoutUs) noexcept {
        std::unique_lock<std::mutex> lk(waitMtx_);
        waitCv_.wait_for(lk, std::chrono::microseconds(timeoutUs), [&] {
            return wakeVersion_.load(std::memory_order_acquire) != lastVersion ||
                   stopRequested_.load(std::memory_order_acquire);
        });
    }

    Processor& processor_;
    WorkerThreadConfig config_;
    std::unique_ptr<HealthWatchdog> healthWatchdog_;

    std::thread thread_;
    std::mutex waitMtx_;
    std::condition_variable waitCv_;
    std::atomic<uint32_t> wakeVersion_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> threadExited_{false};
    std::atomic<bool> wasDetached_{false};
    std::atomic<bool> pauseRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int64_t> startTimeUs_{0};
    std::atomic<int64_t> lastHeartbeatUs_{0};
    mutable std::atomic<bool> watchdogTripped_{false};
    mutable std::atomic<uint32_t> watchdogTripCount_{0};
};

}  // namespace media
