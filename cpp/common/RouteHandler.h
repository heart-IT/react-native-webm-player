#pragma once

#include "AudioRouteTypes.h"
#include "MediaLog.h"
#include <functional>
#include <string>

namespace media {

struct RouteCallbacks {
    std::function<void()> restartStreams;
    std::function<void(AudioRoute, EchoMode)> fireJsCallback;
    std::function<void()> resetDrift;
};

class RouteHandler {
public:
    void setCallbacks(RouteCallbacks cbs) noexcept { cbs_ = std::move(cbs); }

    void onInitialRoute(const RouteDetectionResult& result) noexcept {
        confirmed_ = {result.route, result.echoMode, result.deviceId};
        initialized_ = true;
        MEDIA_LOG_I("RouteHandler: initial route=%s echo=%s device=%s",
                    audioRouteToString(result.route),
                    echoModeToString(result.echoMode),
                    result.deviceId.c_str());
        if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route, confirmed_.echoMode);
    }

    void onRouteDetected(const RouteDetectionResult& result) noexcept {
        if (!initialized_) return;

        if (result.route == confirmed_.route &&
            result.echoMode == confirmed_.echoMode &&
            result.deviceId == confirmed_.deviceId) {
            // Dedup — device list may have changed, fire callback for UI update
            if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route, confirmed_.echoMode);
            return;
        }

        AudioRoute oldRoute = confirmed_.route;
        confirmed_ = {result.route, result.echoMode, result.deviceId};

        MEDIA_LOG_I("RouteHandler: route changed %s -> %s echo=%s device=%s",
                    audioRouteToString(oldRoute),
                    audioRouteToString(confirmed_.route),
                    echoModeToString(confirmed_.echoMode),
                    confirmed_.deviceId.c_str());

        if (result.route != oldRoute) {
            if (cbs_.resetDrift) cbs_.resetDrift();
        }

        if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route, confirmed_.echoMode);

        if (needsRestart(oldRoute, confirmed_.route)) {
            MEDIA_LOG_I("RouteHandler: restart needed (%s -> %s)",
                        audioRouteToString(oldRoute),
                        audioRouteToString(confirmed_.route));
            if (cbs_.restartStreams) cbs_.restartStreams();
        }
    }

    void onExplicitRouteRequested(AudioRoute route) noexcept {
        if (route == AudioRoute::Speaker) {
            speakerPreferred_ = true;
        } else if (route != AudioRoute::Unknown) {
            speakerPreferred_ = false;
        }
    }

    void reset() noexcept {
        confirmed_ = {};
        speakerPreferred_ = true;
        initialized_ = false;
    }

    [[nodiscard]] AudioRoute currentRoute() const noexcept { return confirmed_.route; }
    [[nodiscard]] EchoMode currentEchoMode() const noexcept { return confirmed_.echoMode; }
    [[nodiscard]] const std::string& currentDeviceId() const noexcept { return confirmed_.deviceId; }
    [[nodiscard]] bool speakerPreferred() const noexcept { return speakerPreferred_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    static bool needsRestart(AudioRoute from, AudioRoute to) noexcept {
        if (from == to) return false;
        if (from == AudioRoute::Unknown) return false;

        // Speaker <-> earpiece: same hardware path, no restart
        if (isBuiltinRoute(from) && isBuiltinRoute(to)) return false;

        // A2DP <-> builtin: A2DP is output-only, capture path unchanged
        if ((from == AudioRoute::BluetoothA2dp && isBuiltinRoute(to)) ||
            (to == AudioRoute::BluetoothA2dp && isBuiltinRoute(from))) {
            return false;
        }

        return true;
    }

private:
    struct ConfirmedRoute {
        AudioRoute route{AudioRoute::Unknown};
        EchoMode echoMode{EchoMode::HardwareAEC};
        std::string deviceId;
    } confirmed_;

    bool speakerPreferred_{true};
    bool initialized_{false};
    RouteCallbacks cbs_;
};

}  // namespace media
