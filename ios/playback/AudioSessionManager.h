#pragma once

#include "../../cpp/common/AudioRouteTypes.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/common/MediaConfig.h"
#include "../../cpp/common/RouteHandler.h"
#include <atomic>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace media {

// Forward declaration - implemented after AudioSessionManager class
int64_t routeLatencyOffsetUs() noexcept;

struct AudioSessionDiagnostics {
    int32_t sessionId = 0;
    AudioRoute route = AudioRoute::Unknown;
    EchoMode echoMode = EchoMode::None;
    AudioFocusState focusState = AudioFocusState::Gain;
    bool voiceModeEnabled = false;
    bool hardwareAecActive = false;
    bool isForeground = true;
    int64_t measuredOutputLatencyUs = 0;
    int64_t effectiveRouteLatencyUs = 0;
};

class AudioSessionManager {
public:
    static AudioSessionManager& instance() noexcept;

    bool initialize() noexcept;
    void shutdown() noexcept;

    void onSessionIdGenerated(int32_t sessionId) noexcept {
        currentSessionId_.store(sessionId, std::memory_order_release);
        MEDIA_LOG_I("AudioSessionManager: sessionId=%d", sessionId);
    }

    [[nodiscard]] int32_t currentSessionId() const noexcept {
        return currentSessionId_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool hasValidSessionId() const noexcept {
        int32_t id = currentSessionId_.load(std::memory_order_acquire);
        return id > 0;
    }

    void setVoiceModeEnabled(bool enable) noexcept {
        voiceModeEnabled_.store(enable, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isVoiceModeEnabled() const noexcept {
        return voiceModeEnabled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isHardwareAecActive() const noexcept {
        EchoMode mode = currentEchoMode_.load(std::memory_order_relaxed);
        return mode == EchoMode::HardwareAEC;
    }

    [[nodiscard]] AudioRoute currentRoute() const noexcept {
        return currentRoute_.load(std::memory_order_relaxed);
    }

    // Detect the current route from AVAudioSession if not yet known.
    // Safe to call before initialize() — only queries OS route state.
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
        return diag;
    }

    // Update measured output latency from AVAudioSession.
    // Called periodically by audio output bridge when latency changes.
    // For Bluetooth routes, we use max(hardcoded, measured) to ensure
    // we never underestimate latency while adapting to actual conditions.
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

    // Returns effective route latency: max(hardcoded codec estimate, measured latency)
    // This ensures we never underestimate while adapting to actual hardware conditions.
    [[nodiscard]] int64_t effectiveRouteLatencyUs() const noexcept {
        AudioRoute route = currentRoute_.load(std::memory_order_acquire);
        int64_t measured = measuredOutputLatencyUs_.load(std::memory_order_acquire);

        switch (route) {
            case AudioRoute::BluetoothA2dp: {
                // iOS doesn't expose A2DP codec info, use conservative fallback
                int64_t effective = std::max(config::a2dp::kLatencyUnknownUs, measured);

                // Log once per A2DP connection (flag reset on route change in detectCurrentRoute)
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

    // Set audio output route (speaker, earpiece, bluetooth)
    // When deviceId is non-empty, targets a specific device by its platform ID.
    // Returns true if route was set successfully
    bool setAudioRoute(AudioRoute route, const std::string& deviceId = "") noexcept;

    // Execute the actual AVAudioSession route change.
    // Does NOT call requestStreamRestart() — the route change notification
    // handles restart decisions via RouteHandler::onRouteDetected().
    bool executeRouteChangePlatform(AudioRoute route, const std::string& deviceId = "") noexcept;

    // Get list of available audio routes (deduplicated by type)
    // Returns vector of AudioRoute values that can be passed to setAudioRoute
    std::vector<AudioRoute> getAvailableAudioRoutes() noexcept;

    // Get list of all available audio devices with names and IDs
    // Unlike getAvailableAudioRoutes(), returns every individual device
    std::vector<AudioDeviceInfo> getAvailableAudioDevices() noexcept;

    // Returns whether speaker is preferred (used by audio bridges when configuring session).
    // When true, DefaultToSpeaker option should be included.
    [[nodiscard]] bool isSpeakerPreferred() const noexcept {
        return speakerPreferred_.load(std::memory_order_acquire);
    }

    void requestStreamRestart() noexcept {
        StreamRestartCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = restartCallback_;
        }
        if (cb) cb();
    }

    // Called from notification handlers
    void onAudioInterruption(bool began, bool shouldResume) noexcept;
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

    [[nodiscard]] bool shouldDuck() const noexcept {
        return focusState_.load(std::memory_order_acquire) == AudioFocusState::LossTransientCanDuck;
    }

    // Returns the platform device ID of the current route (UID of AVAudioSessionPortDescription)
    [[nodiscard]] std::string currentDeviceId() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return currentDeviceId_;
    }

    // Wire drift reset callback into RouteHandler (used by MediaPipelineModule)
    void setDriftResetCallback(std::function<void()> fn) noexcept {
        driftResetCallback_ = std::move(fn);
    }

private:
    AudioSessionManager() = default;
    ~AudioSessionManager();

    AudioSessionManager(const AudioSessionManager&) = delete;
    AudioSessionManager& operator=(const AudioSessionManager&) = delete;

    bool configureAudioSession() noexcept;
    bool configureAudioSessionLocked() noexcept;  // Expects mutex_ held
    void registerNotifications() noexcept;
    void unregisterNotifications() noexcept;
    void detectCurrentRoute() noexcept;
    void routeToBestDevice() noexcept;
    void startDevicePollTimer() noexcept;
    void stopDevicePollTimer() noexcept;
    void pollDeviceListChanges() noexcept;

    // Classify LE Audio output port using the bidirectionality cache when
    // available, falling back to a live availableInputs scan otherwise.
    // Used by getAvailableAudioRoutes() and getAvailableAudioDevices() to
    // stay consistent with detectCurrentRoute()'s cached classification.
    // Caller must NOT hold mutex_. Implemented in AudioSessionManager.mm.
    AudioRoute classifyLeAudioPort(void* uid, void* availableInputs) noexcept;

    std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    bool initializing_{false};  // guarded by mutex_, serializes concurrent first-init callers
    int32_t refCount_{0};

    // Opaque pointer to Objective-C notification observers
    void* routeChangeObserver_{nullptr};
    void* interruptionObserver_{nullptr};
    void* mediaServicesLostObserver_{nullptr};
    void* mediaServicesResetObserver_{nullptr};
    void* backgroundObserver_{nullptr};
    void* foregroundObserver_{nullptr};

    // Serial queue for notification delivery. Using a dedicated queue ensures:
    // 1. Handlers never fire on the audio thread (queue:nil delivers on posting thread)
    // 2. removeObserver: drains the queue, guaranteeing no in-flight handlers after return
    void* notificationQueue_{nullptr};  // NSOperationQueue*
    void* notificationDispatchQueue_{nullptr};  // dispatch_queue_t (serializes notification handlers + RouteHandler access)

    std::atomic<int32_t> currentSessionId_{0};
    std::atomic<bool> voiceModeEnabled_{false};
    std::atomic<AudioRoute> currentRoute_{AudioRoute::Unknown};
    std::atomic<EchoMode> currentEchoMode_{EchoMode::HardwareAEC};
    // Initialized to Gain (iOS has implicit focus when audio session is active).
    // Android initializes to None (requires explicit requestAudioFocus()).
    std::atomic<AudioFocusState> focusState_{AudioFocusState::Gain};
    std::atomic<bool> isForeground_{true};
    std::atomic<bool> speakerPreferred_{true};  // Default to speaker on app start

    // Runtime-measured output latency from AVAudioSession.outputLatency
    // Updated by audio output bridge when latency changes.
    std::atomic<int64_t> measuredOutputLatencyUs_{0};

    // One-shot logging flag for A2DP latency source (reset on route change)
    mutable std::atomic<bool> a2dpLatencyLogged_{false};

    // LE Audio bidirectionality cache — prevents mid-session reclassification
    // when the input port disappears (e.g. headset firmware power optimization).
    // Cleared when route changes away from LE Audio.
    // Protected by mutex_ — accessed from both notification queue and JS thread.
    std::string leAudioCachedUid_;  // guarded by mutex_
    bool leAudioCachedBidirectional_{false};  // guarded by mutex_

    // Current device UID from AVAudioSession (set by detectCurrentRoute)
    std::string currentDeviceId_;  // guarded by mutex_

    // Device poll timer — periodically checks availableInputs for changes
    // that iOS doesn't report via notifications (e.g. BT turned off while
    // not the active route). Runs on the notification serial queue.
    void* devicePollTimer_{nullptr};  // dispatch_source_t
    std::vector<std::string> lastPolledDeviceUIDs_;  // guarded by serial queue (no mutex needed)

    AudioRouteCallback routeCallback_;
    StreamRestartCallback restartCallback_;
    AudioFocusCallback focusCallback_;
    LifecycleCallback lifecycleCallback_;
    std::function<void()> driftResetCallback_;

    // Reactive route handler (replaces RoutingStateMachine)
    RouteHandler routeHandler_;
};

// Returns the route-specific latency offset for jitter buffer calculation.
// For Bluetooth routes, uses the maximum of:
//   1. Hardcoded codec latency estimate (conservative baseline)
//   2. Runtime-measured output latency from AVAudioSession (actual hardware)
// This ensures we never underestimate latency while adapting to actual conditions.
// Thread safety: calls Meyers singleton; safe from any thread after first init on JS thread.
inline int64_t routeLatencyOffsetUs() noexcept {
    return AudioSessionManager::instance().effectiveRouteLatencyUs();
}

}  // namespace media
