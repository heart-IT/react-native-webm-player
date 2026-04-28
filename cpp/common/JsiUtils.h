// JSI utility functions for safe argument extraction and buffer access.
// Used at the JS↔native boundary in MediaPipelineModule and VideoPipelineModule.
#pragma once

#include <jsi/jsi.h>
#include <string>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>
#include "AudioRouteTypes.h"

namespace jsiutil {

namespace jsi = facebook::jsi;

// Safe argument extraction for JSI method handlers.
// Prevents undefined behavior from static_cast<int>(NaN/Infinity).
inline int safeIntArg(jsi::Runtime& rt, const jsi::Value& val, const char* name) {
    double d = val.asNumber();
    if (!std::isfinite(d) ||
        d < static_cast<double>(std::numeric_limits<int>::min()) ||
        d > static_cast<double>(std::numeric_limits<int>::max())) {
        throw jsi::JSError(rt, std::string(name) + " must be a finite integer in range");
    }
    return static_cast<int>(d);
}


inline float safeFloatArg(jsi::Runtime& rt, const jsi::Value& val, const char* name) {
    double d = val.asNumber();
    if (!std::isfinite(d)) {
        throw jsi::JSError(rt, std::string(name) + " must be finite");
    }
    return static_cast<float>(d);
}

// Shared buffer extraction for JSI ArrayBuffer / TypedArray arguments.
// Used by both MediaPipelineModule and VideoPipelineModule on both platforms.
struct BufferView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit operator bool() const noexcept { return data && size > 0; }
};

inline BufferView extractBuffer(jsi::Runtime& rt, const jsi::Value& bufferVal) {
    BufferView result;
    if (!bufferVal.isObject()) return result;

    jsi::Object obj = bufferVal.getObject(rt);

    if (obj.isArrayBuffer(rt)) {
        jsi::ArrayBuffer ab = obj.getArrayBuffer(rt);
        uint8_t* ptr = ab.data(rt);
        size_t sz = ab.size(rt);
        if (ptr && sz > 0) { result.data = ptr; result.size = sz; }
        return result;
    }

    jsi::Value bufferProp = obj.getProperty(rt, "buffer");
    if (!bufferProp.isObject()) return result;
    jsi::Object bufferObj = bufferProp.getObject(rt);
    if (!bufferObj.isArrayBuffer(rt)) return result;

    jsi::ArrayBuffer ab = bufferObj.getArrayBuffer(rt);
    uint8_t* basePtr = ab.data(rt);
    size_t totalSize = ab.size(rt);
    if (!basePtr || totalSize == 0) return result;

    size_t offset = 0, length = totalSize;
    jsi::Value offsetVal = obj.getProperty(rt, "byteOffset");
    if (offsetVal.isNumber()) {
        double d = offsetVal.asNumber();
        if (std::isfinite(d) && d >= 0) offset = static_cast<size_t>(d);
    }
    jsi::Value lengthVal = obj.getProperty(rt, "byteLength");
    if (lengthVal.isNumber()) {
        double d = lengthVal.asNumber();
        // Zero-length TypedArray views are legal (`new Uint8Array(ab, off, 0)`)
        // and must pass through as zero, not fall through to the default full-buffer.
        if (std::isfinite(d) && d >= 0) length = static_cast<size_t>(d);
    }
    if (offset > totalSize || length > totalSize - offset) return result;
    result.data = basePtr + offset;
    result.size = length;
    return result;
}

// Build a jsi::Array of {route, deviceName, deviceId} objects from a list of
// audio devices. Shared by the route-callback path, the initial-route fire,
// and the getAvailableAudioDevices JSI binding.
inline jsi::Array marshalAudioDevices(jsi::Runtime& rt,
                                      const std::vector<media::AudioDeviceInfo>& devices) {
    jsi::Array arr(rt, devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        jsi::Object obj(rt);
        obj.setProperty(rt, "route", static_cast<int>(devices[i].route));
        obj.setProperty(rt, "deviceName", jsi::String::createFromUtf8(rt, devices[i].deviceName));
        obj.setProperty(rt, "deviceId", jsi::String::createFromUtf8(rt, devices[i].deviceId));
        arr.setValueAtIndex(rt, i, std::move(obj));
    }
    return arr;
}

}  // namespace jsiutil
