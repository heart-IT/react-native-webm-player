// getAvailableAudioRoutes() + getAvailableAudioDevices() — enumerate the set
// of routes JS can choose from (deduplicated by type) and the per-device
// list (every BT/wired/USB device with name + platform UID).
//
// Member functions of AudioSessionManager; class definition lives in
// AudioSessionManager.h.
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

namespace media {

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

}  // namespace media
