// Debug logging macros for the media pipeline.
// Routes to __android_log_print (Android), os_log (iOS), or stderr (host tests).
// NEVER call from a real-time audio callback — logging performs I/O.
#pragma once

#include <atomic>
#include <cstdio>

#include "Platform.h"

#if MEDIA_PLATFORM_ANDROID
#include <android/log.h>
#elif MEDIA_PLATFORM_IOS
#include <os/log.h>
#endif

namespace media {

namespace log {

constexpr const char* kTag = "WebmPlayer";

enum class Level : int {
#if MEDIA_PLATFORM_ANDROID
  Verbose = ANDROID_LOG_VERBOSE,
  Debug = ANDROID_LOG_DEBUG,
  Info = ANDROID_LOG_INFO,
  Warn = ANDROID_LOG_WARN,
  Error = ANDROID_LOG_ERROR
#else
  Verbose = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4
#endif
};

// Mutable at runtime so tests can silence log-heavy paths (the buffer logs on
// every overflow, which would otherwise flood a saturation test).
inline std::atomic<Level>& minLevel() noexcept {
  static std::atomic<Level> level{Level::Info};
  return level;
}

inline Level getMinLevel() noexcept {
  return minLevel().load(std::memory_order_relaxed);
}

#if MEDIA_PLATFORM_ANDROID

template <typename... Args>
inline void log(Level level, const char* fmt, Args... args) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    __android_log_print(static_cast<int>(level), kTag, fmt, args...);
  }
}

inline void log(Level level, const char* msg) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    __android_log_print(static_cast<int>(level), kTag, "%s", msg);
  }
}

#elif MEDIA_PLATFORM_IOS

namespace detail {

inline os_log_t getOsLog() noexcept {
  static os_log_t handle = os_log_create("com.heartit.webmplayer", "WebmPlayer");
  return handle;
}

inline os_log_type_t toOsLogType(Level level) noexcept {
  switch (level) {
    case Level::Verbose:
    case Level::Debug:
      return OS_LOG_TYPE_DEBUG;
    case Level::Info:
      // Not OS_LOG_TYPE_INFO: info-type messages are memory-only by default and
      // evicted before a post-repro `log collect` can archive them. On-device
      // diagnosis reads collected archives, so Info must persist.
      return OS_LOG_TYPE_DEFAULT;
    case Level::Error:
      return OS_LOG_TYPE_ERROR;
    case Level::Warn:
    default:
      return OS_LOG_TYPE_DEFAULT;
  }
}

}  // namespace detail

template <typename... Args>
inline void log(Level level, const char* fmt, Args... args) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), fmt, args...);
    os_log_with_type(detail::getOsLog(), detail::toOsLogType(level),
                     "%{public}s", buffer);
  }
}

inline void log(Level level, const char* msg) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    os_log_with_type(detail::getOsLog(), detail::toOsLogType(level),
                     "%{public}s", msg);
  }
}

#else  // MEDIA_PLATFORM_HOST

template <typename... Args>
inline void log(Level level, const char* fmt, Args... args) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    fprintf(stderr, "[%s] ", kTag);
    fprintf(stderr, fmt, args...);
    fputc('\n', stderr);
  }
}

inline void log(Level level, const char* msg) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    fprintf(stderr, "[%s] %s\n", kTag, msg);
  }
}

#endif

}  // namespace log

#define MEDIA_LOG_V(fmt, ...) \
  ::media::log::log(::media::log::Level::Verbose, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_D(fmt, ...) \
  ::media::log::log(::media::log::Level::Debug, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_I(fmt, ...) \
  ::media::log::log(::media::log::Level::Info, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_W(fmt, ...) \
  ::media::log::log(::media::log::Level::Warn, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_E(fmt, ...) \
  ::media::log::log(::media::log::Level::Error, fmt, ##__VA_ARGS__)

}  // namespace media
