#pragma once

#include <cstdint>
#include "Platform.h"

#if MEDIA_PLATFORM_ANDROID
    #include <ctime>
#elif MEDIA_PLATFORM_IOS
    #include <mach/mach_time.h>
#endif

namespace media {

#if MEDIA_PLATFORM_ANDROID

inline int64_t nowUs() noexcept {
    struct timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

#elif MEDIA_PLATFORM_IOS

namespace detail {

struct MachTimeInfo {
    uint64_t numer;
    uint64_t denom;

    MachTimeInfo() noexcept {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        numer = info.numer;
        denom = info.denom;
    }
};

inline const MachTimeInfo& getMachTimeInfo() noexcept {
    static MachTimeInfo info;
    return info;
}

}  // namespace detail

inline int64_t nowUs() noexcept {
    const auto& info = detail::getMachTimeInfo();
    uint64_t ticks = mach_absolute_time();
    // Use __uint128_t to prevent overflow: on x86_64 simulator the timebase numer
    // can be ~1e9, and ticks * 1e9 overflows uint64_t after ~18 seconds.
    auto ns = static_cast<__uint128_t>(ticks) * info.numer / info.denom;
    return static_cast<int64_t>(static_cast<uint64_t>(ns / 1000));
}

#endif

}  // namespace media
