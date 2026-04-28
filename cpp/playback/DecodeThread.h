// Audio decode thread — delegates lifecycle to WorkerThread<T>.
#pragma once

#include <atomic>
#include "common/MediaConfig.h"
#include "common/MediaLog.h"
#include "common/MediaTime.h"
#include "common/WorkerThread.h"
#include "MediaTypes.h"
#include "AudioDecodeChannel.h"

namespace media {

// Processor: performs audio decode work each cycle.
class AudioDecodeProcessor {
public:
    explicit AudioDecodeProcessor(AudioDecodeChannel& channel) noexcept
        : audio_(channel) {}

    [[nodiscard]] bool process() noexcept {
        if (!channelActive_.load(std::memory_order_acquire)) return false;
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

        // Periodic health logging (on idle cycles)
        if (!didWork) {
            int64_t now = nowUs();
            if (UNLIKELY(now >= nextHealthLogUs_)) {
                logHealthStatus();
                nextHealthLogUs_ = now + config::thread::kHealthLogIntervalUs;
            }
        }

        return didWork;
    }

    void onDetached() noexcept {}

    void notifyActive() noexcept {
        channelActive_.store(true, std::memory_order_release);
    }

    AudioDecodeChannel& channel() noexcept { return audio_; }

private:
    void logHealthStatus() noexcept {
        if (!channelActive_.load(std::memory_order_acquire)) return;

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

        if (!hasNewIssues && !isHeartbeat) return;

        MEDIA_LOG_V("Health: underruns=%llu dropped=%llu drained=%llu discontinuities=%llu",
                    static_cast<unsigned long long>(underruns),
                    static_cast<unsigned long long>(dropped),
                    static_cast<unsigned long long>(drained),
                    static_cast<unsigned long long>(discontinuities));
    }

    AudioDecodeChannel& audio_;
    std::atomic<bool> channelActive_{false};

    int64_t nextHealthLogUs_{0};
    uint64_t lastLoggedUnderruns_{0};
    uint64_t lastLoggedDropped_{0};
    uint64_t lastLoggedDrained_{0};
    uint64_t lastLoggedDiscontinuities_{0};
    uint32_t healthLogCounter_{0};
};

// Facade: preserves the public API used by MediaSessionBase.
class DecodeThread {
public:
    explicit DecodeThread(AudioDecodeChannel& channel) noexcept
        : processor_(channel)
        , worker_(processor_, {"OpusDecode",
                               config::thread::kDecodeLoopSleepUs,
                               5, true, false}) {}

    ~DecodeThread() noexcept = default;

    DecodeThread(const DecodeThread&) = delete;
    DecodeThread& operator=(const DecodeThread&) = delete;

    void notifyActive() noexcept {
        processor_.notifyActive();
        worker_.wake();
    }

    [[nodiscard]] bool start() noexcept { return worker_.start(); }
    void stop() noexcept { worker_.stop(); }
    void wake() noexcept { worker_.wake(); }

    [[nodiscard]] bool isRunning() const noexcept { return worker_.isRunning(); }
    [[nodiscard]] bool wasDetached() const noexcept { return worker_.wasDetached(); }
    [[nodiscard]] bool isResponsive() noexcept { return worker_.isResponsive(); }
    [[nodiscard]] int64_t uptimeUs() const noexcept { return worker_.uptimeUs(); }
    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept { return worker_.timeSinceLastHeartbeatUs(); }

    [[nodiscard]] bool wasWatchdogTripped() const noexcept { return worker_.wasWatchdogTripped(); }
    [[nodiscard]] uint32_t watchdogTripCount() const noexcept { return worker_.watchdogTripCount(); }
    void resetWatchdogState() noexcept { worker_.resetWatchdogState(); }

    void setHealthWatchdog(std::unique_ptr<HealthWatchdog> wd) noexcept { worker_.setHealthWatchdog(std::move(wd)); }
    void resetHealthWindow() noexcept { worker_.resetHealthWindow(); }

private:
    AudioDecodeProcessor processor_;
    WorkerThread<AudioDecodeProcessor> worker_;
};

}  // namespace media
