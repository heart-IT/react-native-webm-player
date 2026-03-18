#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace media {

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

enum class EchoMode : uint8_t {
    HardwareAEC = 0,
    BluetoothAEC = 1,
    None = 2
};

enum class AudioFocusState : uint8_t {
    None = 0,
    Gain = 1,
    Loss = 2,
    LossTransient = 3,
    LossTransientCanDuck = 4
};

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

inline const char* echoModeToString(EchoMode mode) noexcept {
    switch (mode) {
        case EchoMode::HardwareAEC:  return "HardwareAEC";
        case EchoMode::BluetoothAEC: return "BluetoothAEC";
        case EchoMode::None:         return "None";
        default:                     return "Unknown";
    }
}

inline bool isBluetoothRoute(AudioRoute route) noexcept {
    return route == AudioRoute::BluetoothSco || route == AudioRoute::BluetoothA2dp;
}

struct AudioDeviceInfo {
    AudioRoute route{AudioRoute::Unknown};
    std::string deviceName;
    std::string deviceId;
};

using AudioRouteCallback = std::function<void(AudioRoute newRoute, EchoMode echoMode)>;
using StreamRestartCallback = std::function<void()>;
using AudioFocusCallback = std::function<void(AudioFocusState state)>;
using LifecycleCallback = std::function<void(bool foreground)>;

struct RouteDetectionResult {
    AudioRoute route{AudioRoute::Unknown};
    EchoMode echoMode{EchoMode::HardwareAEC};
    std::string deviceId;
};

inline bool isBuiltinRoute(AudioRoute route) noexcept {
    return route == AudioRoute::Speaker || route == AudioRoute::Earpiece;
}

}  // namespace media
