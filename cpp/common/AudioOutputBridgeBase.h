// Platform-agnostic audio output bridge: state machine, restart loop, render core.
//
// CRTP template: Derived must implement platform-specific stream operations:
//   bool createStreamImpl() noexcept;
//   bool startStreamImpl() noexcept;
//   void destroyStreamImpl() noexcept;
//
// Threading model:
//   warmUp()/start()/stop() called from JS thread (mutex-protected).
//   renderAudioCore() called from platform audio callback thread (lock-free).
//   Restart thread handles error recovery with exponential backoff.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <cstring>
#include <array>
#include <algorithm>
#include "MediaConfig.h"
#include "MediaLog.h"
#include "MediaTime.h"
#include "ThreadAffinity.h"
#include "TimedJoin.h"
#include "CompilerHints.h"
#include "AudioResampler.h"
#include "playback/MediaTypes.h"

namespace media {

struct AudioOutputConfig {
    int32_t sampleRate = config::audio::kSampleRate;
    int32_t channelCount = config::audio::kChannels;
    int32_t framesPerBuffer = config::audio::kFrameSamples;
};

template<typename Derived>
class AudioOutputBridgeBase {
public:
    using AudioCallback = std::function<size_t(float* output, size_t frameCount)>;
    using RestartCallback = std::function<void()>;

    static constexpr uint8_t kStateStopped = 0;
    static constexpr uint8_t kStateWarmUp = 1;
    static constexpr uint8_t kStateActive = 2;
    static constexpr uint8_t kStateShuttingDown = 3;
    static constexpr int kMaxRestartRetries = 5;
    static constexpr int kMaxBackoffMs = 3200;

    explicit AudioOutputBridgeBase(AudioCallback callback, AudioOutputConfig config) noexcept
        : callback_(std::move(callback))
        , baseConfig_(config) {}

    AudioOutputBridgeBase(const AudioOutputBridgeBase&) = delete;
    AudioOutputBridgeBase& operator=(const AudioOutputBridgeBase&) = delete;

    void setRestartCallback(RestartCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        restartCallback_ = std::move(cb);
    }

    void setInterruptedFlag(std::atomic<bool>* flag) noexcept { interruptedFlag_ = flag; }

    [[nodiscard]] bool warmUp() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        uint8_t expected = kStateStopped;
        if (!callbackState_.compare_exchange_strong(expected, kStateWarmUp,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            uint8_t current = callbackState_.load(std::memory_order_acquire);
            return current == kStateWarmUp || current == kStateActive;
        }

        if (!self()->createStreamImpl()) {
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        restartThreadExited_.store(false, std::memory_order_relaxed);
        restartThread_ = std::thread([this] { restartLoop(); });

        if (!self()->startStreamImpl()) {
            restartRequested_.store(true, std::memory_order_release);
            restartRequested_.notify_one();
            callbackState_.store(kStateShuttingDown, std::memory_order_release);
            timedJoin(restartThread_, restartThreadExited_,
                      config::thread::kThreadJoinTimeoutMs, "AudioBridge::warmUp");
            self()->destroyStreamImpl();
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
        MEDIA_LOG_I("%s: warm-up complete, hardware hot", self()->bridgeName());
        return true;
    }

    [[nodiscard]] bool start() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        uint8_t expected = kStateWarmUp;
        if (callbackState_.compare_exchange_strong(expected, kStateActive,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            MEDIA_LOG_I("%s: warm-up -> active (zero latency transition)", self()->bridgeName());
            return true;
        }
        if (expected == kStateActive) return true;

        expected = kStateStopped;
        if (!callbackState_.compare_exchange_strong(expected, kStateActive,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return false;
        }

        if (!self()->createStreamImpl()) {
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        restartThreadExited_.store(false, std::memory_order_relaxed);
        restartThread_ = std::thread([this] { restartLoop(); });

        if (!self()->startStreamImpl()) {
            restartRequested_.store(true, std::memory_order_release);
            restartRequested_.notify_one();
            callbackState_.store(kStateShuttingDown, std::memory_order_release);
            timedJoin(restartThread_, restartThreadExited_,
                      config::thread::kThreadJoinTimeoutMs, "AudioBridge::start");
            self()->destroyStreamImpl();
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
        return true;
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            uint8_t current = callbackState_.load(std::memory_order_acquire);
            if (current == kStateStopped) return;
            if (current == kStateWarmUp || current == kStateActive) {
                callbackState_.store(kStateShuttingDown, std::memory_order_release);
            } else if (current == kStateShuttingDown) {
                return;
            }
        }

        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();
        // Wake any in-flight backoff sleep in restartLoop so stop() doesn't
        // block up to kMaxBackoffMs (3.2s) waiting for the wake-up.
        {
            std::lock_guard<std::mutex> lk(shutdownMtx_);
            shutdownRequested_ = true;
        }
        shutdownCv_.notify_all();

        timedJoin(restartThread_, restartThreadExited_,
                  config::thread::kThreadJoinTimeoutMs, "AudioBridge::stop");
        self()->destroyStreamImpl();

        callbackState_.store(kStateStopped, std::memory_order_release);
        // Reset shutdown flag so a subsequent start()/warmUp() is not
        // immediately woken on its first sleep.
        {
            std::lock_guard<std::mutex> lk(shutdownMtx_);
            shutdownRequested_ = false;
        }
    }

    [[nodiscard]] bool isActive() const noexcept {
        return callbackState_.load(std::memory_order_acquire) == kStateActive;
    }

    [[nodiscard]] bool isWarmingUp() const noexcept {
        return callbackState_.load(std::memory_order_acquire) == kStateWarmUp;
    }

    [[nodiscard]] bool isRunning() const noexcept {
        uint8_t state = callbackState_.load(std::memory_order_acquire);
        return state == kStateWarmUp || state == kStateActive;
    }

    void pauseToWarmUp() noexcept {
        uint8_t expected = kStateActive;
        callbackState_.compare_exchange_strong(expected, kStateWarmUp,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    void resumeFromWarmUp() noexcept {
        uint8_t expected = kStateWarmUp;
        callbackState_.compare_exchange_strong(expected, kStateActive,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    void scheduleRestart() noexcept {
        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();
    }

    [[nodiscard]] int64_t framesWritten() const noexcept {
        return framesWritten_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t restartCount() const noexcept {
        return restartCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t callbackJitterUs() const noexcept {
        return callbackJitterUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] LatencyMode grantedLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isLowLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed) == LatencyMode::LowLatency;
    }

    [[nodiscard]] bool isSampleRateValid() const noexcept {
        int32_t granted = grantedSampleRate_.load(std::memory_order_relaxed);
        return granted == 0 || granted == baseConfig_.sampleRate;
    }

    [[nodiscard]] int32_t grantedSampleRate() const noexcept {
        return grantedSampleRate_.load(std::memory_order_relaxed);
    }

protected:
    // Call from platform audio callback. Lock-free, RT-safe. Defined in
    // AudioOutputBridgeRender.inl. Handles state checks, jitter tracking,
    // resampling, frame accounting.
    HOT_FUNCTION
    size_t renderAudioCore(float* output, size_t frameCount) noexcept;

    // Derived class sets these during createStreamImpl()
    const AudioCallback callback_;
    const AudioOutputConfig baseConfig_;

    std::mutex lifecycleMtx_;
    std::mutex callbackMtx_;

    std::atomic<uint8_t> callbackState_{kStateStopped};
    std::atomic<int64_t> framesWritten_{0};
    std::atomic<uint32_t> restartCount_{0};
    std::atomic<int> consecutiveRestartFailures_{0};
    std::atomic<LatencyMode> grantedLatencyMode_{LatencyMode::Unknown};
    std::atomic<int32_t> grantedSampleRate_{0};
    std::atomic<bool>* interruptedFlag_{nullptr};

    AudioResamplerFloat playbackResampler_;
    static constexpr size_t kResampleSrcCapacity = 4096 * 6;
    alignas(16) std::array<float, kResampleSrcCapacity> resampleSrcBuffer_{};

private:
    Derived* self() noexcept { return static_cast<Derived*>(this); }
    const Derived* self() const noexcept { return static_cast<const Derived*>(this); }

    // Defined in AudioOutputBridgeRestart.inl
    void restartLoop() noexcept;
    void sleepCancellable(int ms) noexcept;

    RestartCallback restartCallback_;
    std::thread restartThread_;
    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> restartThreadExited_{false};

    std::mutex shutdownMtx_;
    std::condition_variable shutdownCv_;
    bool shutdownRequested_{false};

    std::atomic<int64_t> lastCallbackTimeUs_{0};
    std::atomic<int64_t> callbackJitterUs_{0};
};

}  // namespace media

// Out-of-class template member definitions. Included after the class body so
// each .inl has full visibility of state and types.
#include "AudioOutputBridgeRender.inl"
#include "AudioOutputBridgeRestart.inl"
