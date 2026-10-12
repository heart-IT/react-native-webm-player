// Logging macros for the media pipeline: os_log on iOS, stderr on the host
// (test suites).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "Platform.h"

#if MEDIA_PLATFORM_IOS
#include <os/log.h>
#endif

namespace media {

namespace log {

constexpr const char* kTag = "WebmPlayer";

enum class Level : int {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3
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

#if MEDIA_PLATFORM_IOS

namespace detail {

inline os_log_t getOsLog() noexcept {
  static os_log_t handle = os_log_create("com.heartit.webmplayer", "WebmPlayer");
  return handle;
}

inline os_log_type_t toOsLogType(Level level) noexcept {
  switch (level) {
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

// `devicectl device process launch --console` forwards a device app's stdio
// but not its os_log, and reading os_log off a device needs a root-collected
// archive. With WEBM_LOG_STDERR set in the process environment, every line is
// mirrored to stderr so that console shows it.
inline bool mirrorToStderr() noexcept {
  static const bool on = getenv("WEBM_LOG_STDERR") != nullptr;
  return on;
}

// Milliseconds since the first log line: the console has no timestamps of
// its own, and ordering against a background trip needs them.
inline long long millisSinceStart() noexcept {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

inline void emit(Level level, const char* msg) noexcept {
  os_log_with_type(getOsLog(), toOsLogType(level), "%{public}s", msg);
  if (mirrorToStderr()) {
    fprintf(stderr, "%8lld [%s] %s\n", millisSinceStart(), kTag, msg);
  }
}

}  // namespace detail

template <typename... Args>
inline void log(Level level, const char* fmt, Args... args) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), fmt, args...);
    detail::emit(level, buffer);
  }
}

inline void log(Level level, const char* msg) noexcept {
  if (static_cast<int>(level) >= static_cast<int>(getMinLevel())) {
    detail::emit(level, msg);
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

#define MEDIA_LOG_D(fmt, ...) \
  ::media::log::log(::media::log::Level::Debug, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_I(fmt, ...) \
  ::media::log::log(::media::log::Level::Info, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_W(fmt, ...) \
  ::media::log::log(::media::log::Level::Warn, fmt, ##__VA_ARGS__)
#define MEDIA_LOG_E(fmt, ...) \
  ::media::log::log(::media::log::Level::Error, fmt, ##__VA_ARGS__)

}  // namespace media
