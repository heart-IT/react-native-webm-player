// Monotonic microsecond clock for timing measurements.
// Uses clock_gettime(CLOCK_MONOTONIC) on Android, mach_absolute_time() on iOS.
#pragma once

#include <atomic>
#include <cstdint>
#include "Platform.h"

#if MEDIA_PLATFORM_ANDROID
    #include <ctime>
#elif MEDIA_PLATFORM_IOS
    #include <mach/mach_time.h>
#else
    #include <chrono>
#endif

namespace media {

#if MEDIA_PLATFORM_ANDROID

inline int64_t nowUs() noexcept {
    struct timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

// No-op on Android — CLOCK_MONOTONIC needs no priming.
inline void primeClock() noexcept {}

#elif MEDIA_PLATFORM_IOS

namespace detail {

// constinit: the no-__cxa_guard property these rely on is compiler-enforced,
// not just asserted in a comment. Populated once via a race-safe publish on
// first call; subsequent calls see the cached values via atomic reads.
inline constinit std::atomic<uint64_t> g_machNumer{0};
inline constinit std::atomic<uint64_t> g_machDenom{0};

inline void primeMachTimebase() noexcept {
    if (g_machDenom.load(std::memory_order_acquire) != 0) return;
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    // denom written last with release ordering so a reader that observes
    // denom != 0 is guaranteed to see the paired numer.
    g_machNumer.store(info.numer, std::memory_order_relaxed);
    g_machDenom.store(info.denom, std::memory_order_release);
}

}  // namespace detail

// Call once from the JS/init thread before any audio callback fires. Safe to
// call multiple times. After priming, nowUs() is a pure pair of atomic loads.
inline void primeClock() noexcept {
    detail::primeMachTimebase();
}

inline int64_t nowUs() noexcept {
    uint64_t denom = detail::g_machDenom.load(std::memory_order_acquire);
    if (denom == 0) {
        detail::primeMachTimebase();
        denom = detail::g_machDenom.load(std::memory_order_acquire);
    }
    uint64_t numer = detail::g_machNumer.load(std::memory_order_relaxed);
    uint64_t ticks = mach_absolute_time();
    // Use __uint128_t to prevent overflow: on x86_64 simulator the timebase numer
    // can be ~1e9, and ticks * 1e9 overflows uint64_t after ~18 seconds.
    auto ns = static_cast<__uint128_t>(ticks) * numer / denom;
    return static_cast<int64_t>(static_cast<uint64_t>(ns / 1000));
}

#else  // MEDIA_PLATFORM_HOST — desktop builds of the shared C++ (test suites).

inline int64_t nowUs() noexcept {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

inline void primeClock() noexcept {}

#endif

}  // namespace media
