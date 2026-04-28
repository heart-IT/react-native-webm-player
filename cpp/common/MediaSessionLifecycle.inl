// Lifecycle template-method definitions for MediaSessionBase: start, stop,
// warmUp, restartAudioOutput, checkAudioOutputHealth.
//
// Include-only-from: MediaSessionBase.h. Definitions are out-of-class
// template members; included after the class body so the full type is
// visible.
#pragma once

namespace media {

template<typename PlatformTraits>
[[nodiscard]] bool MediaSessionBase<PlatformTraits>::start() noexcept {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    SessionState expected = SessionState::Stopped;
    if (!state_.compare_exchange_strong(expected, SessionState::Starting,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return expected == SessionState::Running;
    }

    startTimeUs_ = nowUs();
    MEDIA_LOG_I("MediaSession starting");

    auto& mgr = PlatformTraits::managerInstance();
    if (!sessionMgr_.load(std::memory_order_acquire)) {
        mgr.initialize();
    }
    sessionMgr_.store(&mgr, std::memory_order_release);

    audio_.setDecoder(std::make_unique<OpusDecoderAdapter>());

    if (!decodeThread_.start()) {
        MEDIA_LOG_E("MediaSession::start failed: decode thread start failed");
        sessionMgr_.store(nullptr, std::memory_order_release);
        PlatformTraits::shutdownManager();
        state_.store(SessionState::Stopped, std::memory_order_release);
        return false;
    }

    if (config_.enableAudioOutput) {
        bool audioPreWarmed = audioOutput_ && audioOutput_->isWarmingUp();

        if (!audioOutput_) {
            audioOutput_ = std::make_unique<Bridge>(
                [this](float* output, size_t frames) { return onAudioCallback(output, frames); },
                config_.audioConfig
            );
            audioOutput_->setRestartCallback([this] { decodeThread_.wake(); });
            audioOutput_->setInterruptedFlag(PlatformTraits::managerInstance().interruptedFlagPtr());
        }

        // Wire the session-level restart callback (iOS interruption-end with
        // shouldResume, Android focus regain, AVAudioSessionMediaServicesWereReset).
        // The bridge-level setRestartCallback above only wakes the decode thread
        // on bridge-internal restart success — it does NOT trigger a restart from
        // an OS-level audio-session event. This callback bridges that gap.
        // Liveness: weakOwner.lock() pins the module shared_ptr for the entire
        // body, so `this` cannot dangle while the callback runs.
        {
            std::weak_ptr<void> weakOwner = weakOwner_;
            PlatformTraits::managerInstance().setRestartCallback([this, weakOwner]() noexcept {
                auto owner = weakOwner.lock();
                if (!owner) return;
                std::lock_guard<std::mutex> cbLk(lifecycleMtx_);
                if (state_.load(std::memory_order_acquire) == SessionState::Running) {
                    restartAudioOutput();
                }
            });
        }

        if (!audioOutput_->start()) {
            MEDIA_LOG_W("MediaSession::start: audio output failed, will recover on route change");
            audioOutput_->stop();
            audioOutput_.reset();
        }

        if (audioPreWarmed) {
            MEDIA_LOG_I("MediaSession: audio transitioned from warm-up to active (zero latency)");
        }
    }

    state_.store(SessionState::Running, std::memory_order_release);

    if (config_.enableAudioOutput && !audioOutput_) {
        MEDIA_LOG_I("MediaSession: audio output null after start, attempting recovery");
        restartAudioOutput();
    }

    MEDIA_LOG_I("MediaSession started successfully");
    return true;
}

template<typename PlatformTraits>
void MediaSessionBase<PlatformTraits>::stop() noexcept {
    // Clear the AudioSessionManager session-restart callback BEFORE acquiring
    // lifecycleMtx_. An in-flight callback locks the same mutex, so dropping it
    // here lets any future OS-level audio-session event short-circuit at the
    // singleton's null-check instead of queuing behind us. Idempotent if start()
    // never wired one.
    if (auto* mgr = sessionMgr_.load(std::memory_order_acquire)) {
        mgr->setRestartCallback(nullptr);
    }

    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    SessionState current = state_.load(std::memory_order_acquire);
    if (current == SessionState::Stopped) {
        if (sessionMgr_.load(std::memory_order_acquire)) {
            sessionMgr_.store(nullptr, std::memory_order_release);
            PlatformTraits::shutdownManager();
        }
        return;
    }

    if (!state_.compare_exchange_strong(current, SessionState::Stopping,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;
    }

    MEDIA_LOG_I("MediaSession stopping");

    if (audioOutput_) {
        audioOutput_->stop();
        audioOutput_.reset();
    }

    decodeThread_.stop();
    audio_.deactivate();
    jitter_.reset();

    sessionMgr_.store(nullptr, std::memory_order_release);
    PlatformTraits::shutdownManager();

    state_.store(SessionState::Stopped, std::memory_order_release);
    MEDIA_LOG_I("MediaSession stopped");
}

template<typename PlatformTraits>
[[nodiscard]] bool MediaSessionBase<PlatformTraits>::warmUp() noexcept {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    if (!sessionMgr_.load(std::memory_order_acquire)) {
        auto& mgr = PlatformTraits::managerInstance();
        mgr.initialize();
        sessionMgr_.store(&mgr, std::memory_order_release);
    }

    if (!config_.enableAudioOutput) return true;

    if (audioOutput_ && audioOutput_->isRunning()) return true;

    if (!audioOutput_) {
        audioOutput_ = std::make_unique<Bridge>(
            [this](float* output, size_t frames) { return onAudioCallback(output, frames); },
            config_.audioConfig
        );
        audioOutput_->setInterruptedFlag(PlatformTraits::managerInstance().interruptedFlagPtr());
    }

    if (!audioOutput_->warmUp()) {
        MEDIA_LOG_E("MediaSession::warmUp failed: audio output warm-up failed");
        return false;
    }

    MEDIA_LOG_I("MediaSession: audio hardware warm-up complete");
    return true;
}

template<typename PlatformTraits>
void MediaSessionBase<PlatformTraits>::checkAudioOutputHealth() noexcept {
    if (!config_.enableAudioOutput) return;

    int64_t now = nowUs();
    if (now - lastAudioHealthCheckUs_ < config::thread::kAudioOutputCheckIntervalUs) return;
    lastAudioHealthCheckUs_ = now;

    if (!audioOutput_ || !audioOutput_->isRunning()) {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        if (state_.load(std::memory_order_acquire) != SessionState::Running) return;
        if (audioOutput_ && audioOutput_->isRunning()) return;

        MEDIA_LOG_W("MediaSession: audio output not running, attempting recovery");
        restartAudioOutput();
    }
}

template<typename PlatformTraits>
void MediaSessionBase<PlatformTraits>::restartAudioOutput() noexcept {
    if (!config_.enableAudioOutput) return;

    if (!audioOutput_) {
        audioOutput_ = std::make_unique<Bridge>(
            [this](float* out, size_t frames) { return onAudioCallback(out, frames); },
            config_.audioConfig);
        audioOutput_->setRestartCallback([this] { decodeThread_.wake(); });
        audioOutput_->setInterruptedFlag(PlatformTraits::managerInstance().interruptedFlagPtr());
    }

    if (!audioOutput_->start()) {
        MEDIA_LOG_E("MediaSession: failed to restart audio output");
    }
}

}  // namespace media
