// Current-route detection from AVAudioSession.currentRoute.
// LE Audio classification (uni- vs bidirectional) lives here too — it's a
// helper used by both detectCurrentRoute and external callers in
// AudioRouteSetter / AudioDeviceEnumerator.
//
// Member functions of AudioSessionManager; class definition lives in
// AudioSessionManager.h.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>

namespace {

// Probe availableInputs for an LE Audio port matching the given output UID.
// Presence in availableInputs implies bidirectional (treated like SCO/HFP);
// absence implies output-only (treated like A2DP).
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

void AudioSessionManager::ensureRouteDetected() noexcept {
    if (currentRoute_.load(std::memory_order_acquire) != AudioRoute::Unknown) return;
    detectCurrentRoute();
}

void AudioSessionManager::detectCurrentRoute() noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        AVAudioSessionRouteDescription* route = session.currentRoute;

        AudioRoute newRoute = AudioRoute::Unknown;
        bool isBuiltinPort = false;

        for (AVAudioSessionPortDescription* output in route.outputs) {
            NSString* portType = output.portType;

            if ([portType isEqualToString:AVAudioSessionPortBuiltInReceiver]) {
                newRoute = AudioRoute::Earpiece;
                isBuiltinPort = true;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBuiltInSpeaker]) {
                newRoute = AudioRoute::Speaker;
                isBuiltinPort = true;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortHeadphones]) {
                newRoute = AudioRoute::WiredHeadset;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
                newRoute = AudioRoute::BluetoothSco;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothA2DP]) {
                newRoute = AudioRoute::BluetoothA2dp;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
                // LE Audio can be unidirectional (output-only, like A2DP) or
                // bidirectional (input+output, like HFP/SCO). Check availableInputs
                // for a matching LE port to distinguish.
                // Cache the classification per-UID so mid-session input port changes
                // don't reclassify a bidirectional device as A2DP.
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
                } else {
                    newRoute = AudioRoute::BluetoothA2dp;
                }
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortUSBAudio]) {
                newRoute = AudioRoute::UsbDevice;
                break;
            } else if ([portType isEqualToString:AVAudioSessionPortAirPlay] ||
                       [portType isEqualToString:AVAudioSessionPortHDMI] ||
                       [portType isEqualToString:AVAudioSessionPortCarAudio]) {
                // Output-only devices with no echo path, treat like external speaker
                newRoute = AudioRoute::Speaker;
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
        // external devices use real AVAudioSession UIDs for correct dedup.
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

        if (oldRoute != newRoute) {
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
            MEDIA_LOG_I("AudioRoute: %s -> %s",
                        audioRouteToString(oldRoute), audioRouteToString(newRoute));
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

}  // namespace media
