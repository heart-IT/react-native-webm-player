// JSI bindings for audio routing queries + setters.
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerAudioRoutingMethods() {
    auto& t = dispatch_;

    t.method("setAudioRoute", 2, [](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setAudioRoute requires route (number)");
        int route = jsiutil::safeIntArg(rt, args[0], "route");
        if (route < static_cast<int>(media::AudioRoute::Earpiece) ||
            route > static_cast<int>(media::AudioRoute::UsbDevice))
            throw jsi::JSError(rt, "setAudioRoute: route out of range");
        std::string deviceId;
        if (count >= 2 && args[1].isString()) deviceId = args[1].getString(rt).utf8(rt);
        return jsi::Value(media::AudioSessionManager::instance().setAudioRoute(
            static_cast<media::AudioRoute>(route), deviceId));
    });
    t.method("getAvailableAudioRoutes", 0, [](jsi::Runtime& rt, const jsi::Value*, size_t) {
        auto routes = media::AudioSessionManager::instance().getAvailableAudioRoutes();
        jsi::Array result(rt, routes.size());
        for (size_t i = 0; i < routes.size(); ++i)
            result.setValueAtIndex(rt, i, static_cast<int>(routes[i]));
        return result;
    });
    t.method("getAvailableAudioDevices", 0, [](jsi::Runtime& rt, const jsi::Value*, size_t) {
        return jsiutil::marshalAudioDevices(rt,
            media::AudioSessionManager::instance().getAvailableAudioDevices());
    });
    t.method("getCurrentAudioRoute", 0, [](jsi::Runtime&, const jsi::Value*, size_t) {
        media::AudioSessionManager::instance().ensureRouteDetected();
        return jsi::Value(static_cast<int>(media::AudioSessionManager::instance().currentRoute()));
    });
}

}  // namespace mediamodule
