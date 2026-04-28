// iOS audio session: AVAudioSession configuration, route change notifications,
// and device enumeration. Routes events to the shared C++ RouteHandler.
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

struct AudioSessionDiagnostics {
    AudioRoute route = AudioRoute::Unknown;
};

class AudioSessionManager {
public:
    static AudioSessionManager& instance() noexcept;

    bool initialize() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] AudioRoute currentRoute() const noexcept {
        return currentRoute_.load(std::memory_order_relaxed);
    }

    // RT-safe: atomic load only, no locks. Used by AVAudioOutputBridge render callback.
    [[nodiscard]] bool canPlayAudio() const noexcept {
        return !interrupted_.load(std::memory_order_acquire);
    }

    // Provide direct pointer for RT callback to avoid singleton access on audio thread.
    [[nodiscard]] std::atomic<bool>* interruptedFlagPtr() noexcept { return &interrupted_; }

    // Detect the current route from AVAudioSession if not yet known.
    // Safe to call before initialize() — only queries OS route state.
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

    void requestStreamRestart() noexcept {
        StreamRestartCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = restartCallback_;
        }
        if (cb) cb();
    }

    // Returns the platform device ID of the current route (UID of AVAudioSessionPortDescription)
    [[nodiscard]] std::string currentDeviceId() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return currentDeviceId_;
    }

    void setAudioFocusCallback(AudioFocusCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        audioFocusCallback_ = std::move(cb);
    }

    // Wire drift reset callback into RouteHandler (used by MediaPipelineModule)
    void setDriftResetCallback(std::function<void()> fn) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        driftResetCallback_ = std::move(fn);
    }

    [[nodiscard]] uint64_t routeChangeCount() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return routeHandler_.routeChangeCount();
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
    std::atomic<bool> interrupted_{false};
    bool initializing_{false};  // guarded by mutex_, serializes concurrent first-init callers
    int32_t refCount_{0};

    // Host-app AVAudioSession state snapshotted before we reconfigure, so shutdown()
    // can restore it (only if nobody else has changed the session in the interim).
    // Opaque NSString*/NSNumber* to keep this header ObjC++-neutral.
    void* savedCategory_{nullptr};       // NSString*
    void* savedMode_{nullptr};           // NSString*
    unsigned long savedCategoryOptions_{0};  // AVAudioSessionCategoryOptions
    bool hostStateSaved_{false};

    // Opaque pointer to Objective-C notification observers
    void* routeChangeObserver_{nullptr};
    void* interruptionObserver_{nullptr};
    void* mediaServicesLostObserver_{nullptr};
    void* mediaServicesResetObserver_{nullptr};

    // Serial queue for notification delivery. Using a dedicated queue ensures:
    // 1. Handlers never fire on the audio thread (queue:nil delivers on posting thread)
    // 2. removeObserver: drains the queue, guaranteeing no in-flight handlers after return
    void* notificationQueue_{nullptr};  // NSOperationQueue*
    void* notificationDispatchQueue_{nullptr};  // dispatch_queue_t (serializes notification handlers + RouteHandler access)

    std::atomic<AudioRoute> currentRoute_{AudioRoute::Unknown};

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
    std::function<void()> driftResetCallback_;
    AudioFocusCallback audioFocusCallback_;

    // Reactive route handler (replaces RoutingStateMachine)
    RouteHandler routeHandler_;
};

}  // namespace media
