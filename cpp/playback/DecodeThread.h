// Bounded sleep-loop decode thread.
#pragma once

#include <atomic>
#include <cassert>
#include <thread>
#include "common/MediaConfig.h"
#include "common/MediaLog.h"
#include "MediaTypes.h"
#include "AudioDecodeChannel.h"
#include "common/ThreadAffinity.h"
#include "common/CompilerHints.h"
#include "common/TimedJoin.h"
#include "common/HealthWatchdog.h"

namespace media {

// Thread safety: Internal thread performs all decode work.
// - start()/stop() called from main thread only
// - wake()/notifyActive() safe to call from any thread
// - Stats queries return relaxed atomic snapshots
class DecodeThread {
public:
    explicit DecodeThread(AudioDecodeChannel& channel) noexcept
        : audio_(channel) {}

    ~DecodeThread() noexcept { stop(); }

    DecodeThread(const DecodeThread&) = delete;
    DecodeThread& operator=(const DecodeThread&) = delete;

    void notifyActive() noexcept {
        channelActive_.store(true, std::memory_order_release);
        wake();
    }

    [[nodiscard]] bool start() noexcept {
        if (running_.load(std::memory_order_acquire)) return true;

        if (wasDetached_.load(std::memory_order_acquire)) {
            MEDIA_LOG_E("DecodeThread: previous thread was detached, cannot restart");
            return false;
        }

        stopRequested_.store(false, std::memory_order_relaxed);
        threadExited_.store(false, std::memory_order_relaxed);
        wakeVersion_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { threadLoop(); });

        return true;
    }

    [[nodiscard]] bool wasDetached() const noexcept {
        return wasDetached_.load(std::memory_order_acquire);
    }

    void stop() noexcept {
        if (!running_.load(std::memory_order_acquire)) return;

        stopRequested_.store(true, std::memory_order_release);
        wakeVersion_.fetch_add(1, std::memory_order_release);
        wakeVersion_.notify_one();

        timedJoin(thread_, threadExited_, config::thread::kThreadJoinTimeoutMs, "DecodeThread");
        running_.store(false, std::memory_order_release);

        if (!threadExited_.load(std::memory_order_acquire)) {
            wasDetached_.store(true, std::memory_order_release);
            // Release watchdog ownership — the detached thread may still call evaluate().
            // Leaking is intentional: use-after-free is worse than a small leak in this
            // already-degraded state (thread refused to exit).
            (void)healthWatchdog_.release();
        }
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void wake() noexcept {
        wakeVersion_.fetch_add(1, std::memory_order_release);
        wakeVersion_.notify_one();
    }

    [[nodiscard]] int64_t uptimeUs() const noexcept {
        int64_t start = startTimeUs_.load(std::memory_order_relaxed);
        return start > 0 ? nowUs() - start : 0;
    }

    void setHealthWatchdog(std::unique_ptr<HealthWatchdog> wd) noexcept {
        healthWatchdog_ = std::move(wd);
    }

    void resetHealthWindow() noexcept {
        if (healthWatchdog_) healthWatchdog_->requestWindowReset();
    }

    [[nodiscard]] bool isResponsive() const noexcept {
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

    [[nodiscard]] bool wasWatchdogTripped() const noexcept {
        return watchdogTripped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t watchdogTripCount() const noexcept {
        return watchdogTripCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept {
        int64_t heartbeat = lastHeartbeatUs_.load(std::memory_order_relaxed);
        if (heartbeat == 0) return 0;
        return nowUs() - heartbeat;
    }

    void resetWatchdogState() noexcept {
        watchdogTripped_.store(false, std::memory_order_relaxed);
        watchdogTripCount_.store(0, std::memory_order_relaxed);
    }

private:
    void threadLoop() noexcept {
        thread_affinity::setThreadName("OpusDecode");
        thread_affinity::configureCurrentThreadForDecode();

        int64_t startTime = nowUs();
        startTimeUs_.store(startTime, std::memory_order_relaxed);

        int64_t nextHealthLogUs = startTime + config::thread::kHealthLogIntervalUs;
        int consecutiveIdleCycles = 0;
        uint32_t lastWakeVersion = 0;

        MEDIA_LOG_I("DecodeThread: started");

        logHealthStatus();

        while (!stopRequested_.load(std::memory_order_acquire)) {
            bool didWork = processAudio();

            int64_t now = nowUs();
            lastHeartbeatUs_.store(now, std::memory_order_relaxed);
            watchdogTripped_.store(false, std::memory_order_relaxed);

            if (LIKELY(didWork)) {
                consecutiveIdleCycles = 0;
                lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
                // Evaluate health on every work cycle too, not just idle
                if (healthWatchdog_) {
                    healthWatchdog_->evaluate();
                }
                continue;
            }

            consecutiveIdleCycles++;

            if (UNLIKELY(now >= nextHealthLogUs)) {
                logHealthStatus();
                nextHealthLogUs = now + config::thread::kHealthLogIntervalUs;
            }

            if (healthWatchdog_) {
                healthWatchdog_->evaluate();
            }

            int64_t sleepUs;
            if (consecutiveIdleCycles < config::thread::kIdleCycleThreshold) {
                sleepUs = config::thread::kDecodeLoopSleepUs;
            } else {
                int backoffShift = std::min(consecutiveIdleCycles - config::thread::kIdleCycleThreshold, 5);
                sleepUs = config::thread::kDecodeLoopSleepUs << backoffShift;
            }

            waitForWakeOrTimeout(lastWakeVersion, sleepUs);
            lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
        }

        MEDIA_LOG_I("DecodeThread: stopped after %llds",
                    static_cast<long long>(uptimeUs() / 1000000));
        threadExited_.store(true, std::memory_order_release);
    }

    void waitForWakeOrTimeout(uint32_t lastVersion, int64_t timeoutUs) noexcept {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(timeoutUs);

        while (std::chrono::steady_clock::now() < deadline) {
            uint32_t current = wakeVersion_.load(std::memory_order_acquire);
            if (current != lastVersion || stopRequested_.load(std::memory_order_relaxed)) {
                return;
            }
            wakeVersion_.wait(current, std::memory_order_acquire);
        }
    }

    [[nodiscard]] bool processAudio() noexcept {
        if (!channelActive_.load(std::memory_order_acquire)) return false;

        // When paused, keep decoded frames warm but don't decode more
        if (audio_.state() == StreamState::Paused) return false;

        bool didWork = false;

        audio_.serviceDeferredClear();

        if (audio_.hasPendingDecode()) {
            if (audio_.processPendingDecode()) {
                didWork = true;
            }
        }

        if (audio_.needsPLC()) {
            if (audio_.generatePLC()) {
                didWork = true;
            }
        }

        return didWork;
    }

    void logHealthStatus() noexcept {
        if (!channelActive_.load(std::memory_order_acquire)) {
            MEDIA_LOG_V("Health: channel inactive, uptime=%llds",
                        static_cast<long long>(uptimeUs() / 1000000));
            return;
        }

        const auto& m = audio_.metrics();
        uint64_t underruns = m.underruns.load(std::memory_order_relaxed);
        uint64_t dropped = m.framesDropped.load(std::memory_order_relaxed);
        uint64_t drained = m.framesDrained.load(std::memory_order_relaxed);
        uint64_t discontinuities = m.ptsDiscontinuities.load(std::memory_order_relaxed);

        bool hasNewIssues = (underruns > lastLoggedUnderruns_) ||
                           (dropped > lastLoggedDropped_) ||
                           (drained > lastLoggedDrained_) ||
                           (discontinuities > lastLoggedDiscontinuities_);

        lastLoggedUnderruns_ = underruns;
        lastLoggedDropped_ = dropped;
        lastLoggedDrained_ = drained;
        lastLoggedDiscontinuities_ = discontinuities;

        healthLogCounter_++;
        bool isHeartbeat = (healthLogCounter_ % config::thread::kHealthHeartbeatInterval) == 0;

        if (!hasNewIssues && !isHeartbeat) {
            return;
        }

        MEDIA_LOG_V("Health: underruns=%llu dropped=%llu drained=%llu discontinuities=%llu uptime=%llds",
                    static_cast<unsigned long long>(underruns),
                    static_cast<unsigned long long>(dropped),
                    static_cast<unsigned long long>(drained),
                    static_cast<unsigned long long>(discontinuities),
                    static_cast<long long>(uptimeUs() / 1000000));
    }

    AudioDecodeChannel& audio_;
    std::unique_ptr<HealthWatchdog> healthWatchdog_;

    std::thread thread_;
    std::atomic<uint32_t> wakeVersion_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> threadExited_{false};
    std::atomic<bool> wasDetached_{false};
    std::atomic<bool> channelActive_{false};

    std::atomic<int64_t> startTimeUs_{0};
    std::atomic<int64_t> lastHeartbeatUs_{0};
    mutable std::atomic<bool> watchdogTripped_{false};
    mutable std::atomic<uint32_t> watchdogTripCount_{0};

    uint64_t lastLoggedUnderruns_{0};
    uint64_t lastLoggedDropped_{0};
    uint64_t lastLoggedDrained_{0};
    uint64_t lastLoggedDiscontinuities_{0};
    uint32_t healthLogCounter_{0};
};

}  // namespace media
