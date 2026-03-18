#pragma once

#include <jsi/jsi.h>
#include <string>
#include <cstdint>
#include <cmath>
#include <limits>

namespace jsiutil {

namespace jsi = facebook::jsi;

inline int getIntOr(jsi::Runtime& rt, jsi::Object& obj, const char* name, int defaultVal) {
    if (!obj.hasProperty(rt, name)) return defaultVal;

    jsi::Value prop = obj.getProperty(rt, name);
    if (!prop.isNumber()) return defaultVal;

    double val = prop.asNumber();
    if (!std::isfinite(val) ||
        val < static_cast<double>(std::numeric_limits<int>::min()) ||
        val > static_cast<double>(std::numeric_limits<int>::max())) {
        return defaultVal;
    }
    return static_cast<int>(val);
}

inline double getDoubleOr(jsi::Runtime& rt, jsi::Object& obj, const char* name, double defaultVal) {
    if (!obj.hasProperty(rt, name)) return defaultVal;

    jsi::Value prop = obj.getProperty(rt, name);
    if (!prop.isNumber()) return defaultVal;

    double val = prop.asNumber();
    return std::isfinite(val) ? val : defaultVal;
}

inline float getFloatOr(jsi::Runtime& rt, jsi::Object& obj, const char* name, float defaultVal) {
    return static_cast<float>(getDoubleOr(rt, obj, name, static_cast<double>(defaultVal)));
}

inline bool getBoolOr(jsi::Runtime& rt, jsi::Object& obj, const char* name, bool defaultVal) {
    if (!obj.hasProperty(rt, name)) return defaultVal;

    jsi::Value prop = obj.getProperty(rt, name);
    if (!prop.isBool()) return defaultVal;

    return prop.getBool();
}

inline std::string getStringOr(jsi::Runtime& rt, jsi::Object& obj, const char* name, const std::string& defaultVal) {
    if (!obj.hasProperty(rt, name)) return defaultVal;

    jsi::Value prop = obj.getProperty(rt, name);
    if (!prop.isString()) return defaultVal;

    return prop.asString(rt).utf8(rt);
}

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

inline int64_t safeInt64Arg(jsi::Runtime& rt, const jsi::Value& val, const char* name) {
    double d = val.asNumber();
    if (!std::isfinite(d) ||
        d < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        d > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        throw jsi::JSError(rt, std::string(name) + " must be a finite integer in range");
    }
    return static_cast<int64_t>(d);
}

inline float safeFloatArg(jsi::Runtime& rt, const jsi::Value& val, const char* name) {
    double d = val.asNumber();
    if (!std::isfinite(d)) {
        throw jsi::JSError(rt, std::string(name) + " must be finite");
    }
    return static_cast<float>(d);
}

namespace detail {

constexpr uint32_t fnv1a(const char* s, size_t len) noexcept {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint32_t>(s[i]);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace detail

constexpr uint32_t operator""_h(const char* s, size_t len) noexcept {
    return detail::fnv1a(s, len);
}

inline uint32_t hash(const std::string& s) noexcept {
    return detail::fnv1a(s.data(), s.size());
}

inline uint32_t hash(const char* s, size_t len) noexcept {
    return detail::fnv1a(s, len);
}

// Hash collision safety: JSIMETHOD expands to a switch-case label with
// a constexpr hash. Duplicate hashes produce duplicate case labels, which
// is a compile error in C++. This provides implicit collision detection.
#define JSIMETHOD(str, id) \
    case ::jsiutil::detail::fnv1a(str, sizeof(str) - 1): \
        if (name.size() == sizeof(str) - 1) return id; \
        break

// Explicit compile-time collision assertion for use outside switch contexts.
// Usage: static_assert(jsiutil::detail::hashes_unique("a", "b", "c"));
namespace detail {

template<typename... Strs>
constexpr bool hashes_unique(Strs... strs) noexcept {
    uint32_t hashes[] = { fnv1a(strs, __builtin_strlen(strs))... };
    for (size_t i = 0; i < sizeof...(strs); ++i) {
        for (size_t j = i + 1; j < sizeof...(strs); ++j) {
            if (hashes[i] == hashes[j]) return false;
        }
    }
    return true;
}

}  // namespace detail

}  // namespace jsiutil
