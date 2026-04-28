// JSI bindings for setting JS callbacks (route, health, keyframe-needed, focus).
// Include-only-from: PipelineOrchestrator.h.
#pragma once

namespace mediamodule {

inline void PipelineOrchestrator::registerCallbackMethods() {
    auto& t = dispatch_;

    t.method("setAudioRouteCallback", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1) throw jsi::JSError(rt, "setAudioRouteCallback requires a callback function or null");
        if (callbacks_.setRouteCallback(rt, args[0])) {
            callbacks_.setupAudioRouteCallback(callbackContext());
            callbacks_.fireInitialRouteEvent(rt);
        }
        return jsi::Value(true);
    });
    t.method("setHealthCallback", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1) throw jsi::JSError(rt, "setHealthCallback requires a callback function or null");
        if (callbacks_.setHealthCallback(rt, args[0])) {
            callbacks_.setupHealthWatchdog(callbackContext());
        }
        return jsi::Value(true);
    });
    t.method("setKeyFrameNeededCallback", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1) throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a callback function or null");
        if (callbacks_.setKeyFrameNeededCallback(rt, args[0])) {
            callbacks_.setupKeyFrameNeededCallback(callbackContext());
        }
        return jsi::Value(true);
    });
    t.method("setAudioFocusCallback", 1, [this](jsi::Runtime& rt, const jsi::Value* args, size_t count) {
        if (count < 1) throw jsi::JSError(rt, "setAudioFocusCallback requires a callback function or null");
        return callbacks_.setAudioFocusCallback(rt, args[0], callbackContext());
    });
}

}  // namespace mediamodule
