// Thread priority and CPU affinity configuration for decode and worker threads.
// Attempts SCHED_FIFO (Android) or QoS (iOS) for real-time-adjacent priority,
// with graceful fallback to nice values when RT scheduling is unavailable.
#pragma once

#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <pthread.h>
#include "Platform.h"
#include "MediaLog.h"

#if MEDIA_PLATFORM_ANDROID
    #include <sched.h>
    #include <unistd.h>
    #include <cerrno>
    #include <mutex>
    #include <sys/syscall.h>
    #include <sys/resource.h>
#elif MEDIA_PLATFORM_IOS
    #include <mach/thread_act.h>
    #include <mach/thread_policy.h>
    #include <pthread/qos.h>
#endif

namespace media::thread_affinity {

namespace detail {
    constexpr size_t kMaxThreadNameLen = 15;
}

namespace nice {
    constexpr int kAudioCritical = -16;
    constexpr int kHigh = -10;
    constexpr int kBackground = 10;  // De-prioritized — for transcript/analytics threads
}

struct ConfigureResult {
    bool affinitySet = false;
    bool prioritySet = false;
    bool niceSet = false;

    [[nodiscard]] bool allSucceeded() const noexcept {
        return affinitySet && prioritySet && niceSet;
    }

    [[nodiscard]] bool anySucceeded() const noexcept {
        return affinitySet || prioritySet || niceSet;
    }
};

inline void setThreadName(const char* name) noexcept {
    char truncated[detail::kMaxThreadNameLen + 1];
    size_t len = std::strlen(name);
    size_t copyLen = len < detail::kMaxThreadNameLen ? len : detail::kMaxThreadNameLen;
    std::memcpy(truncated, name, copyLen);
    truncated[copyLen] = '\0';

#if MEDIA_PLATFORM_ANDROID
    pthread_setname_np(pthread_self(), truncated);
#elif MEDIA_PLATFORM_IOS
    pthread_setname_np(truncated);
#endif
}

#if MEDIA_PLATFORM_ANDROID

// Android-specific implementations
namespace android {

inline int getCoreCount() noexcept {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

inline int readCoreMaxFreq(int cpuId) noexcept {
    char path[64];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpuId);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int freq = -1;
    (void)fscanf(f, "%d", &freq);
    fclose(f);
    return freq;
}

// Build a cpu_set_t containing the fastest cores by reading sysfs frequencies.
// Works on 3-cluster SoCs (1 prime + 3 big + 4 little) and classic big.LITTLE.
// Falls back to all cores if sysfs is unavailable.
inline cpu_set_t computeBigCoreMask() noexcept {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    int count = getCoreCount();
    if (count <= 4) {
        for (int i = 0; i < count; ++i) CPU_SET(i, &mask);
        return mask;
    }

    constexpr size_t kMaxCores = 16;
    int maxFreq = 0;
    std::array<int, kMaxCores> freqs{};
    int readable = 0;
    size_t limit = static_cast<size_t>(count) < kMaxCores ? static_cast<size_t>(count) : kMaxCores;
    for (size_t i = 0; i < limit; ++i) {
        freqs[i] = readCoreMaxFreq(static_cast<int>(i));
        if (freqs[i] > 0) {
            maxFreq = std::max(maxFreq, freqs[i]);
            ++readable;
        }
    }

    if (readable == 0 || maxFreq <= 0) {
        for (int i = 0; i < count; ++i) CPU_SET(i, &mask);
        return mask;
    }

    // Select cores within 80% of the fastest frequency
    int threshold = maxFreq * 80 / 100;
    for (size_t i = 0; i < limit; ++i) {
        if (freqs[i] >= threshold) {
            CPU_SET(static_cast<int>(i), &mask);
        }
    }
    return mask;
}

// Cached version: sysfs reads happen exactly once per process.
// Previously computed on every decode-thread start (cold path, up to 50ms of I/O).
inline cpu_set_t bigCoreMask() noexcept {
    static cpu_set_t cached;
    static std::once_flag flag;
    std::call_once(flag, [] { cached = computeBigCoreMask(); });
    return cached;
}

}  // namespace android

// Android thread priority configuration
// AAudio callbacks get system-managed priority, but DecodeThread
// is not an AAudio callback and needs explicit priority elevation.
inline ConfigureResult configureCurrentThreadForDecode() noexcept {
    ConfigureResult result;

    // Set thread priority via setpriority() - affects scheduling
    // AUDIO_CRITICAL priority for decode thread to ensure smooth playback
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice::kAudioCritical) == 0) {
        result.niceSet = true;
    } else {
        MEDIA_LOG_W("configureCurrentThreadForDecode: setpriority failed (errno=%d)", errno);
    }

    // Try SCHED_FIFO for real-time scheduling (requires CAP_SYS_NICE)
    struct sched_param param;
    param.sched_priority = 2;  // Low RT priority, just above normal
    if (sched_setscheduler(tid, SCHED_FIFO, &param) == 0) {
        result.prioritySet = true;
    } else {
        // SCHED_FIFO requires elevated permissions, fall back to nice only
        // This is expected on most Android devices without root
        result.prioritySet = false;
    }

    // Core affinity: prefer big/prime cores for decode performance
    cpu_set_t bigCores = android::bigCoreMask();
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &bigCores) == 0) {
        result.affinitySet = true;
    }

    return result;
}

inline ConfigureResult configureCurrentThreadForWorker() noexcept {
    ConfigureResult result;

    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));

    // Worker threads get elevated but not critical priority
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice::kHigh) == 0) {
        result.niceSet = true;
    }

    // Workers use nice, not SCHED_FIFO — leave prioritySet/affinitySet false
    // to accurately report that RT scheduling was not attempted.

    return result;
}

// Lower priority than the default for background work (transcript inference etc).
// Prevents whisper's OpenMP threads from starving audio/video on mid-tier devices.
inline ConfigureResult configureCurrentThreadForBackground() noexcept {
    ConfigureResult result;
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice::kBackground) == 0) {
        result.niceSet = true;
    }
    return result;
}

inline void initializeCoreConfig() noexcept {}

#elif MEDIA_PLATFORM_IOS

inline void initializeCoreConfig() noexcept {}

inline ConfigureResult configureCurrentThreadForDecode() noexcept {
    ConfigureResult result;

    // Set time constraint policy for decode/encode threads
    mach_port_t thread = pthread_mach_thread_np(pthread_self());

    // 20ms audio frame period in Mach absolute time (1:1 with ns on Apple Silicon)
    thread_time_constraint_policy_data_t policy;
    policy.period = 20000000;      // 20ms — audio frame cadence
    policy.computation = 500000;   // 0.5ms
    policy.constraint = 1000000;   // 1ms
    policy.preemptible = 1;

    kern_return_t kr = thread_policy_set(
        thread,
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );

    // Report honestly: only time-constraint policy was attempted. iOS has no
    // core affinity and we do not set nice/QoS for decode threads here.
    // Fields left false indicate "not applicable on this platform" — callers
    // should inspect platform before interpreting allSucceeded().
    result.prioritySet = (kr == KERN_SUCCESS);
    result.affinitySet = false;
    result.niceSet = false;

    return result;
}

inline ConfigureResult configureCurrentThreadForWorker() noexcept {
    // iOS doesn't need explicit worker thread configuration — default QoS is adequate.
    // Report false for fields we didn't actually configure.
    return {};
}

// Background priority for transcript/analytics work — iOS uses QOS_CLASS_UTILITY.
inline ConfigureResult configureCurrentThreadForBackground() noexcept {
    ConfigureResult result;
    int err = pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    if (err == 0) result.niceSet = true;
    return result;
}

#endif

}  // namespace media::thread_affinity
