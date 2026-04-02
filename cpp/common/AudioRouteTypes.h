// Audio routing types shared between iOS and Android.
// Defines route enums, device info, focus state, and callback types
// used by RouteHandler and platform AudioSessionManagers.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace media {

// Audio output destination. Values must match TypeScript AudioRoute enum in src/index.tsx.
enum class AudioRoute : uint8_t {
    Unknown = 0,
    Earpiece = 1,
    Speaker = 2,
    WiredHeadset = 3,
    BluetoothSco = 4,
    BluetoothA2dp = 5,
    UsbDevice = 6
};

static_assert(static_cast<int>(AudioRoute::UsbDevice) < 32,
              "Bitset dedup in getAvailableAudioRoutes requires AudioRoute values < 32");

inline const char* audioRouteToString(AudioRoute route) noexcept {
    switch (route) {
        case AudioRoute::Earpiece:       return "Earpiece";
        case AudioRoute::Speaker:        return "Speaker";
        case AudioRoute::WiredHeadset:   return "WiredHeadset";
        case AudioRoute::BluetoothSco:   return "BluetoothSco";
        case AudioRoute::BluetoothA2dp:  return "BluetoothA2dp";
        case AudioRoute::UsbDevice:      return "UsbDevice";
        default:                         return "Unknown";
    }
}

inline bool isBluetoothRoute(AudioRoute route) noexcept {
    return route == AudioRoute::BluetoothA2dp || route == AudioRoute::BluetoothSco;
}

inline bool isBuiltinRoute(AudioRoute route) noexcept {
    return route == AudioRoute::Speaker || route == AudioRoute::Earpiece;
}

struct AudioDeviceInfo {
    AudioRoute route{AudioRoute::Unknown};
    std::string deviceName;
    std::string deviceId;
};

enum class AudioFocusState : uint8_t {
    Gained = 0,
    Lost,
    LostTransient,
    LostTransientCanDuck
};

using AudioRouteCallback = std::function<void(AudioRoute newRoute)>;
using AudioFocusCallback = std::function<void(AudioFocusState state)>;
using StreamRestartCallback = std::function<void()>;

struct RouteDetectionResult {
    AudioRoute route{AudioRoute::Unknown};
    std::string deviceId;
};

}  // namespace media
