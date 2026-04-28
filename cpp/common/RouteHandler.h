// Reactive audio routing handler — receives OS route-change events and decides:
// dedup (same route), restart audio stream, reset drift compensation, fire JS callback.
// Zero state machines, zero timers. The OS manages device state internally.
// Thread safety: all methods must be called from the same thread (platform audio session thread).
#pragma once

#include "AudioRouteTypes.h"
#include "MediaLog.h"
#include <cstdint>
#include <functional>
#include <string>

namespace media {

struct RouteCallbacks {
    std::function<void()> restartStreams;
    std::function<void(AudioRoute)> fireJsCallback;
    std::function<void()> resetDrift;
};

class RouteHandler {
public:
    void setCallbacks(RouteCallbacks cbs) noexcept { cbs_ = std::move(cbs); }

    void onInitialRoute(const RouteDetectionResult& result) noexcept {
        confirmed_ = {result.route, result.deviceId};
        initialized_ = true;
        MEDIA_LOG_I("RouteHandler: initial route=%s device=%s",
                    audioRouteToString(result.route),
                    result.deviceId.c_str());
        if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route);
    }

    void onRouteDetected(const RouteDetectionResult& result) noexcept {
        if (!initialized_) return;

        if (result.route == confirmed_.route &&
            result.deviceId == confirmed_.deviceId) {
            if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route);
            return;
        }

        AudioRoute oldRoute = confirmed_.route;
        confirmed_ = {result.route, result.deviceId};
        ++routeChangeCount_;

        MEDIA_LOG_I("RouteHandler: route changed %s -> %s device=%s",
                    audioRouteToString(oldRoute),
                    audioRouteToString(confirmed_.route),
                    confirmed_.deviceId.c_str());

        if (result.route != oldRoute) {
            if (cbs_.resetDrift) cbs_.resetDrift();
        }

        if (cbs_.fireJsCallback) cbs_.fireJsCallback(confirmed_.route);

        if (needsRestart(oldRoute, confirmed_.route)) {
            MEDIA_LOG_I("RouteHandler: restart needed (%s -> %s)",
                        audioRouteToString(oldRoute),
                        audioRouteToString(confirmed_.route));
            if (cbs_.restartStreams) cbs_.restartStreams();
        }
    }

    void reset() noexcept {
        confirmed_ = {};
        initialized_ = false;
    }

    [[nodiscard]] AudioRoute currentRoute() const noexcept { return confirmed_.route; }
    [[nodiscard]] const std::string& currentDeviceId() const noexcept { return confirmed_.deviceId; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    // Monotonic count of confirmed route changes since construction.
    // Session-long signal so flapping is visible across resetStream boundaries.
    // Torn reads across threads are acceptable: the counter is monotonic, the
    // read site only needs an approximate value, and 64-bit loads are atomic
    // on all supported ABIs.
    [[nodiscard]] uint64_t routeChangeCount() const noexcept {
        return routeChangeCount_;
    }

    static bool needsRestart(AudioRoute from, AudioRoute to) noexcept {
        if (from == to) return false;
        if (from == AudioRoute::Unknown) return false;
        // Builtin transitions (Speaker <-> Earpiece): no restart, same hardware path
        if (isBuiltinRoute(from) && isBuiltinRoute(to)) return false;
        // A2DP <-> builtin: no restart (output-only, same audio path)
        if ((from == AudioRoute::BluetoothA2dp && isBuiltinRoute(to)) ||
            (isBuiltinRoute(from) && to == AudioRoute::BluetoothA2dp)) {
            return false;
        }
        return true;
    }

private:
    struct ConfirmedRoute {
        AudioRoute route{AudioRoute::Unknown};
        std::string deviceId;
    } confirmed_;

    bool initialized_{false};
    RouteCallbacks cbs_;
    uint64_t routeChangeCount_{0};
};

}  // namespace media
