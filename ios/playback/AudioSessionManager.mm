#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

namespace {
bool isLeAudioBidirectional(NSArray<AVAudioSessionPortDescription*>* inputs,
                            NSString* uid) {
    for (AVAudioSessionPortDescription* input in inputs) {
        if ([input.portType isEqualToString:AVAudioSessionPortBluetoothLE] &&
            [input.UID isEqualToString:uid]) {
            return true;
        }
    }
    return false;
}
}  // anonymous namespace

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

    // Create the notification dispatch queue for serializing route change handlers.
    if (!notificationDispatchQueue_) {
        dispatch_queue_t dq = dispatch_queue_create("media.AudioSessionManager.notifications",
                                                     DISPATCH_QUEUE_SERIAL);
        notificationDispatchQueue_ = (__bridge_retained void*)dq;
    }

    // Wire RouteHandler callbacks to existing infrastructure
    routeHandler_.setCallbacks({
        .restartStreams = [this]() { this->requestStreamRestart(); },
        .fireJsCallback = [this](AudioRoute route, EchoMode echoMode) {
            AudioRouteCallback cb;
            {
                std::lock_guard<std::mutex> lk(this->mutex_);
                cb = this->routeCallback_;
            }
            if (cb) cb(route, echoMode);
        },
        .resetDrift = [this]() {
            if (this->driftResetCallback_) this->driftResetCallback_();
        },
    });

    {
        std::lock_guard<std::mutex> lk(mutex_);
        registerNotifications();
        focusState_.store(AudioFocusState::Gain, std::memory_order_release);

        initialized_.store(true, std::memory_order_release);
        initializing_ = false;
        MEDIA_LOG_I("AudioSessionManager initialized (refCount=%d)", refCount_);
    }

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
            this->currentEchoMode_.load(std::memory_order_acquire),
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

        // DefaultToSpeaker is a persistent category option that tells iOS
        // "prefer speaker over earpiece when no external device is selected."
        // It does NOT override active external devices — iOS gives BT/wired
        // priority over DefaultToSpeaker automatically (Apple QA1754).
        // Always include it when speakerPreferred_ is true, regardless of
        // current route. This matches WebRTC's approach and ensures the
        // preference survives AudioUnit restarts (which clear the transient
        // overrideOutputAudioPort but not category options).
        //
        AVAudioSessionCategoryOptions options =
            AVAudioSessionCategoryOptionAllowBluetooth |
            AVAudioSessionCategoryOptionAllowBluetoothA2DP;
        if (speakerPreferred_.load(std::memory_order_acquire)) {
            options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
        }

        BOOL success = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                       mode:AVAudioSessionModeVoiceChat
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
        }

        MEDIA_LOG_I("AudioSessionManager: AVAudioSession configured (PlayAndRecord + Bluetooth, speaker=%s)",
                    speakerPreferred_.load(std::memory_order_relaxed) ? "yes" : "no");
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
    focusCallback_ = nullptr;
    lifecycleCallback_ = nullptr;

    // Deactivate audio session to release hardware and allow other apps to use audio
    @autoreleasepool {
        NSError* error = nil;
        [[AVAudioSession sharedInstance] setActive:NO
                                       withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                             error:&error];
        if (error) {
            MEDIA_LOG_W("AudioSessionManager: setActive:NO failed: %s",
                        error.localizedDescription.UTF8String);
        }
    }

    initialized_.store(false, std::memory_order_release);

    // Reset all routing state to defaults for clean re-initialization
    currentRoute_.store(AudioRoute::Unknown, std::memory_order_relaxed);
    currentEchoMode_.store(EchoMode::HardwareAEC, std::memory_order_relaxed);
    focusState_.store(AudioFocusState::Gain, std::memory_order_relaxed);
    speakerPreferred_.store(true, std::memory_order_relaxed);
    isForeground_.store(true, std::memory_order_relaxed);
    voiceModeEnabled_.store(false, std::memory_order_relaxed);
    currentSessionId_.store(0, std::memory_order_relaxed);
    measuredOutputLatencyUs_.store(0, std::memory_order_relaxed);
    a2dpLatencyLogged_.store(false, std::memory_order_relaxed);
    leAudioCachedUid_.clear();
    leAudioCachedBidirectional_ = false;

    MEDIA_LOG_I("AudioSessionManager shutdown");
}

void AudioSessionManager::registerNotifications() noexcept {
    @autoreleasepool {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

        // Use the pre-created serial dispatch queue.
        // All notification handlers run on this serial queue, preventing data races.
        dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
        NSOperationQueue* opQueue = [[NSOperationQueue alloc] init];
        opQueue.underlyingQueue = dq;
        opQueue.maxConcurrentOperationCount = 1;
        notificationQueue_ = (__bridge_retained void*)opQueue;

        // Audio route change notification — reactive routing.
        // On device connect/disconnect: reconfigure session and route to best device.
        // Then detect the current route and let RouteHandler handle dedup/restart.
        routeChangeObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionRouteChangeNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification* note) {
                        NSDictionary* userInfo = note.userInfo;
                        NSNumber* reasonNumber = userInfo[AVAudioSessionRouteChangeReasonKey];
                        AVAudioSessionRouteChangeReason reason =
                            static_cast<AVAudioSessionRouteChangeReason>([reasonNumber integerValue]);

                        // Update device poll baseline so the timer doesn't redundantly
                        // fire for changes we're already handling via this notification.
                        {
                            this->lastPolledDeviceUIDs_.clear();
                            AVAudioSession* s = [AVAudioSession sharedInstance];
                            for (AVAudioSessionPortDescription* p in s.availableInputs) {
                                if (p.UID) {
                                    this->lastPolledDeviceUIDs_.push_back(p.UID.UTF8String);
                                }
                            }
                            std::sort(this->lastPolledDeviceUIDs_.begin(),
                                      this->lastPolledDeviceUIDs_.end());
                        }

                        bool isDeviceChange =
                            reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
                            reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable;

                        if (isDeviceChange) {
                            this->configureAudioSession();
                            this->routeToBestDevice();
                        }

                        this->detectCurrentRoute();
                        std::string deviceId;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            deviceId = this->currentDeviceId_;
                        }
                        this->routeHandler_.onRouteDetected({
                            this->currentRoute_.load(std::memory_order_acquire),
                            this->currentEchoMode_.load(std::memory_order_acquire),
                            deviceId
                        });
                    }];

        // Audio interruption notification (phone calls, Siri, etc.)
        interruptionObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionInterruptionNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification* note) {
                        NSDictionary* userInfo = note.userInfo;
                        NSNumber* typeNumber = userInfo[AVAudioSessionInterruptionTypeKey];
                        AVAudioSessionInterruptionType type =
                            static_cast<AVAudioSessionInterruptionType>([typeNumber integerValue]);

                        bool began = (type == AVAudioSessionInterruptionTypeBegan);
                        bool shouldResume = false;

                        if (!began) {
                            NSNumber* optionsNumber = userInfo[AVAudioSessionInterruptionOptionKey];
                            AVAudioSessionInterruptionOptions options =
                                static_cast<AVAudioSessionInterruptionOptions>([optionsNumber integerValue]);
                            shouldResume = (options & AVAudioSessionInterruptionOptionShouldResume) != 0;
                        }

                        MEDIA_LOG_I("AVAudioSession interruption: began=%d shouldResume=%d", began, shouldResume);
                        this->onAudioInterruption(began, shouldResume);
                    }];

        // Media services lost — reset route handler
        mediaServicesLostObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionMediaServicesWereLostNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        MEDIA_LOG_E("AVAudioSession: media services were LOST");
                        this->focusState_.store(AudioFocusState::Loss, std::memory_order_release);
                        AudioFocusCallback cb;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            cb = this->focusCallback_;
                        }
                        if (cb) cb(AudioFocusState::Loss);
                        this->routeHandler_.reset();
                    }];

        // Media services reset — reconfigure, restart, re-detect
        mediaServicesResetObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        MEDIA_LOG_W("AVAudioSession: media services were reset");
                        this->configureAudioSession();
                        this->requestStreamRestart();
                        this->detectCurrentRoute();
                        std::string deviceId;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            deviceId = this->currentDeviceId_;
                        }
                        this->routeHandler_.onInitialRoute({
                            this->currentRoute_.load(std::memory_order_acquire),
                            this->currentEchoMode_.load(std::memory_order_acquire),
                            deviceId
                        });
                    }];

        // App lifecycle notifications for background mic restriction
        backgroundObserver_ = (__bridge_retained void*)[center
            addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        this->onLifecycleChanged(false);
                    }];

        foregroundObserver_ = (__bridge_retained void*)[center
            addObserverForName:UIApplicationWillEnterForegroundNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        this->onLifecycleChanged(true);
                    }];

        MEDIA_LOG_I("AudioSessionManager: registered notifications");
    }
}

void AudioSessionManager::unregisterNotifications() noexcept {
    @autoreleasepool {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

        if (routeChangeObserver_) {
            [center removeObserver:(__bridge id)routeChangeObserver_];
            CFRelease(routeChangeObserver_);
            routeChangeObserver_ = nullptr;
        }

        if (interruptionObserver_) {
            [center removeObserver:(__bridge id)interruptionObserver_];
            CFRelease(interruptionObserver_);
            interruptionObserver_ = nullptr;
        }

        if (mediaServicesLostObserver_) {
            [center removeObserver:(__bridge id)mediaServicesLostObserver_];
            CFRelease(mediaServicesLostObserver_);
            mediaServicesLostObserver_ = nullptr;
        }

        if (mediaServicesResetObserver_) {
            [center removeObserver:(__bridge id)mediaServicesResetObserver_];
            CFRelease(mediaServicesResetObserver_);
            mediaServicesResetObserver_ = nullptr;
        }

        if (backgroundObserver_) {
            [center removeObserver:(__bridge id)backgroundObserver_];
            CFRelease(backgroundObserver_);
            backgroundObserver_ = nullptr;
        }

        if (foregroundObserver_) {
            [center removeObserver:(__bridge id)foregroundObserver_];
            CFRelease(foregroundObserver_);
            foregroundObserver_ = nullptr;
        }

        if (notificationQueue_) {
            CFRelease(notificationQueue_);
            notificationQueue_ = nullptr;
        }
        // Release the dispatch queue after the NSOperationQueue.
        if (notificationDispatchQueue_) {
            CFRelease(notificationDispatchQueue_);
            notificationDispatchQueue_ = nullptr;
        }

        MEDIA_LOG_I("AudioSessionManager: unregistered notifications");
    }
}

void AudioSessionManager::ensureRouteDetected() noexcept {
    if (currentRoute_.load(std::memory_order_acquire) != AudioRoute::Unknown) return;
    detectCurrentRoute();
}

void AudioSessionManager::detectCurrentRoute() noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        AVAudioSessionRouteDescription* route = session.currentRoute;

        AudioRoute newRoute = AudioRoute::Unknown;
        EchoMode newEchoMode = EchoMode::HardwareAEC;
        bool isBuiltinPort = false;

        for (AVAudioSessionPortDescription* output in route.outputs) {
            NSString* portType = output.portType;

            if ([portType isEqualToString:AVAudioSessionPortBuiltInReceiver]) {
                newRoute = AudioRoute::Earpiece;
                newEchoMode = EchoMode::HardwareAEC;
                isBuiltinPort = true;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBuiltInSpeaker]) {
                newRoute = AudioRoute::Speaker;
                newEchoMode = EchoMode::HardwareAEC;
                isBuiltinPort = true;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones]) {
                newRoute = AudioRoute::WiredHeadset;
                newEchoMode = EchoMode::None;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                newRoute = AudioRoute::BluetoothSco;
                newEchoMode = EchoMode::BluetoothAEC;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                newRoute = AudioRoute::BluetoothA2dp;
                // A2DP is output-only (no mic), device mic used for capture.
                // AEC limitations logged once per connection in effectiveRouteLatencyUs().
                newEchoMode = EchoMode::HardwareAEC;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                // LE Audio can be unidirectional (output-only, like A2DP) or
                // bidirectional (input+output, like HFP/SCO). Check availableInputs
                // for a matching LE port to distinguish.
                // Cache the classification per-UID so mid-session input port changes
                // (e.g. headset firmware disabling mic for power) don't reclassify
                // a bidirectional device as A2DP, which would break echo cancellation.
                std::string uid = output.UID ? output.UID.UTF8String : "";
                bool isBidirectional = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    if (!uid.empty() && uid == leAudioCachedUid_) {
                        isBidirectional = leAudioCachedBidirectional_;
                    } else {
                        isBidirectional = isLeAudioBidirectional(session.availableInputs, output.UID);
                        if (!uid.empty()) {
                            leAudioCachedUid_ = uid;
                            leAudioCachedBidirectional_ = isBidirectional;
                        }
                    }
                }
                if (isBidirectional) {
                    newRoute = AudioRoute::BluetoothSco;
                    newEchoMode = EchoMode::BluetoothAEC;
                } else {
                    newRoute = AudioRoute::BluetoothA2dp;
                    newEchoMode = EchoMode::HardwareAEC;
                }
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                newRoute = AudioRoute::UsbDevice;
                newEchoMode = EchoMode::None;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortAirPlay] ||
                       [portType isEqualToString:AVAudioSessionPortHDMI] ||
                       [portType isEqualToString:AVAudioSessionPortCarAudio]) {
                // Output-only devices with no echo path, treat like external speaker
                newRoute = AudioRoute::Speaker;
                newEchoMode = EchoMode::None;
                MEDIA_LOG_I("detectCurrentRoute: mapped %s to Speaker (output-only)",
                            portType.UTF8String);
                break;
            } else {
                MEDIA_LOG_W("detectCurrentRoute: unrecognized port type: %s",
                            portType.UTF8String);
            }
        }

        // Cache the device ID for dedup in the route handler.
        // Built-in ports use synthetic IDs matching getAvailableAudioDevices();
        // external devices (including AirPlay/HDMI/CarAudio mapped to Speaker)
        // use real AVAudioSession UIDs for correct dedup.
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (isBuiltinPort && newRoute == AudioRoute::Speaker) {
                currentDeviceId_ = "builtin-speaker";
            } else if (isBuiltinPort && newRoute == AudioRoute::Earpiece) {
                currentDeviceId_ = "builtin-earpiece";
            } else {
                NSString* uid = route.outputs.firstObject.UID;
                currentDeviceId_ = uid ? uid.UTF8String : "";
            }
        }

        // Update route state directly — detectCurrentRoute() is a low-level helper
        // used during both initialization and notification handling.
        AudioRoute oldRoute = currentRoute_.exchange(newRoute, std::memory_order_acq_rel);
        currentEchoMode_.store(newEchoMode, std::memory_order_release);

        // Reset A2DP latency logging flag on route change to log again for new connection
        if (oldRoute != newRoute) {
            a2dpLatencyLogged_.store(false, std::memory_order_relaxed);
            // Clear LE Audio classification cache when leaving LE Audio route.
            // This forces a fresh bidirectionality check on the next LE connection.
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!leAudioCachedUid_.empty()) {
                    bool newIsLeAudio = false;
                    for (AVAudioSessionPortDescription* output in session.currentRoute.outputs) {
                        if ([output.portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                            newIsLeAudio = true;
                            break;
                        }
                    }
                    if (!newIsLeAudio) {
                        leAudioCachedUid_.clear();
                        leAudioCachedBidirectional_ = false;
                    }
                }
            }
            MEDIA_LOG_I("AudioRoute: %s -> %s (echo=%s)",
                        audioRouteToString(oldRoute), audioRouteToString(newRoute),
                        echoModeToString(newEchoMode));
        }
    }
}

void AudioSessionManager::onAudioInterruption(bool began, bool shouldResume) noexcept {
    AudioFocusState newState;
    if (began) {
        newState = AudioFocusState::LossTransient;
    } else if (shouldResume) {
        newState = AudioFocusState::Gain;
    } else {
        newState = AudioFocusState::Loss;
    }

    AudioFocusState oldState = focusState_.exchange(newState, std::memory_order_acq_rel);

    MEDIA_LOG_I("AudioFocus: %d -> %d", static_cast<int>(oldState), static_cast<int>(newState));

    // Copy callbacks under lock, invoke outside to avoid holding mutex during callback.
    // Safe to block: notification handlers run on main/system threads, not RT audio thread.
    // shutdown() unregisters notifications before acquiring mutex_, preventing deadlock.
    // AudioCoordinator's restart callback handles both capture and playback
    AudioFocusCallback focusCb;
    StreamRestartCallback restartCb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        focusCb = focusCallback_;
        if (!began && shouldResume) {
            restartCb = restartCallback_;
        }
    }

    if (focusCb) focusCb(newState);

    // If interruption ended and should resume, re-activate the session then restart.
    // iOS may deactivate the audio session during interruption; AudioUnit start
    // will fail unless we re-activate it first.
    // Dispatch async to avoid blocking the notification serial queue — setActive:YES
    // and restart callbacks may do synchronous AudioUnit teardown/recreate.
    if (restartCb) {
        dispatch_async(dispatch_get_main_queue(), ^{
            // Re-read callback from instance state to avoid using a stale captured
            // copy. If shutdown() completed while this block was queued, the callback
            // is null and we skip both setActive:YES and the restart.
            StreamRestartCallback cb;
            {
                std::lock_guard<std::mutex> lk(this->mutex_);
                if (!this->initialized_.load(std::memory_order_acquire)) return;
                cb = this->restartCallback_;
            }
            if (!cb) return;
            @autoreleasepool {
                NSError* error = nil;
                BOOL activated = [[AVAudioSession sharedInstance] setActive:YES error:&error];
                if (!activated) {
                    MEDIA_LOG_W("onAudioInterruption: setActive:YES failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                }
            }
            cb();
        });
    }
}

void AudioSessionManager::onLifecycleChanged(bool foreground) noexcept {
    bool wasForeground = isForeground_.exchange(foreground, std::memory_order_acq_rel);

    if (wasForeground == foreground) return;

    MEDIA_LOG_I("Lifecycle: %s -> %s",
                wasForeground ? "foreground" : "background",
                foreground ? "foreground" : "background");

    LifecycleCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = lifecycleCallback_;
    }
    if (cb) cb(foreground);

    // iOS 17.4+ stale interruption recovery: some system interruptions (Siri, CallKit)
    // fire InterruptionBegan without a matching InterruptionEnded. On foreground resume,
    // if focus is stuck in LossTransient, synthesize recovery.
    if (foreground &&
        focusState_.load(std::memory_order_acquire) == AudioFocusState::LossTransient) {
        MEDIA_LOG_W("StaleInterruption: stuck in LossTransient on foreground, recovering");
        dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
        if (dq) {
            dispatch_async(dq, ^{
                this->onAudioInterruption(/*began=*/false, /*shouldResume=*/true);
            });
        }
    }
}

AudioRoute AudioSessionManager::classifyLeAudioPort(
        void* uidRaw,
        void* availableInputsRaw) noexcept {
    NSString* uid = (__bridge NSString*)uidRaw;
    NSArray<AVAudioSessionPortDescription*>* availableInputs =
        (__bridge NSArray<AVAudioSessionPortDescription*>*)availableInputsRaw;
    std::string uidStr = uid ? uid.UTF8String : "";
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!uidStr.empty() && uidStr == leAudioCachedUid_) {
            return leAudioCachedBidirectional_
                ? AudioRoute::BluetoothSco : AudioRoute::BluetoothA2dp;
        }
    }
    // Cache miss — fall back to live check (don't update cache; only
    // detectCurrentRoute() should write the cache to keep it authoritative).
    bool isBidirectional = isLeAudioBidirectional(availableInputs, uid);
    return isBidirectional ? AudioRoute::BluetoothSco : AudioRoute::BluetoothA2dp;
}

std::vector<AudioDeviceInfo> AudioSessionManager::getAvailableAudioDevices() noexcept {
    std::vector<AudioDeviceInfo> devices;
    devices.reserve(8);

    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];

        // Built-in devices (always available on iPhone, no UID from ports)
        if ([[UIDevice currentDevice] userInterfaceIdiom] == UIUserInterfaceIdiomPhone) {
            devices.push_back({AudioRoute::Earpiece, "Earpiece", "builtin-earpiece"});
        }
        devices.push_back({AudioRoute::Speaker, "Speaker", "builtin-speaker"});

        // Track seen UIDs to avoid duplicates across inputs and outputs
        NSMutableSet<NSString*>* seenUIDs = [NSMutableSet set];

        // Enumerate available inputs for external devices
        for (AVAudioSessionPortDescription* port in session.availableInputs) {
            NSString* portType = port.portType;
            NSString* uid = port.UID;
            if ([seenUIDs containsObject:uid]) continue;
            [seenUIDs addObject:uid];

            AudioRoute r = AudioRoute::Unknown;
            if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                r = AudioRoute::BluetoothSco;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                r = AudioRoute::BluetoothA2dp;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                // LE Audio in availableInputs means bidirectional (like SCO)
                r = AudioRoute::BluetoothSco;
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones] ||
                       [portType isEqualToString:AVAudioSessionPortHeadsetMic]) {
                r = AudioRoute::WiredHeadset;
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                r = AudioRoute::UsbDevice;
            }

            if (r != AudioRoute::Unknown) {
                devices.push_back({
                    r,
                    port.portName ? port.portName.UTF8String : "",
                    uid ? uid.UTF8String : ""
                });
            }
        }

        // Also check current route outputs for devices not yet in inputs
        AVAudioSessionRouteDescription* currentRoute = session.currentRoute;
        for (AVAudioSessionPortDescription* output in currentRoute.outputs) {
            NSString* uid = output.UID;
            if ([seenUIDs containsObject:uid]) continue;
            [seenUIDs addObject:uid];

            NSString* portType = output.portType;
            AudioRoute r = AudioRoute::Unknown;
            if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                r = AudioRoute::BluetoothSco;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                r = AudioRoute::BluetoothA2dp;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                r = this->classifyLeAudioPort((__bridge void*)uid, (__bridge void*)session.availableInputs);
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones]) {
                r = AudioRoute::WiredHeadset;
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                r = AudioRoute::UsbDevice;
            } else if ([portType isEqualToString:AVAudioSessionPortAirPlay] ||
                       [portType isEqualToString:AVAudioSessionPortHDMI] ||
                       [portType isEqualToString:AVAudioSessionPortCarAudio]) {
                // Output-only external devices — map to Speaker so they appear
                // in the device list with their actual name (e.g. "Apple TV").
                r = AudioRoute::Speaker;
            }

            if (r != AudioRoute::Unknown) {
                devices.push_back({
                    r,
                    output.portName ? output.portName.UTF8String : "",
                    uid ? uid.UTF8String : ""
                });
            }
        }
    }

    return devices;
}

bool AudioSessionManager::setAudioRoute(AudioRoute route, const std::string& deviceId) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
        MEDIA_LOG_W("setAudioRoute: called before audio session initialized");
        return false;
    }

    // Dispatch on the notification serial queue to serialize with route change handlers.
    __block bool success = false;
    dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
    dispatch_sync(dq, ^{
        this->routeHandler_.onExplicitRouteRequested(route);
        success = this->executeRouteChangePlatform(route, deviceId);
    });

    return success;
}

bool AudioSessionManager::executeRouteChangePlatform(AudioRoute route, const std::string& deviceId) noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // Shared logic for WiredHeadset / UsbDevice: reconfigure session without
        // DefaultToSpeaker, scan availableInputs for a matching port type, and
        // set it as preferred input. Rolls back speakerPreferred_ on failure.
        auto routeToInputPort = [&](NSArray<NSString*>* portTypes,
                                    const char* routeName) -> bool {
            bool wasSpeakerPreferred = speakerPreferred_.exchange(false, std::memory_order_acq_rel);
            {
                NSError* catError = nil;
                AVAudioSessionCategoryOptions options =
                    AVAudioSessionCategoryOptionAllowBluetooth |
                    AVAudioSessionCategoryOptionAllowBluetoothA2DP;
                BOOL catSuccess = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                                  mode:AVAudioSessionModeVoiceChat
                                               options:options
                                                 error:&catError];
                if (!catSuccess) {
                    MEDIA_LOG_E("setAudioRoute: setCategory(%s) failed: %s",
                                routeName, catError ? catError.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    return false;
                }
                [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
                [session setPreferredInput:nil error:nil];
            }

            NSString* targetUID = deviceId.empty() ? nil : [NSString stringWithUTF8String:deviceId.c_str()];
            NSArray<AVAudioSessionPortDescription*>* availableInputs = session.availableInputs;
            for (AVAudioSessionPortDescription* port in availableInputs) {
                bool matched = false;
                for (NSString* pt in portTypes) {
                    if ([port.portType isEqualToString:pt]) { matched = true; break; }
                }
                if (!matched) continue;
                if (targetUID && ![port.UID isEqualToString:targetUID]) continue;

                BOOL success = [session setPreferredInput:port error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: setPreferredInput(%s) failed: %s",
                                routeName, error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    configureAudioSession();
                    return false;
                }
                MEDIA_LOG_I("setAudioRoute: set preferred input to %s", port.portName.UTF8String);
                return true;
            }
            MEDIA_LOG_W("setAudioRoute: no %s available", routeName);
            speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
            configureAudioSession();
            return false;
        };

        switch (route) {
            case AudioRoute::Speaker: {
                // Update speakerPreferred_ BEFORE API calls so configureAudioSession()
                // reads the correct value if a notification fires during the transition.
                bool wasSpeakerPreferred = speakerPreferred_.exchange(true, std::memory_order_acq_rel);

                // Reconfigure with DefaultToSpeaker option and apply speaker override.
                // NOTE: overrideOutputAudioPort:Speaker only overrides earpiece→speaker,
                // it does NOT override active BT (Apple QA1754). Forcing speaker while
                // BT is connected is a known iOS limitation — tracked for future work.
                AVAudioSessionCategoryOptions options =
                    AVAudioSessionCategoryOptionDefaultToSpeaker |
                    AVAudioSessionCategoryOptionAllowBluetooth |
                    AVAudioSessionCategoryOptionAllowBluetoothA2DP;

                BOOL success = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                               mode:AVAudioSessionModeVoiceChat
                                            options:options
                                              error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: setCategory(Speaker) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    return false;
                }

                // Clear stale preferred input from prior BT/Wired/USB selection.
                // setPreferredInput: is sticky — without clearing, the input mic stays
                // pinned to the old device even after speaker override takes effect.
                AVAudioSessionPortDescription* previousInput = session.preferredInput;
                [session setPreferredInput:nil error:nil];

                success = [session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: overrideOutputAudioPort(Speaker) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    // Rollback: restore preferredInput, speakerPreferred_, and category
                    [session setPreferredInput:previousInput error:nil];
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    AVAudioSessionCategoryOptions rollbackOptions =
                        AVAudioSessionCategoryOptionAllowBluetooth |
                        AVAudioSessionCategoryOptionAllowBluetoothA2DP;
                    [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                    mode:AVAudioSessionModeVoiceChat
                                 options:rollbackOptions
                                   error:nil];
                    return false;
                }
                // Route change notification will fire with AVAudioSessionRouteChangeReasonOverride
                // and handle restart if needed. Don't call requestStreamRestart() here to avoid
                // double-restart (causes janky audio during speaker↔earpiece switch).
                return true;
            }

            case AudioRoute::Earpiece: {
                // iPad has no earpiece receiver — reject early to avoid misleading
                // the caller (audio would silently route to the bottom speaker).
                if ([[UIDevice currentDevice] userInterfaceIdiom] != UIUserInterfaceIdiomPhone) {
                    MEDIA_LOG_W("setAudioRoute: earpiece not available on this device");
                    return false;
                }

                // Update speakerPreferred_ BEFORE API calls so configureAudioSession()
                // reads the correct value if a notification fires during the transition.
                bool wasSpeakerPreferred = speakerPreferred_.exchange(false, std::memory_order_acq_rel);

                // Reconfigure WITHOUT DefaultToSpeaker but WITH AllowBluetooth
                // (keeping AllowBluetooth ensures BT devices stay visible in
                // availableInputs — removing it hides them from the UI).
                AVAudioSessionCategoryOptions options =
                    AVAudioSessionCategoryOptionAllowBluetooth |
                    AVAudioSessionCategoryOptionAllowBluetoothA2DP;

                BOOL success = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                               mode:AVAudioSessionModeVoiceChat
                                            options:options
                                              error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: setCategory(Earpiece) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    return false;
                }

                // Select builtin mic as preferred input — decouples from HFP
                // (Apple QA1799), causing output to route to earpiece receiver.
                bool builtinSelected = false;
                for (AVAudioSessionPortDescription* port in session.availableInputs) {
                    if ([port.portType isEqualToString:AVAudioSessionPortBuiltInMic]) {
                        success = [session setPreferredInput:port error:&error];
                        if (success) {
                            builtinSelected = true;
                            MEDIA_LOG_I("setAudioRoute: set preferred input to %s",
                                        port.portName.UTF8String);
                        } else {
                            MEDIA_LOG_E("setAudioRoute: setPreferredInput(BuiltInMic) failed: %s",
                                        error ? error.localizedDescription.UTF8String : "unknown");
                        }
                        break;
                    }
                }

                if (!builtinSelected) {
                    MEDIA_LOG_W("setAudioRoute: builtin mic not found, cannot route to earpiece");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    [session setPreferredInput:nil error:nil];
                    configureAudioSession();
                    return false;
                }

                // Remove any speaker override
                success = [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: overrideOutputAudioPort(None) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    [session setPreferredInput:nil error:nil];
                    configureAudioSession();
                    return false;
                }

                // NOTE: setPreferredInput:BuiltInMic may not override active BT in
                // VoiceChat mode on iOS 15+ (iOS treats AllowBluetooth as higher
                // priority than preferredInput). Forcing earpiece while BT is
                // connected is a known iOS limitation — same as the speaker case.
                // Tracked for future work alongside BT→Speaker forcing.

                return true;
            }

            case AudioRoute::BluetoothSco:
            case AudioRoute::BluetoothA2dp: {
                // Update speakerPreferred_ BEFORE API calls so configureAudioSession()
                // reads the correct value if a notification fires during the transition.
                bool wasSpeakerPreferred = speakerPreferred_.exchange(false, std::memory_order_acq_rel);

                // Clear DefaultToSpeaker so iOS doesn't override BT routing.
                // When Speaker was previously selected, the category has DefaultToSpeaker
                // and overrideOutputAudioPort is set to Speaker — both must be cleared
                // or the system ignores setPreferredInput for the BT port.
                AVAudioSessionCategoryOptions options =
                    AVAudioSessionCategoryOptionAllowBluetooth |
                    AVAudioSessionCategoryOptionAllowBluetoothA2DP;

                BOOL success = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                               mode:AVAudioSessionModeVoiceChat
                                            options:options
                                              error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: setCategory(Bluetooth) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    return false;
                }

                // Clear speaker override — required for BT output to take effect
                success = [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: overrideOutputAudioPort(None) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                    configureAudioSession();
                    return false;
                }

                // Find Bluetooth port — prefer the port type matching the requested route,
                // fall back to any BT port if the specific type isn't available.
                // SCO → HFP or bidirectional LE, A2DP → A2DP or unidirectional LE.
                NSString* targetUID = deviceId.empty() ? nil : [NSString stringWithUTF8String:deviceId.c_str()];
                NSArray<AVAudioSessionPortDescription*>* availableInputs = session.availableInputs;

                auto selectPort = [&](AVAudioSessionPortDescription* port) -> bool {
                    if (targetUID && ![port.UID isEqualToString:targetUID]) return false;
                    success = [session setPreferredInput:port error:&error];
                    if (!success) {
                        MEDIA_LOG_E("setAudioRoute: setPreferredInput(Bluetooth) failed: %s",
                                    error ? error.localizedDescription.UTF8String : "unknown");
                        return false;
                    }
                    MEDIA_LOG_I("setAudioRoute: set preferred input to %s (type=%s, uid=%s)",
                                port.portName.UTF8String, port.portType.UTF8String,
                                port.UID.UTF8String);
                    return true;
                };

                // First pass: match the specific BT profile requested.
                // Use classifyLeAudioPort() for LE Audio to leverage the
                // bidirectionality cache (prevents misclassification if the
                // input port temporarily disappears due to firmware power mgmt).
                for (AVAudioSessionPortDescription* port in availableInputs) {
                    NSString* pt = port.portType;
                    if (route == AudioRoute::BluetoothSco) {
                        if ([pt isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                            if (selectPort(port)) return true;
                        } else if ([pt isEqualToString:AVAudioSessionPortBluetoothLE]) {
                            AudioRoute leRoute = classifyLeAudioPort(
                                (__bridge void*)port.UID, (__bridge void*)availableInputs);
                            if (leRoute == AudioRoute::BluetoothSco &&
                                selectPort(port)) return true;
                        }
                    } else {
                        if ([pt isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                            if (selectPort(port)) return true;
                        } else if ([pt isEqualToString:AVAudioSessionPortBluetoothLE]) {
                            AudioRoute leRoute = classifyLeAudioPort(
                                (__bridge void*)port.UID, (__bridge void*)availableInputs);
                            if (leRoute == AudioRoute::BluetoothA2dp &&
                                selectPort(port)) return true;
                        }
                    }
                }

                // Fallback: any BT port if the specific profile isn't available.
                // A device may only expose one profile at a time (e.g. HFP without A2DP).
                for (AVAudioSessionPortDescription* port in availableInputs) {
                    NSString* pt = port.portType;
                    if ([pt isEqualToString:AVAudioSessionPortBluetoothHFP] ||
                        [pt isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
                        [pt isEqualToString:AVAudioSessionPortBluetoothLE]) {
                        if (selectPort(port)) return true;
                    }
                }
                MEDIA_LOG_W("setAudioRoute: no Bluetooth device available%s",
                            targetUID ? " matching deviceId" : "");
                speakerPreferred_.store(wasSpeakerPreferred, std::memory_order_release);
                configureAudioSession();
                return false;
            }

            case AudioRoute::WiredHeadset:
                return routeToInputPort(
                    @[AVAudioSessionPortHeadsetMic, AVAudioSessionPortHeadphones],
                    "wired headset");

            case AudioRoute::UsbDevice:
                return routeToInputPort(
                    @[AVAudioSessionPortUSBAudio],
                    "USB device");

            default:
                MEDIA_LOG_W("setAudioRoute: unsupported route %d", static_cast<int>(route));
                return false;
        }
    }
}

std::vector<AudioRoute> AudioSessionManager::getAvailableAudioRoutes() noexcept {
    std::vector<AudioRoute> routes;
    routes.reserve(7);  // Max possible routes

    // Use bitset for O(1) deduplication instead of O(n) std::find
    uint32_t seen = 0;
    auto addRoute = [&routes, &seen](AudioRoute r) {
        uint32_t bit = 1u << static_cast<uint8_t>(r);
        if (!(seen & bit)) {
            seen |= bit;
            routes.push_back(r);
        }
    };

    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];

        // Speaker is always available; earpiece only on iPhone (not iPad)
        if ([[UIDevice currentDevice] userInterfaceIdiom] == UIUserInterfaceIdiomPhone) {
            addRoute(AudioRoute::Earpiece);
        }
        addRoute(AudioRoute::Speaker);

        // Check available inputs for Bluetooth and wired headsets
        NSArray<AVAudioSessionPortDescription*>* availableInputs = session.availableInputs;
        for (AVAudioSessionPortDescription* port in availableInputs) {
            NSString* portType = port.portType;

            if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                addRoute(AudioRoute::BluetoothSco);
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                addRoute(AudioRoute::BluetoothA2dp);
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                // LE Audio in availableInputs means bidirectional (like SCO)
                addRoute(AudioRoute::BluetoothSco);
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones] ||
                       [portType isEqualToString:AVAudioSessionPortHeadsetMic]) {
                addRoute(AudioRoute::WiredHeadset);
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                addRoute(AudioRoute::UsbDevice);
            }
        }

        // Also check current route outputs for devices that may not appear in inputs.
        AVAudioSessionRouteDescription* currentRoute = session.currentRoute;
        for (AVAudioSessionPortDescription* output in currentRoute.outputs) {
            NSString* portType = output.portType;

            if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                addRoute(AudioRoute::BluetoothSco);
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                addRoute(AudioRoute::BluetoothA2dp);
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                addRoute(this->classifyLeAudioPort((__bridge void*)output.UID, (__bridge void*)availableInputs));
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones]) {
                addRoute(AudioRoute::WiredHeadset);
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                addRoute(AudioRoute::UsbDevice);
            }
        }
    }

    return routes;
}

void AudioSessionManager::routeToBestDevice() noexcept {
    bool speakerPref = speakerPreferred_.load(std::memory_order_acquire);
    if (!speakerPref) return;

    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];

        // Only override to speaker if no external device is the active output.
        bool externalActive = false;
        for (AVAudioSessionPortDescription* output in session.currentRoute.outputs) {
            NSString* pt = output.portType;
            if ([pt isEqualToString:AVAudioSessionPortBluetoothHFP] ||
                [pt isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
                [pt isEqualToString:AVAudioSessionPortBluetoothLE] ||
                [pt isEqualToString:AVAudioSessionPortHeadphones] ||
                [pt isEqualToString:AVAudioSessionPortUSBAudio]) {
                externalActive = true;
                break;
            }
        }

        if (!externalActive) {
            NSError* error = nil;
            [session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&error];
            if (error) {
                MEDIA_LOG_W("routeToBestDevice: speaker override failed: %s",
                            error.localizedDescription.UTF8String);
            }
        }
    }
}

void AudioSessionManager::startDevicePollTimer() noexcept {
    if (devicePollTimer_) return;
    dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
    if (!dq) return;

    // Snapshot the initial device count so the first poll has a baseline.
    // No UID collection needed — the fast path only compares count.
    @autoreleasepool {
        lastPolledDeviceUIDs_.clear();
        NSArray<AVAudioSessionPortDescription*>* inputs = [AVAudioSession sharedInstance].availableInputs;
        for (AVAudioSessionPortDescription* port in inputs) {
            if (port.UID) {
                lastPolledDeviceUIDs_.push_back(port.UID.UTF8String);
            }
        }
        std::sort(lastPolledDeviceUIDs_.begin(), lastPolledDeviceUIDs_.end());
    }

    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dq);
    // Poll every 3s with 1s leeway — iOS doesn't fire OldDeviceUnavailable when
    // a BT device is powered off while not the active route (confirmed iOS bug,
    // also hit by Amazon Chime SDK). This is the only reliable workaround.
    dispatch_source_set_timer(timer,
                              dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                              3 * NSEC_PER_SEC,
                              1 * NSEC_PER_SEC);  // 1s leeway for battery

    dispatch_source_set_event_handler(timer, ^{
        this->pollDeviceListChanges();
    });

    dispatch_resume(timer);
    devicePollTimer_ = (__bridge_retained void*)timer;
    MEDIA_LOG_I("AudioSessionManager: device poll timer started (3s interval)");
}

void AudioSessionManager::stopDevicePollTimer() noexcept {
    if (!devicePollTimer_) return;
    dispatch_source_t timer = (__bridge_transfer dispatch_source_t)devicePollTimer_;
    dispatch_source_cancel(timer);
    devicePollTimer_ = nullptr;
    lastPolledDeviceUIDs_.clear();
    MEDIA_LOG_I("AudioSessionManager: device poll timer stopped");
}

void AudioSessionManager::pollDeviceListChanges() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;

    @autoreleasepool {
        NSArray<AVAudioSessionPortDescription*>* inputs = [AVAudioSession sharedInstance].availableInputs;

        // Fast path: count mismatch means definite change (most common case).
        // Avoids UID enumeration + sort + string comparison on every tick.
        NSUInteger currentCount = inputs.count;
        if (currentCount == lastPolledDeviceUIDs_.size()) {
            // Count matches — do full UID comparison only if counts equal
            // (handles device swap: one disconnects, another connects simultaneously)
            std::vector<std::string> currentUIDs;
            currentUIDs.reserve(currentCount);
            for (AVAudioSessionPortDescription* port in inputs) {
                if (port.UID) {
                    currentUIDs.push_back(port.UID.UTF8String);
                }
            }
            std::sort(currentUIDs.begin(), currentUIDs.end());
            if (currentUIDs == lastPolledDeviceUIDs_) return;
            lastPolledDeviceUIDs_ = std::move(currentUIDs);
        } else {
            // Count changed — rebuild baseline
            lastPolledDeviceUIDs_.clear();
            lastPolledDeviceUIDs_.reserve(currentCount);
            for (AVAudioSessionPortDescription* port in inputs) {
                if (port.UID) {
                    lastPolledDeviceUIDs_.push_back(port.UID.UTF8String);
                }
            }
            std::sort(lastPolledDeviceUIDs_.begin(), lastPolledDeviceUIDs_.end());
        }

        MEDIA_LOG_I("AudioSessionManager: device list changed (poll detected, now %zu inputs)",
                    lastPolledDeviceUIDs_.size());

        // Fire the route callback so JS gets an updated device list
        AudioRoute route = currentRoute_.load(std::memory_order_acquire);
        EchoMode echoMode = currentEchoMode_.load(std::memory_order_acquire);
        AudioRouteCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = routeCallback_;
        }
        if (cb) cb(route, echoMode);
    }
}

}  // namespace media
