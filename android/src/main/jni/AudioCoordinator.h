#pragma once

#include <mutex>
#include <memory>
#include <functional>
#include <atomic>
#include "playback/AudioSessionManager.h"
#include "common/MediaLog.h"

// JNI upcalls for session lifecycle (defined in audiosession_jni.cpp)
extern void jniStartSession();
extern void jniStopSession();

namespace media {

class MediaSession;

class AudioCoordinator {
public:
    static AudioCoordinator& instance() noexcept {
        static AudioCoordinator coordinator;
        return coordinator;
    }

    void setPlayback(MediaSession*) noexcept {
        bool wasEmpty;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            wasEmpty = !playbackValid_;
            playbackValid_ = true;
        }
        if (wasEmpty) jniStartSession();
        MEDIA_LOG_I("AudioCoordinator: playback registered");
    }

    void clearPlayback() noexcept {
        bool nowEmpty;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            playbackValid_ = false;
            playbackRestartFn_ = nullptr;
            playbackPauseFn_ = nullptr;
            nowEmpty = true;
        }
        if (nowEmpty) jniStopSession();
    }

    void setPlaybackRestartFn(std::function<void()> fn) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        playbackRestartFn_ = std::move(fn);
    }

    void setPlaybackPauseFn(std::function<void(bool)> fn) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        playbackPauseFn_ = std::move(fn);
    }

    void initialize() noexcept;

    void shutdown() noexcept {
        AudioSessionManager::instance().setRestartCallback(nullptr);
        AudioSessionManager::instance().setFocusCallback(nullptr);

        std::lock_guard<std::mutex> lk(mutex_);
        playbackValid_ = false;
        playbackRestartFn_ = nullptr;
        playbackPauseFn_ = nullptr;
        initialized_.store(false, std::memory_order_release);

        MEDIA_LOG_I("AudioCoordinator shutdown");
    }

private:
    AudioCoordinator() = default;

    void onRouteChangeRestart() noexcept {
        std::function<void()> playbackFn;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (playbackValid_) {
                playbackFn = playbackRestartFn_;
            }
        }

        MEDIA_LOG_I("AudioCoordinator: route change restart");

        if (playbackFn) playbackFn();
    }

    void onAudioFocusChanged(AudioFocusState state) noexcept {
        uint32_t seq = focusSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
        MEDIA_LOG_I("AudioCoordinator: focus state=%d seq=%u", static_cast<int>(state), seq);

        std::function<void(bool)> playbackPause;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            playbackPause = playbackPauseFn_;
        }

        auto isCurrentSequence = [this, seq]() {
            return focusSequence_.load(std::memory_order_acquire) == seq;
        };

        switch (state) {
            case AudioFocusState::Gain:
                if (playbackPause && isCurrentSequence()) playbackPause(false);
                break;

            case AudioFocusState::Loss:
            case AudioFocusState::LossTransient:
                if (playbackPause && isCurrentSequence()) playbackPause(true);
                break;

            case AudioFocusState::LossTransientCanDuck:
                break;

            default:
                break;
        }
    }

    mutable std::mutex mutex_;

    std::function<void()> playbackRestartFn_;
    std::function<void(bool)> playbackPauseFn_;
    std::atomic<bool> initialized_{false};
    bool playbackValid_{false};
    std::atomic<uint32_t> focusSequence_{0};
};

}  // namespace media
