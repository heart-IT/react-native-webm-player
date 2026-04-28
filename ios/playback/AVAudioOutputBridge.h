// iOS RemoteIO audio output bridge — platform-specific AudioUnit operations.
// Derives shared state machine, restart loop, and render core from AudioOutputBridgeBase.
#pragma once

#include "../../cpp/common/AudioOutputBridgeBase.h"

// Forward-declare AudioUnit opaque type to avoid AudioToolbox include in header
typedef struct OpaqueAudioComponentInstance* AudioComponentInstance;
typedef AudioComponentInstance AudioUnit;

namespace media {

namespace ios_audio {
    constexpr double kLowLatencyThresholdSec = 0.010;
    constexpr int64_t kCallbackDrainTimeoutUs = 50000;  // 50ms
}

class AVAudioOutputBridge : public AudioOutputBridgeBase<AVAudioOutputBridge> {
    friend class AudioOutputBridgeBase<AVAudioOutputBridge>;
public:
    explicit AVAudioOutputBridge(AudioCallback callback, AudioOutputConfig config = {}) noexcept;
    ~AVAudioOutputBridge() noexcept;

    AVAudioOutputBridge(const AVAudioOutputBridge&) = delete;
    AVAudioOutputBridge& operator=(const AVAudioOutputBridge&) = delete;

    [[nodiscard]] int64_t totalLatencyUs() const noexcept {
        return cachedLatencyUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t actualSampleRate() const noexcept {
        return grantedSampleRate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int lastError() const noexcept {
        return lastError_.load(std::memory_order_relaxed);
    }

    // Called from Audio Unit render callback — must be public for C callback access.
    size_t renderAudio(float* output, size_t frameCount) noexcept;

private:
    // ── CRTP interface for AudioOutputBridgeBase ──

    const char* bridgeName() const noexcept { return "AVAudioOutputBridge"; }
    const char* restartThreadName() const noexcept { return "AVAudioOutRstrt"; }
    int restartDelayMs() const noexcept { return config::audio_output::kRestartDelayMs; }

    void storeLastError(int code) noexcept {
        lastError_.store(code, std::memory_order_relaxed);
    }

    void logRestartError() noexcept {
        int err = lastError_.load(std::memory_order_relaxed);
        if (err != 0) {
            MEDIA_LOG_E("AVAudioOutputBridge: restart triggered (error=%d)", err);
        }
    }

    // ── Platform stream operations (implemented in .mm) ──

    bool createStreamImpl() noexcept;
    bool startStreamImpl() noexcept;
    void destroyStreamImpl() noexcept;
    // Returns false on timeout — caller must decide whether to leak or proceed.
    [[nodiscard]] bool waitForCallbackDrain() noexcept;

    // ── Platform-specific state ──

    AudioUnit audioUnit_ = nullptr;
    std::atomic<bool> audioUnitActive_{false};
    std::atomic<int64_t> cachedLatencyUs_{0};
    std::atomic<int> lastError_{0};

    // Track in-flight callbacks for safe shutdown
    alignas(config::kCacheLineSize) std::atomic<int32_t> callbackInflight_{0};
};

}  // namespace media
