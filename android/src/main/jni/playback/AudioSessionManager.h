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

struct AudioSessionDiagnostics {
    AudioRoute route = AudioRoute::Unknown;
};

class AudioSessionManager {
public:
    static AudioSessionManager& instance() noexcept;

    bool initialize() noexcept;

    [[nodiscard]] AudioRoute currentRoute() const noexcept {
        return currentRoute_.load(std::memory_order_relaxed);
    }

    // Detect the current route via JNI if not yet known.
    // Safe to call before startSession() — queries AudioManager directly.
    void ensureRouteDetected() noexcept;

    [[nodiscard]] AudioSessionDiagnostics diagnostics() const noexcept {
        AudioSessionDiagnostics diag;
        diag.route = currentRoute_.load(std::memory_order_acquire);
        return diag;
    }

    void setRouteCallback(AudioRouteCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        routeCallback_ = std::move(cb);
    }

    void setRestartCallback(StreamRestartCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        restartCallback_ = std::move(cb);
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

    [[nodiscard]] std::string currentDeviceId() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return currentDeviceId_;
    }

    // Called from Kotlin via JNI when route changes.
    void onAudioRouteChanged(AudioRoute newRoute, const std::string& deviceId) noexcept;

    // Wire drift reset callback into RouteHandler (used by MediaPipelineModule)
    void setDriftResetCallback(std::function<void()> fn) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        driftResetCallback_ = std::move(fn);
    }

    void setAudioFocusCallback(AudioFocusCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        audioFocusCallback_ = std::move(cb);
    }

    void onAudioFocusChanged(int focusChange) noexcept;

    void shutdown([[maybe_unused]] JNIEnv* env) noexcept;

private:
    AudioSessionManager() = default;
    ~AudioSessionManager();

    AudioSessionManager(const AudioSessionManager&) = delete;
    AudioSessionManager& operator=(const AudioSessionManager&) = delete;

    std::mutex mutex_;
    bool initialized_{false};
    int32_t refCount_{0};

    std::atomic<AudioRoute> currentRoute_{AudioRoute::Unknown};

    // Android device ID from AudioDeviceInfo.getId(), stored as string for
    // consistency with iOS (AVAudioSessionPortDescription.UID). Guarded by mutex_.
    std::string currentDeviceId_;

    AudioRouteCallback routeCallback_;
    StreamRestartCallback restartCallback_;
    std::function<void()> driftResetCallback_;
    AudioFocusCallback audioFocusCallback_;

    // Reactive route handler (replaces RoutingStateMachine)
    // Serialized by routingMutex_ — JNI callbacks may race.
    std::mutex routingMutex_;
    RouteHandler routeHandler_;
};

}  // namespace media
