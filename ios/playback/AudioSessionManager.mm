// AudioSessionManager top-level lifecycle: singleton, initialize/shutdown,
// AVAudioSession category configuration. The remaining concerns are split
// across sibling .mm files:
//
//   AudioSessionNotifications.mm — register/unregisterNotifications
//   AudioRouteDetector.mm        — ensureRouteDetected, detectCurrentRoute,
//                                  classifyLeAudioPort
//   AudioRouteSetter.mm          — setAudioRoute, executeRouteChangePlatform
//   AudioDeviceEnumerator.mm     — getAvailableAudioRoutes/Devices
//   AudioDevicePoller.mm         — start/stop/pollDeviceListChanges
//
// All five are member functions of AudioSessionManager; the class
// declaration lives in AudioSessionManager.h.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>

namespace media {

AudioSessionManager& AudioSessionManager::instance() noexcept {
    static AudioSessionManager mgr;
    return mgr;
}

AudioSessionManager::~AudioSessionManager() = default;

bool AudioSessionManager::initialize() noexcept {
    bool shouldInit = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++refCount_;
        if (initialized_.load(std::memory_order_acquire)) return true;
        if (initializing_) return true;
        initializing_ = true;
        shouldInit = true;
    }

    if (!shouldInit) return true;

    // Diagnostic: host app must declare UIBackgroundModes=audio for background playback.
    // Without it, iOS suspends our audio session on backgrounding and the pipeline silently
    // stops producing audio. We still init — the host may only support foreground playback.
    NSArray* backgroundModes = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"UIBackgroundModes"];
    if (!backgroundModes || ![backgroundModes containsObject:@"audio"]) {
        MEDIA_LOG_W("AudioSessionManager: host Info.plist lacks UIBackgroundModes=audio — "
                    "playback will suspend on backgrounding");
    }

    // Create the notification dispatch queue for serializing route change handlers.
    if (!notificationDispatchQueue_) {
        dispatch_queue_t dq = dispatch_queue_create("media.AudioSessionManager.notifications",
                                                     DISPATCH_QUEUE_SERIAL);
        notificationDispatchQueue_ = (__bridge_retained void*)dq;
    }

    // Wire RouteHandler callbacks to existing infrastructure
    routeHandler_.setCallbacks({
        .restartStreams = [this]() { this->requestStreamRestart(); },
        .fireJsCallback = [this](AudioRoute route) {
            AudioRouteCallback cb;
            {
                std::lock_guard<std::mutex> lk(this->mutex_);
                cb = this->routeCallback_;
            }
            if (cb) cb(route);
        },
        .resetDrift = [this]() {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lk(this->mutex_);
                cb = this->driftResetCallback_;
            }
            if (cb) cb();
        },
    });

    {
        std::lock_guard<std::mutex> lk(mutex_);
        registerNotifications();

        initialized_.store(true, std::memory_order_release);
        initializing_ = false;
        MEDIA_LOG_I("AudioSessionManager initialized (refCount=%d)", refCount_);
    }

    interrupted_.store(false, std::memory_order_release);

    // Initialize: configure session, detect route, notify JS.
    // dispatch_sync serializes with notification handlers.
    dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
    dispatch_sync(dq, ^{
        this->configureAudioSession();
        this->detectCurrentRoute();
        std::string deviceId;
        {
            std::lock_guard<std::mutex> lk(this->mutex_);
            deviceId = this->currentDeviceId_;
        }
        routeHandler_.onInitialRoute({
            this->currentRoute_.load(std::memory_order_acquire),
            deviceId
        });
    });

    // Start periodic device list polling to detect silent BT disconnects.
    // iOS doesn't fire OldDeviceUnavailable when a BT device is turned off
    // while it's not the active route. This timer catches those changes.
    startDevicePollTimer();

    return true;
}

bool AudioSessionManager::configureAudioSession() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return configureAudioSessionLocked();
}

bool AudioSessionManager::configureAudioSessionLocked() noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // Snapshot host-app session state on first configure so shutdown() can restore it.
        // Only save once — repeated configures (e.g. route change recovery) shouldn't
        // overwrite the saved state with our own category.
        if (!hostStateSaved_) {
            savedCategory_ = (__bridge_retained void*)session.category;
            savedMode_ = (__bridge_retained void*)session.mode;
            savedCategoryOptions_ = static_cast<unsigned long>(session.categoryOptions);
            hostStateSaved_ = true;
        }

        // Playback-only: use .playback category with .moviePlayback mode.
        // AllowBluetoothA2DP enables high-quality A2DP output for playback.
        // AllowBluetooth (HFP) is not needed — no capture path.
        AVAudioSessionCategoryOptions options =
            AVAudioSessionCategoryOptionAllowBluetoothA2DP;

        BOOL success = [session setCategory:AVAudioSessionCategoryPlayback
                                       mode:AVAudioSessionModeMoviePlayback
                                    options:options
                                      error:&error];
        if (!success) {
            MEDIA_LOG_E("AudioSessionManager: setCategory failed: %s",
                        error ? error.localizedDescription.UTF8String : "unknown");
            return false;
        }

        success = [session setActive:YES error:&error];
        if (!success) {
            MEDIA_LOG_E("AudioSessionManager: setActive failed: %s",
                        error ? error.localizedDescription.UTF8String : "unknown");
            return false;
        }

        MEDIA_LOG_I("AudioSessionManager: AVAudioSession configured (Playback + BluetoothA2DP)");
    }
    return true;
}

void AudioSessionManager::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!initialized_.load(std::memory_order_acquire) || refCount_ <= 0) return;
        --refCount_;
        if (refCount_ > 0) {
            MEDIA_LOG_I("AudioSessionManager: shutdown deferred (refCount=%d)", refCount_);
            return;
        }
    }

    // Reset route handler on the notification serial queue.
    // dispatch_sync serializes with notification handlers,
    // preventing data races. Must complete before unregisterNotifications()
    // releases the queue.
    if (notificationDispatchQueue_) {
        dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
        dispatch_sync(dq, ^{
            this->routeHandler_.reset();
        });
    }

    // Stop device poll timer before unregistering notifications
    stopDevicePollTimer();

    // Unregister notifications BEFORE acquiring mutex_ to prevent deadlock
    unregisterNotifications();

    std::lock_guard<std::mutex> lk(mutex_);
    if (!initialized_.load(std::memory_order_acquire)) return;

    routeCallback_ = nullptr;
    restartCallback_ = nullptr;
    driftResetCallback_ = nullptr;
    audioFocusCallback_ = nullptr;

    // Deactivate audio session to release hardware and allow other apps to use audio.
    // Then restore the host-app session state IFF our category is still what we set
    // (if another module changed it mid-playback, don't clobber their change).
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        [session setActive:NO
               withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                     error:&error];
        if (error) {
            MEDIA_LOG_W("AudioSessionManager: setActive:NO failed: %s",
                        error.localizedDescription.UTF8String);
        }

        if (hostStateSaved_) {
            NSString* savedCategory = (__bridge_transfer NSString*)savedCategory_;
            NSString* savedMode = (__bridge_transfer NSString*)savedMode_;
            savedCategory_ = nullptr;
            savedMode_ = nullptr;
            hostStateSaved_ = false;

            // Only restore if our category is still active — don't clobber a new owner.
            if (savedCategory && [session.category isEqualToString:AVAudioSessionCategoryPlayback]) {
                NSError* restoreErr = nil;
                [session setCategory:savedCategory
                                mode:(savedMode ?: AVAudioSessionModeDefault)
                             options:static_cast<AVAudioSessionCategoryOptions>(savedCategoryOptions_)
                               error:&restoreErr];
                if (restoreErr) {
                    MEDIA_LOG_W("AudioSessionManager: restore host category failed: %s",
                                restoreErr.localizedDescription.UTF8String);
                }
            }
        }
    }

    initialized_.store(false, std::memory_order_release);

    // Reset all routing state to defaults for clean re-initialization
    currentRoute_.store(AudioRoute::Unknown, std::memory_order_relaxed);
    leAudioCachedUid_.clear();
    leAudioCachedBidirectional_ = false;

    MEDIA_LOG_I("AudioSessionManager shutdown");
}

}  // namespace media
