// AVAudioSession notification handlers — route changes, interruptions, media
// services lost/reset. Drives RouteHandler from the serial notification queue.
//
// Member functions of AudioSessionManager; class definition lives in
// AudioSessionManager.h. Other implementations are split into
// AudioRouteDetector.mm, AudioRouteSetter.mm, AudioDeviceEnumerator.mm,
// AudioDevicePoller.mm, and AudioSessionManager.mm.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>

#include <algorithm>

namespace media {

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
        // On device connect/disconnect: reconfigure session.
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
                        }

                        this->detectCurrentRoute();
                        std::string deviceId;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            deviceId = this->currentDeviceId_;
                        }
                        this->routeHandler_.onRouteDetected({
                            this->currentRoute_.load(std::memory_order_acquire),
                            deviceId
                        });
                    }];

        // Audio interruption notification (phone calls, Siri, etc.)
        // For playback-only, just log and attempt recovery when interruption ends.
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

                        if (began) {
                            this->interrupted_.store(true, std::memory_order_release);
                        } else {
                            // Interruption ended — clear flag regardless of shouldResume.
                            // shouldResume controls auto-restart, not whether audio hardware is available.
                            this->interrupted_.store(false, std::memory_order_release);
                            NSNumber* optionsNumber = userInfo[AVAudioSessionInterruptionOptionKey];
                            AVAudioSessionInterruptionOptions options =
                                static_cast<AVAudioSessionInterruptionOptions>([optionsNumber integerValue]);
                            shouldResume = (options & AVAudioSessionInterruptionOptionShouldResume) != 0;
                        }

                        MEDIA_LOG_I("AVAudioSession interruption: began=%d shouldResume=%d", began, shouldResume);

                        // Map iOS interruption to AudioFocusState for JS parity with Android
                        {
                            AudioFocusState focusState = began
                                ? AudioFocusState::LostTransient
                                : AudioFocusState::Gained;
                            AudioFocusCallback focusCb;
                            {
                                std::lock_guard<std::mutex> lk(this->mutex_);
                                focusCb = this->audioFocusCallback_;
                            }
                            if (focusCb) focusCb(focusState);
                        }

                        // On interruption end with shouldResume, re-activate session and restart streams
                        if (!began && shouldResume) {
                            StreamRestartCallback restartCb;
                            {
                                std::lock_guard<std::mutex> lk(this->mutex_);
                                restartCb = this->restartCallback_;
                            }
                            if (restartCb) {
                                dispatch_async(dispatch_get_main_queue(), ^{
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
                    }];

        // Media services lost — reset route handler + emit permanent focus loss
        // so JS handlers observe the same AudioFocusState::Lost alphabet as Android.
        mediaServicesLostObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionMediaServicesWereLostNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        MEDIA_LOG_E("AVAudioSession: media services were LOST");
                        this->interrupted_.store(true, std::memory_order_release);
                        this->routeHandler_.reset();
                        AudioFocusCallback focusCb;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            focusCb = this->audioFocusCallback_;
                        }
                        if (focusCb) focusCb(AudioFocusState::Lost);
                    }];

        // Media services reset — reconfigure, restart, re-detect
        mediaServicesResetObserver_ = (__bridge_retained void*)[center
            addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:nil
                         queue:opQueue
                    usingBlock:^(NSNotification*) {
                        MEDIA_LOG_W("AVAudioSession: media services were reset");
                        this->interrupted_.store(false, std::memory_order_release);
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
                            deviceId
                        });
                        // Counterpart to MediaServicesWereLost → ::Lost: surface
                        // the recovery as ::Gained so JS state machines can restart.
                        AudioFocusCallback focusCb;
                        {
                            std::lock_guard<std::mutex> lk(this->mutex_);
                            focusCb = this->audioFocusCallback_;
                        }
                        if (focusCb) focusCb(AudioFocusState::Gained);
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

}  // namespace media
