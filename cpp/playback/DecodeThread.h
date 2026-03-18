// Bounded sleep-loop decode thread with watchdog liveness check.
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <thread>
#include <array>
#include "common/MediaConfig.h"
#include "common/MediaLog.h"
#include "MediaTypes.h"
#include "AudioDecodeSlot.h"
#include "common/ThreadAffinity.h"
#include "common/CompilerHints.h"
#include "common/TimedJoin.h"
#include "Participant.h"

namespace media {

class SlotAdmission;

// Thread safety: Internal thread performs all decode work.
// - start()/stop() called from main thread only
// - wake()/notifySlotBound()/notifySlotUnbound() safe to call from any thread
// - Watchdog queries (isHealthy(), wasWatchdogTripped()) safe from any thread
// - Stats queries return relaxed atomic snapshots
class DecodeThread {
public:
    static constexpr int kMaxAudioSlots = config::limits::kMaxAudioSlots;

    explicit DecodeThread(std::array<AudioDecodeSlot*, kMaxAudioSlots>& slots) noexcept
        : audioSlots_(slots) {}

    ~DecodeThread() noexcept { stop(); }

    DecodeThread(const DecodeThread&) = delete;
    DecodeThread& operator=(const DecodeThread&) = delete;

    void notifySlotBound(int slotIndex) noexcept {
        if (slotIndex >= 0 && slotIndex < kMaxAudioSlots) {
            activeSlotMask_.fetch_or(static_cast<uint16_t>(1u << slotIndex), std::memory_order_release);
            wake();
        }
    }

    void notifySlotUnbound(int slotIndex) noexcept {
        if (slotIndex >= 0 && slotIndex < kMaxAudioSlots) {
            activeSlotMask_.fetch_and(static_cast<uint16_t>(~(1u << slotIndex)), std::memory_order_release);
        }
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
        lastHeartbeatUs_.store(0, std::memory_order_relaxed);
        watchdogTripped_.store(false, std::memory_order_relaxed);
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

    [[nodiscard]] bool isResponsive() noexcept {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }

        int64_t lastBeat = lastHeartbeatUs_.load(std::memory_order_acquire);
        if (lastBeat == 0) {
            return true;
        }

        int64_t elapsed = nowUs() - lastBeat;
        bool responsive = elapsed < config::thread::kWatchdogTimeoutUs;

        // Trip watchdog on first detection of stall (per-recovery cycle)
        if (!responsive && !watchdogTripped_.load(std::memory_order_acquire)) {
            watchdogTripped_.store(true, std::memory_order_release);
            uint32_t tripCount = watchdogTripCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            MEDIA_LOG_E("DecodeThread watchdog tripped (#%u): no heartbeat for %lldms (timeout=%lldms)",
                        tripCount,
                        static_cast<long long>(elapsed / 1000),
                        static_cast<long long>(config::thread::kWatchdogTimeoutUs / 1000));
        }

        return responsive;
    }

    [[nodiscard]] bool wasWatchdogTripped() const noexcept {
        return watchdogTripped_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t watchdogTripCount() const noexcept {
        return watchdogTripCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept {
        int64_t lastBeat = lastHeartbeatUs_.load(std::memory_order_acquire);
        if (lastBeat == 0) return 0;
        return nowUs() - lastBeat;
    }

    void resetWatchdogState() noexcept {
        watchdogTripped_.store(false, std::memory_order_release);
    }

    using InactivityCallback = void(*)(void* ctx, int slotIndex);

    // Must be called before start() — not safe to call while decode thread is running.
    void setInactivityCallback(ParticipantRegistry& participants,
                               InactivityCallback cb, void* ctx) noexcept {
        if (UNLIKELY(running_.load(std::memory_order_acquire))) {
            MEDIA_LOG_E("setInactivityCallback called while decode thread is running — ignored");
            return;
        }
        inactivityParticipants_ = &participants;
        inactivityCb_ = cb;
        inactivityCtx_ = ctx;
    }

private:
    void petWatchdog() noexcept {
        lastHeartbeatUs_.store(nowUs(), std::memory_order_release);
    }

    void threadLoop() noexcept {
        thread_affinity::setThreadName("OpusDecode");
        thread_affinity::configureCurrentThreadForDecode();

        int64_t startTime = nowUs();
        startTimeUs_.store(startTime, std::memory_order_relaxed);
        petWatchdog();

        int64_t nextHealthLogUs = startTime + config::thread::kHealthLogIntervalUs;
        int consecutiveIdleCycles = 0;
        uint32_t lastWakeVersion = 0;

        MEDIA_LOG_I("DecodeThread: started with watchdog (timeout=%lldms)",
                    static_cast<long long>(config::thread::kWatchdogTimeoutUs / 1000));

        // Log initial health status immediately on startup
        logHealthStatus();

        while (!stopRequested_.load(std::memory_order_acquire)) {
            petWatchdog();

            bool didWork = processAudioSlots();

            if (LIKELY(didWork)) {
                consecutiveIdleCycles = 0;
                lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
                continue;
            }

            consecutiveIdleCycles++;
            int64_t now = nowUs();

            if (UNLIKELY(now >= nextHealthLogUs)) {
                logHealthStatus();
                nextHealthLogUs = now + config::thread::kHealthLogIntervalUs;
            }

            checkInactivity(now);

            int64_t sleepUs;
            if (consecutiveIdleCycles < config::thread::kIdleCycleThreshold) {
                sleepUs = config::thread::kDecodeLoopSleepUs;
            } else {
                // Exponential backoff: double sleep time for each idle cycle beyond threshold,
                // capped at 32x base sleep (16ms max). Reduces CPU usage during silence while
                // maintaining fast wake response when audio arrives.
                int backoffShift = std::min(consecutiveIdleCycles - config::thread::kIdleCycleThreshold, 5);
                sleepUs = config::thread::kDecodeLoopSleepUs << backoffShift;
            }

            // Pet watchdog before sleeping so idle-wait (no pending work)
            // is not misreported as a thread stall. The thread is alive and
            // responsive — it simply has nothing to decode.
            petWatchdog();
            waitForWakeOrTimeout(lastWakeVersion, sleepUs);
            lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
        }

        MEDIA_LOG_I("DecodeThread: stopped after %llds",
                    static_cast<long long>(uptimeUs() / 1000000));
        threadExited_.store(true, std::memory_order_release);
    }

    void waitForWakeOrTimeout(uint32_t lastVersion, int64_t timeoutUs) noexcept {
        // Sleep in short intervals, checking for wake between each.
        // Avoids C++20 atomic::wait() which has no timeout and can block
        // indefinitely if notify_one() is never called (e.g., no incoming
        // frames), causing false watchdog trips.
        constexpr int64_t kSpinIntervalUs = 500;
        int64_t elapsed = 0;

        while (elapsed < timeoutUs) {
            uint32_t current = wakeVersion_.load(std::memory_order_acquire);
            if (current != lastVersion || stopRequested_.load(std::memory_order_relaxed)) {
                return;
            }

            int64_t remaining = timeoutUs - elapsed;
            std::this_thread::sleep_for(std::chrono::microseconds(
                std::min(kSpinIntervalUs, remaining)));
            elapsed += kSpinIntervalUs;
        }
    }

    [[nodiscard]] bool processAudioSlots() noexcept {
        bool didWork = false;

        uint16_t activeMask = activeSlotMask_.load(std::memory_order_acquire);

        if (UNLIKELY(activeMask == 0)) return false;

        while (activeMask) {
            auto i = static_cast<size_t>(std::countr_zero(activeMask));
            uint16_t remainingMask = activeMask & (activeMask - 1);

            AudioDecodeSlot* slot = audioSlots_[i];
            if (UNLIKELY(!slot)) {
                activeMask = remainingMask;
                continue;
            }

            // Prefetch next slot's cache line while processing current slot.
            // Reduces cache miss latency by ~50-100 cycles on ARM64.
            if (remainingMask) {
                auto nextIdx = static_cast<size_t>(std::countr_zero(remainingMask));
                if (AudioDecodeSlot* nextSlot = audioSlots_[nextIdx]) {
                    PREFETCH_READ(nextSlot);
                }
            }

            // Service deferred encoded queue clear from unbind() before any
            // encodedQueue_ access. unbind() defers this to the decode thread
            // to avoid SPSC contract violation (JS thread vs decode thread).
            slot->serviceDeferredClear();

            if (slot->hasPendingDecode()) {
                if (slot->processPendingDecode()) {
                    didWork = true;
                }
            }

            if (slot->needsPLC()) {
                if (slot->generatePLC()) {
                    didWork = true;
                }
            }

            activeMask = remainingMask;
        }

        return didWork;
    }

    void checkInactivity(int64_t now) noexcept {
        if (!inactivityCb_ || !inactivityParticipants_) return;
        if (now - lastInactivityCheckUs_ < config::admission::kInactivityCheckIntervalUs) return;
        lastInactivityCheckUs_ = now;

        uint16_t activeMask = activeSlotMask_.load(std::memory_order_acquire);
        while (activeMask) {
            auto i = static_cast<size_t>(std::countr_zero(activeMask));
            activeMask &= activeMask - 1;

            AudioDecodeSlot* slot = audioSlots_[i];
            if (!slot) continue;

            int boundPid = slot->boundParticipant();
            if (boundPid < 0) continue;

            const Participant* p = inactivityParticipants_->get(boundPid);
            if (!p) continue;

            int64_t inactiveUs = now - p->lastActivityUs();
            if (inactiveUs >= config::admission::kInactivityTimeoutUs) {
                inactivityCb_(inactivityCtx_, static_cast<int>(i));
            }
        }
    }

    void logHealthStatus() noexcept {
        uint64_t totalUnderruns = 0;
        uint64_t totalDropped = 0;
        uint64_t totalDrained = 0;
        uint64_t totalDiscontinuities = 0;
        int boundSlots = 0;

        // Per-slot metrics for detailed breakdown
        struct SlotStats {
            int slotIdx;
            int participantId;
            uint64_t underruns;
            uint64_t drained;
            uint64_t dropped;
            uint64_t discontinuities;
        };
        std::array<SlotStats, kMaxAudioSlots> slotStats{};

        uint16_t activeMask = activeSlotMask_.load(std::memory_order_acquire);

        while (activeMask) {
            size_t i = static_cast<size_t>(std::countr_zero(activeMask));
            activeMask &= activeMask - 1;

            AudioDecodeSlot* slot = audioSlots_[i];
            if (UNLIKELY(!slot)) continue;

            const auto& m = slot->metrics();
            uint64_t underruns = m.underruns.load(std::memory_order_relaxed);
            uint64_t dropped = m.framesDropped.load(std::memory_order_relaxed);
            uint64_t drained = m.framesDrained.load(std::memory_order_relaxed);
            uint64_t discontinuities = m.ptsDiscontinuities.load(std::memory_order_relaxed);

            slotStats[static_cast<size_t>(boundSlots)] = {
                static_cast<int>(i),
                slot->boundParticipant(),
                underruns,
                drained,
                dropped,
                discontinuities
            };

            totalUnderruns += underruns;
            totalDropped += dropped;
            totalDrained += drained;
            totalDiscontinuities += discontinuities;
            boundSlots++;
        }

        bool hasNewIssues = (totalUnderruns > lastLoggedUnderruns_) ||
                           (totalDropped > lastLoggedDropped_) ||
                           (totalDrained > lastLoggedDrained_) ||
                           (totalDiscontinuities > lastLoggedDiscontinuities_);

        lastLoggedUnderruns_ = totalUnderruns;
        lastLoggedDropped_ = totalDropped;
        lastLoggedDrained_ = totalDrained;
        lastLoggedDiscontinuities_ = totalDiscontinuities;

        healthLogCounter_++;
        bool isHeartbeat = (healthLogCounter_ % config::thread::kHealthHeartbeatInterval) == 0;

        if (!hasNewIssues && !isHeartbeat && boundSlots > 0) {
            return;
        }

        // Verbose level: silent by default (min level = Info). Avoids I/O
        // on the high-priority decode thread in production.
        MEDIA_LOG_V("Health: slots=%d underruns=%llu dropped=%llu drained=%llu discontinuities=%llu uptime=%llds",
                    boundSlots,
                    static_cast<unsigned long long>(totalUnderruns),
                    static_cast<unsigned long long>(totalDropped),
                    static_cast<unsigned long long>(totalDrained),
                    static_cast<unsigned long long>(totalDiscontinuities),
                    static_cast<long long>(uptimeUs() / 1000000));

        if (hasNewIssues && boundSlots > 0) {
            for (size_t s = 0; s < static_cast<size_t>(boundSlots); ++s) {
                const auto& ss = slotStats[s];
                if (ss.underruns > 0 || ss.dropped > 0 || ss.drained > 0 || ss.discontinuities > 0) {
                    MEDIA_LOG_V("  slot[%d] pid=%d: underruns=%llu dropped=%llu drained=%llu disc=%llu",
                                ss.slotIdx, ss.participantId,
                                static_cast<unsigned long long>(ss.underruns),
                                static_cast<unsigned long long>(ss.dropped),
                                static_cast<unsigned long long>(ss.drained),
                                static_cast<unsigned long long>(ss.discontinuities));
                }
            }
        }
    }

    std::array<AudioDecodeSlot*, kMaxAudioSlots>& audioSlots_;

    std::thread thread_;
    std::atomic<uint32_t> wakeVersion_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> threadExited_{false};
    std::atomic<bool> wasDetached_{false};
    std::atomic<uint16_t> activeSlotMask_{0};
    static_assert(kMaxAudioSlots <= 16, "activeSlotMask_ is uint16_t — increase width if kMaxAudioSlots > 16");

    std::atomic<int64_t> startTimeUs_{0};

    alignas(config::kCacheLineSize) std::atomic<int64_t> lastHeartbeatUs_{0};
    std::atomic<bool> watchdogTripped_{false};
    std::atomic<uint32_t> watchdogTripCount_{0};

    uint64_t lastLoggedUnderruns_{0};
    uint64_t lastLoggedDropped_{0};
    uint64_t lastLoggedDrained_{0};
    uint64_t lastLoggedDiscontinuities_{0};
    uint32_t healthLogCounter_{0};

    ParticipantRegistry* inactivityParticipants_{nullptr};
    InactivityCallback inactivityCb_{nullptr};
    void* inactivityCtx_{nullptr};
    int64_t lastInactivityCheckUs_{0};
};

}  // namespace media
