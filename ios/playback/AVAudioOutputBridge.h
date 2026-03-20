// Low-latency audio output using RemoteIO Audio Unit
//
// Uses kAudioUnitSubType_RemoteIO for:
// - Direct render callbacks (deterministic latency vs AVAudioEngine)
// - Parity with Android's AAudio callback model
// - Explicit resampling (48kHz mixer → hardware rate) via AudioResamplerFloat
//
// Threading model:
// - start()/stop() called from JS thread
// - Audio Unit render callback on high-priority audio thread
// - Restart thread for error recovery with exponential backoff
#pragma once

#include "../../cpp/common/ThreadAffinity.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/common/MediaConfig.h"
#include "../../cpp/common/CompilerHints.h"
#include "../../cpp/common/TimedJoin.h"
#include "../../cpp/common/AudioResampler.h"
#include "../../cpp/playback/MediaTypes.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <cstring>
#include <cmath>
#include <array>

// Forward-declare AudioUnit opaque type to avoid AudioToolbox include in header
typedef struct OpaqueAudioComponentInstance* AudioComponentInstance;
typedef AudioComponentInstance AudioUnit;

namespace media {

namespace ios_audio {
    // Maximum sample rate deviation allowed (0.1% tolerance for clock drift)
    constexpr double kSampleRateTolerance = 0.001;
    // Low latency threshold
    constexpr double kLowLatencyThresholdSec = 0.010;
    // Restart delay for iOS (separate from Android AAudio constant)
    constexpr int32_t kRestartDelayMs = 100;
    // Maximum wait for in-flight callbacks during shutdown (microseconds)
    constexpr int64_t kCallbackDrainTimeoutUs = 50000;  // 50ms
    constexpr int64_t kCallbackDrainPollUs = 100;       // 100us poll interval
}

struct AVAudioConfig {
    int32_t sampleRate = config::audio::kSampleRate;
    int32_t channelCount = config::audio::kChannels;
    int32_t framesPerBuffer = config::audio::kFrameSamples;
};

class AVAudioOutputBridge {
public:
    using AudioCallback = std::function<size_t(float* output, size_t frameCount)>;
    using RestartCallback = std::function<void()>;

    static constexpr uint8_t kStateStopped = 0;
    static constexpr uint8_t kStateWarmUp = 1;      // AudioUnit running with silence (hardware hot)
    static constexpr uint8_t kStateActive = 2;
    static constexpr uint8_t kStateShuttingDown = 3;
    static constexpr int kMaxRestartRetries = 5;
    static constexpr int kMaxBackoffMs = 3200;

    explicit AVAudioOutputBridge(AudioCallback callback, AVAudioConfig config = {}) noexcept;
    ~AVAudioOutputBridge() noexcept;

    AVAudioOutputBridge(const AVAudioOutputBridge&) = delete;
    AVAudioOutputBridge& operator=(const AVAudioOutputBridge&) = delete;

    void setRestartCallback(RestartCallback cb) noexcept;

    [[nodiscard]] bool warmUp() noexcept;
    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;

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

    [[nodiscard]] int64_t framesWritten() const noexcept {
        return framesWritten_.load(std::memory_order_relaxed);
    }

    // Cached at createAudioUnit() time — safe to call from any thread (no ObjC).
    [[nodiscard]] int64_t totalLatencyUs() const noexcept {
        return cachedLatencyUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t actualSampleRate() const noexcept {
        return grantedSampleRate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int lastError() const noexcept {
        return lastError_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t restartCount() const noexcept {
        return restartCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] LatencyMode grantedLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isLowLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed) == LatencyMode::LowLatency;
    }

    void scheduleRestart() noexcept;

    // Called from Audio Unit render callback - MUST be public for C callback access
    // Returns samples written. Tracks in-flight count for safe shutdown.
    size_t renderAudio(float* output, size_t frameCount) noexcept;

    // Check if granted sample rate matches requested (within tolerance)
    [[nodiscard]] bool isSampleRateValid() const noexcept {
        return sampleRateValid_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t grantedSampleRate() const noexcept {
        return grantedSampleRate_.load(std::memory_order_relaxed);
    }

private:
    bool createAudioUnit() noexcept;
    bool startAudioUnit() noexcept;
    void destroyAudioUnit() noexcept;
    void restartLoop() noexcept;
    void waitForCallbackDrain() noexcept;

    const AudioCallback callback_;
    const AVAudioConfig config_;

    std::mutex lifecycleMtx_;

    // RemoteIO Audio Unit (AudioComponentInstance)
    AudioUnit audioUnit_ = nullptr;
    // Atomic flag — avoids race on non-atomic audioUnit_
    std::atomic<bool> audioUnitActive_{false};
    // Cached at createAudioUnit() / destroyAudioUnit() — avoids ObjC from arbitrary threads
    std::atomic<int64_t> cachedLatencyUs_{0};

    // Resampler for 48kHz mixer output → hardware rate (e.g., 44.1kHz, BT SCO 16kHz)
    AudioResamplerFloat playbackResampler_;
    // Source buffer for resampler input. Sized for iOS MaxFramesPerSlice (4096) at worst
    // upsample ratio (8kHz→48kHz = 6x): 4096 * 6 = 24576 samples.
    // render callback computes srcFrames = hwFrames * 48kHz / hwRate, clamped to this capacity.
    static constexpr size_t kResampleSrcCapacity = 4096 * 6;
    alignas(16) std::array<float, kResampleSrcCapacity> resampleSrcBuffer_{};

    std::thread restartThread_;
    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> restartThreadExited_{false};


    std::mutex callbackMtx_;
    RestartCallback restartCallback_;

    std::atomic<uint8_t> callbackState_{kStateStopped};
    std::atomic<int64_t> framesWritten_{0};
    std::atomic<int> lastError_{0};
    std::atomic<uint32_t> restartCount_{0};
    std::atomic<int> consecutiveRestartFailures_{0};
    std::atomic<LatencyMode> grantedLatencyMode_{LatencyMode::Unknown};

    // Track in-flight callbacks to prevent use-after-free
    // Incremented at callback entry, decremented at exit. destroyAudioUnit() waits for zero.
    alignas(config::kCacheLineSize) std::atomic<int32_t> callbackInflight_{0};

    // Sample rate validation - fail if iOS grants different rate than expected
    std::atomic<int32_t> grantedSampleRate_{0};
    std::atomic<bool> sampleRateValid_{false};

};

}  // namespace media
