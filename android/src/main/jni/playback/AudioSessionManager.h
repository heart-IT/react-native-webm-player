#pragma once

#include <jni.h>
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <vector>
#include "common/AudioRouteTypes.h"
#include "common/MediaLog.h"
#include "common/MediaConfig.h"
#include "common/RouteHandler.h"

namespace media {

// Forward declaration - implemented after AudioSessionManager class
int64_t routeLatencyOffsetUs() noexcept;

struct AudioSessionDiagnostics {
    int32_t sessionId = 0;
    AudioRoute route = AudioRoute::Unknown;
    EchoMode echoMode = EchoMode::None;
    AudioFocusState focusState = AudioFocusState::None;
    bool voiceModeEnabled = false;
    bool hardwareAecActive = false;
    bool isForeground = true;
    int64_t routeLatencyUs = 0;
    int64_t measuredOutputLatencyUs = 0;
    int64_t effectiveRouteLatencyUs = 0;
};

class AudioSessionManager {
public:
    static AudioSessionManager& instance() noexcept;

    bool initialize() noexcept;

    void onSessionIdGenerated(int32_t sessionId) noexcept {
        currentSessionId_.store(sessionId, std::memory_order_release);
        MEDIA_LOG_I("AudioSessionManager: sessionId=%d", sessionId);
    }

    [[nodiscard]] int32_t currentSessionId() const noexcept {
        return currentSessionId_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool hasValidSessionId() const noexcept {
        int32_t id = currentSessionId_.load(std::memory_order_acquire);
        return id != AAUDIO_SESSION_ID_ALLOCATE && id > 0;
    }

    void setVoiceModeEnabled(bool enable) noexcept {
        voiceModeEnabled_.store(enable, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isVoiceModeEnabled() const noexcept {
        return voiceModeEnabled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isHardwareAecActive() const noexcept {
        EchoMode mode = currentEchoMode_.load(std::memory_order_relaxed);
        return mode == EchoMode::HardwareAEC && hasValidSessionId();
    }

    [[nodiscard]] AudioRoute currentRoute() const noexcept {
        return currentRoute_.load(std::memory_order_relaxed);
    }

    // Detect the current route via JNI if not yet known.
    // Safe to call before startSession() — queries AudioManager directly.
    void ensureRouteDetected() noexcept;

    [[nodiscard]] EchoMode currentEchoMode() const noexcept {
        return currentEchoMode_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] AudioSessionDiagnostics diagnostics() const noexcept {
        AudioSessionDiagnostics diag;
        diag.sessionId = currentSessionId_.load(std::memory_order_acquire);
        diag.route = currentRoute_.load(std::memory_order_acquire);
        diag.echoMode = currentEchoMode_.load(std::memory_order_acquire);
        diag.focusState = focusState_.load(std::memory_order_acquire);
        diag.voiceModeEnabled = voiceModeEnabled_.load(std::memory_order_acquire);
        diag.hardwareAecActive = isHardwareAecActive();
        diag.isForeground = isForeground_.load(std::memory_order_acquire);
        diag.measuredOutputLatencyUs = measuredOutputLatencyUs_.load(std::memory_order_acquire);
        diag.effectiveRouteLatencyUs = effectiveRouteLatencyUs();
        diag.routeLatencyUs = diag.effectiveRouteLatencyUs;
        return diag;
    }

    // Update measured output latency from AAudio stream.
    // Called when stream opens or restarts.
    void updateMeasuredOutputLatency(int64_t latencyUs) noexcept {
        int64_t old = measuredOutputLatencyUs_.exchange(latencyUs, std::memory_order_acq_rel);
        if (old != latencyUs && isBluetoothRoute(currentRoute_.load(std::memory_order_acquire))) {
            MEDIA_LOG_I("AudioSessionManager: measured output latency %lldms -> %lldms",
                        static_cast<long long>(old / 1000),
                        static_cast<long long>(latencyUs / 1000));
        }
    }

    [[nodiscard]] int64_t measuredOutputLatencyUs() const noexcept {
        return measuredOutputLatencyUs_.load(std::memory_order_acquire);
    }

    // Returns effective route latency: uses measured latency or conservative defaults.
    // A2DP codec detection is not available via public Android APIs, so we use
    // conservative SBC latency estimates (~150ms) for all A2DP connections.
    [[nodiscard]] int64_t effectiveRouteLatencyUs() const noexcept {
        AudioRoute route = currentRoute_.load(std::memory_order_acquire);
        int64_t measured = measuredOutputLatencyUs_.load(std::memory_order_acquire);

        switch (route) {
            case AudioRoute::BluetoothA2dp: {
                int64_t effective = std::max(config::a2dp::kLatencyUnknownUs, measured);

                if (!a2dpLatencyLogged_.exchange(true, std::memory_order_relaxed)) {
                    if (measured > config::a2dp::kLatencyUnknownUs) {
                        MEDIA_LOG_I("A2DP latency: using measured %lldms (> conservative %lldms)",
                                    static_cast<long long>(measured / 1000),
                                    static_cast<long long>(config::a2dp::kLatencyUnknownUs / 1000));
                    } else {
                        MEDIA_LOG_I("A2DP latency: using conservative %lldms fallback "
                                    "(codec detection unavailable, measured=%lldms)",
                                    static_cast<long long>(config::a2dp::kLatencyUnknownUs / 1000),
                                    static_cast<long long>(measured / 1000));
                    }
                }
                return effective;
            }
            case AudioRoute::BluetoothSco:
                // SCO has lower latency than A2DP, use measured if available
                return measured > 0 ? measured : 0;
            default:
                return 0;
        }
    }

    void setRouteCallback(AudioRouteCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        routeCallback_ = std::move(cb);
    }

    void setRestartCallback(StreamRestartCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        restartCallback_ = std::move(cb);
    }

    void setFocusCallback(AudioFocusCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        focusCallback_ = std::move(cb);
    }

    void setLifecycleCallback(LifecycleCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        lifecycleCallback_ = std::move(cb);
    }

    // Set audio output route. Implemented in audiosession_jni.cpp
    bool setAudioRoute(AudioRoute route, const std::string& deviceId = "") noexcept;

    // Execute the actual route change via JNI. Implemented in audiosession_jni.cpp
    bool executeRouteChangePlatform(AudioRoute route, const std::string& deviceId = "") noexcept;

    // Get list of available audio routes via JNI call to AudioSessionBridge.getAvailableAudioRoutes()
    // Implemented in audiosession_jni.cpp
    std::vector<AudioRoute> getAvailableAudioRoutes() noexcept;

    // Get list of all available audio devices with names and IDs
    // Implemented in audiosession_jni.cpp
    std::vector<AudioDeviceInfo> getAvailableAudioDevices() noexcept;

    void requestStreamRestart() noexcept {
        StreamRestartCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = restartCallback_;
        }
        if (cb) cb();
    }

    void onAudioFocusChanged(AudioFocusState newState) noexcept;
    void onLifecycleChanged(bool foreground) noexcept;

    [[nodiscard]] AudioFocusState currentFocusState() const noexcept {
        return focusState_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isForeground() const noexcept {
        return isForeground_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool canPlayAudio() const noexcept {
        AudioFocusState focus = focusState_.load(std::memory_order_acquire);
        return focus == AudioFocusState::Gain ||
               focus == AudioFocusState::LossTransientCanDuck;
    }

    [[nodiscard]] bool canCapture() const noexcept {
        return focusState_.load(std::memory_order_acquire) == AudioFocusState::Gain;
    }

    [[nodiscard]] std::string currentDeviceId() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return currentDeviceId_;
    }

    [[nodiscard]] bool shouldDuck() const noexcept {
        return focusState_.load(std::memory_order_acquire) == AudioFocusState::LossTransientCanDuck;
    }

    // Called from Kotlin via JNI when route changes.
    void onAudioRouteChanged(AudioRoute newRoute, EchoMode newEchoMode,
                             const std::string& deviceId) noexcept;

    // Called from Kotlin via JNI when devices are added/removed.
    void onDeviceConnected() noexcept;
    void onDeviceDisconnected() noexcept;

    // Wire drift reset callback into RouteHandler (used by MediaPipelineModule)
    void setDriftResetCallback(std::function<void()> fn) noexcept {
        driftResetCallback_ = std::move(fn);
    }

    void shutdown([[maybe_unused]] JNIEnv* env) noexcept;

private:
    AudioSessionManager() = default;
    ~AudioSessionManager();

    AudioSessionManager(const AudioSessionManager&) = delete;
    AudioSessionManager& operator=(const AudioSessionManager&) = delete;

    std::mutex mutex_;
    bool initialized_{false};

    std::atomic<int32_t> currentSessionId_{AAUDIO_SESSION_ID_ALLOCATE};
    std::atomic<bool> voiceModeEnabled_{false};
    std::atomic<AudioRoute> currentRoute_{AudioRoute::Unknown};
    std::atomic<EchoMode> currentEchoMode_{EchoMode::HardwareAEC};
    // Initialized to None (Android requires explicit requestAudioFocus()).
    // iOS initializes to Gain (implicit focus when audio session is active).
    std::atomic<AudioFocusState> focusState_{AudioFocusState::None};
    std::atomic<bool> isForeground_{true};

    // Runtime-measured output latency from AAudio stream
    std::atomic<int64_t> measuredOutputLatencyUs_{0};

    // One-shot logging flag for A2DP latency source (reset on route change)
    mutable std::atomic<bool> a2dpLatencyLogged_{false};

    // Android device ID from AudioDeviceInfo.getId(), stored as string for
    // consistency with iOS (AVAudioSessionPortDescription.UID). Guarded by mutex_.
    std::string currentDeviceId_;

    AudioRouteCallback routeCallback_;
    StreamRestartCallback restartCallback_;
    AudioFocusCallback focusCallback_;
    LifecycleCallback lifecycleCallback_;
    std::function<void()> driftResetCallback_;

    // Reactive route handler (replaces RoutingStateMachine)
    // Serialized by routingMutex_ — JNI callbacks may race.
    std::mutex routingMutex_;
    RouteHandler routeHandler_;
};

// Returns the route-specific latency offset for jitter buffer calculation.
// For Bluetooth A2DP, uses conservative latency estimates since codec detection
// requires hidden Android APIs. Uses runtime-measured latency when available.
inline int64_t routeLatencyOffsetUs() noexcept {
    return AudioSessionManager::instance().effectiveRouteLatencyUs();
}

}  // namespace media
