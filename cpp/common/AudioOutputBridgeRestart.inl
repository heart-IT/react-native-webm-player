// Restart-loop worker thread for AudioOutputBridgeBase: handles platform
// stream errors with exponential backoff, transitions to fatal state after
// kMaxRestartRetries failures, fires the user-supplied restart callback on
// success or fatal exit.
//
// Include-only-from: AudioOutputBridgeBase.h. Out-of-class template member
// definitions; included after the class body so the full type is visible.
#pragma once

namespace media {

template<typename Derived>
void AudioOutputBridgeBase<Derived>::restartLoop() noexcept {
    thread_affinity::setThreadName(self()->restartThreadName());
    thread_affinity::configureCurrentThreadForWorker();

    while (callbackState_.load(std::memory_order_acquire) != kStateShuttingDown) {
        while (!restartRequested_.load(std::memory_order_acquire)) {
            if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                restartThreadExited_.store(true, std::memory_order_release);
                return;
            }
            restartRequested_.wait(false, std::memory_order_relaxed);
            if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                restartThreadExited_.store(true, std::memory_order_release);
                return;
            }
        }

        if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) break;
        restartRequested_.store(false, std::memory_order_release);

        // Log deferred error
        self()->logRestartError();

        uint8_t state = callbackState_.load(std::memory_order_acquire);
        if (state != kStateActive && state != kStateWarmUp) {
            if (state == kStateStopped) {
                RestartCallback fatalCb;
                { std::lock_guard<std::mutex> lk(callbackMtx_); fatalCb = restartCallback_; }
                if (fatalCb) fatalCb();
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            self()->destroyStreamImpl();
        }

        sleepCancellable(self()->restartDelayMs());
        if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) break;

        {
            std::unique_lock<std::mutex> lk(lifecycleMtx_);

            state = callbackState_.load(std::memory_order_acquire);
            if (state != kStateActive && state != kStateWarmUp) continue;

            if (!self()->createStreamImpl() || !self()->startStreamImpl()) {
                int failures = consecutiveRestartFailures_.fetch_add(1, std::memory_order_relaxed) + 1;

                if (failures >= kMaxRestartRetries) {
                    MEDIA_LOG_E("%s: max restart retries (%d) exceeded, stopping",
                                self()->bridgeName(), kMaxRestartRetries);
                    self()->storeLastError(-20);
                    callbackState_.store(kStateStopped, std::memory_order_release);

                    RestartCallback failCb;
                    { std::lock_guard<std::mutex> cbLk(callbackMtx_); failCb = restartCallback_; }
                    lk.unlock();
                    if (failCb) failCb();
                    break;
                }

                int backoffMs = std::min(self()->restartDelayMs() << failures, kMaxBackoffMs);
                MEDIA_LOG_W("%s: restart failed (%d/%d), backoff %dms",
                            self()->bridgeName(), failures, kMaxRestartRetries, backoffMs);
                lk.unlock();
                sleepCancellable(backoffMs);
                if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) break;
                continue;
            }

            consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
            restartCount_.fetch_add(1, std::memory_order_relaxed);
            MEDIA_LOG_I("%s: restart #%u successful", self()->bridgeName(),
                        restartCount_.load(std::memory_order_relaxed));
        }

        RestartCallback cb;
        { std::lock_guard<std::mutex> lk(callbackMtx_); cb = restartCallback_; }
        if (cb) cb();
    }
    restartThreadExited_.store(true, std::memory_order_release);
}

// Sleep up to ms milliseconds, but wake immediately on shutdown so stop()
// doesn't block waiting for an exponential backoff to expire.
template<typename Derived>
void AudioOutputBridgeBase<Derived>::sleepCancellable(int ms) noexcept {
    if (ms <= 0) return;
    std::unique_lock<std::mutex> lk(shutdownMtx_);
    shutdownCv_.wait_for(lk, std::chrono::milliseconds(ms),
                         [this]() { return shutdownRequested_; });
}

}  // namespace media
