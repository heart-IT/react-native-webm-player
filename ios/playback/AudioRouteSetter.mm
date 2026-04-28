// setAudioRoute() + executeRouteChangePlatform() — apply a JS-requested route
// to AVAudioSession via overrideOutputAudioPort and setPreferredInput.
//
// Member functions of AudioSessionManager; class definition lives in
// AudioSessionManager.h.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

namespace media {

bool AudioSessionManager::setAudioRoute(AudioRoute route, const std::string& deviceId) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
        MEDIA_LOG_W("setAudioRoute: called before audio session initialized");
        return false;
    }

    // Dispatch on the notification serial queue to serialize with route change handlers.
    __block bool success = false;
    dispatch_queue_t dq = (__bridge dispatch_queue_t)notificationDispatchQueue_;
    dispatch_sync(dq, ^{
        success = this->executeRouteChangePlatform(route, deviceId);
    });

    return success;
}

bool AudioSessionManager::executeRouteChangePlatform(AudioRoute route, const std::string& deviceId) noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // For playback-only, route changes are simpler — no mic input to manage.
        // We use setPreferredInput for BT and override for speaker/earpiece.
        switch (route) {
            case AudioRoute::Speaker: {
                BOOL success = [session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: overrideOutputAudioPort(Speaker) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                return true;
            }

            case AudioRoute::Earpiece: {
                // iPad has no earpiece receiver
                if ([[UIDevice currentDevice] userInterfaceIdiom] != UIUserInterfaceIdiomPhone) {
                    MEDIA_LOG_W("setAudioRoute: earpiece not available on this device");
                    return false;
                }

                BOOL success = [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
                if (!success) {
                    MEDIA_LOG_E("setAudioRoute: overrideOutputAudioPort(None) failed: %s",
                                error ? error.localizedDescription.UTF8String : "unknown");
                    return false;
                }
                return true;
            }

            case AudioRoute::BluetoothSco:
            case AudioRoute::BluetoothA2dp: {
                // Clear any speaker override so BT output takes effect
                [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];

                NSString* targetUID = deviceId.empty() ? nil : [NSString stringWithUTF8String:deviceId.c_str()];
                NSArray<AVAudioSessionPortDescription*>* availableInputs = session.availableInputs;

                auto selectPort = [&](AVAudioSessionPortDescription* port) -> bool {
                    if (targetUID && ![port.UID isEqualToString:targetUID]) return false;
                    BOOL success = [session setPreferredInput:port error:&error];
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

                // Fallback: any BT port
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
                return false;
            }

            case AudioRoute::WiredHeadset: {
                NSString* targetUID = deviceId.empty() ? nil : [NSString stringWithUTF8String:deviceId.c_str()];
                for (AVAudioSessionPortDescription* port in session.availableInputs) {
                    if ([port.portType isEqualToString:AVAudioSessionPortHeadsetMic] ||
                        [port.portType isEqualToString:AVAudioSessionPortHeadphones]) {
                        if (targetUID && ![port.UID isEqualToString:targetUID]) continue;
                        [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
                        BOOL success = [session setPreferredInput:port error:&error];
                        if (success) return true;
                        MEDIA_LOG_E("setAudioRoute: setPreferredInput(WiredHeadset) failed: %s",
                                    error ? error.localizedDescription.UTF8String : "unknown");
                    }
                }
                // Headphone-only devices (no mic) won't appear in availableInputs.
                // If already active on the current output route, consider it routed.
                for (AVAudioSessionPortDescription* output in session.currentRoute.outputs) {
                    if ([output.portType isEqualToString:AVAudioSessionPortHeadphones]) {
                        if (targetUID && ![output.UID isEqualToString:targetUID]) continue;
                        [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
                        return true;
                    }
                }
                MEDIA_LOG_W("setAudioRoute: no wired headset available");
                return false;
            }

            case AudioRoute::UsbDevice: {
                NSString* targetUID = deviceId.empty() ? nil : [NSString stringWithUTF8String:deviceId.c_str()];
                for (AVAudioSessionPortDescription* port in session.availableInputs) {
                    if ([port.portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                        if (targetUID && ![port.UID isEqualToString:targetUID]) continue;
                        [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
                        BOOL success = [session setPreferredInput:port error:&error];
                        if (success) return true;
                        MEDIA_LOG_E("setAudioRoute: setPreferredInput(USB) failed: %s",
                                    error ? error.localizedDescription.UTF8String : "unknown");
                    }
                }
                // USB audio-only devices (no mic) won't appear in availableInputs.
                // If already active on the current output route, consider it routed.
                for (AVAudioSessionPortDescription* output in session.currentRoute.outputs) {
                    if ([output.portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                        if (targetUID && ![output.UID isEqualToString:targetUID]) continue;
                        [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:nil];
                        return true;
                    }
                }
                MEDIA_LOG_W("setAudioRoute: no USB device available");
                return false;
            }

            default:
                MEDIA_LOG_W("setAudioRoute: unsupported route %d", static_cast<int>(route));
                return false;
        }
    }
}

}  // namespace media
