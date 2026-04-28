// Periodic device-list poll. iOS doesn't fire OldDeviceUnavailable when a
// BT device is powered off while not the active route, so we sweep the
// availableInputs list every 3s and synthesize a route callback if it changes.
//
// Member functions of AudioSessionManager; class definition lives in
// AudioSessionManager.h.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>

#include <algorithm>

namespace media {

void AudioSessionManager::startDevicePollTimer() noexcept {
    if (devicePollTimer_) return;
    dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
    if (!dq) return;

    // Snapshot the initial device list on the serial notification queue so it
    // can't race with a concurrent route-change observer that also writes
    // lastPolledDeviceUIDs_. (Observer is already live by the time this runs —
    // registerNotifications() has returned.)
    dispatch_sync(dq, ^{
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
    });

    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dq);
    // Poll every 3s with 1s leeway — iOS doesn't fire OldDeviceUnavailable when
    // a BT device is powered off while not the active route (confirmed iOS bug).
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
        NSUInteger currentCount = inputs.count;
        if (currentCount == lastPolledDeviceUIDs_.size()) {
            // Count matches — do full UID comparison only if counts equal
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
        AudioRouteCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = routeCallback_;
        }
        if (cb) cb(route);
    }
}

}  // namespace media
