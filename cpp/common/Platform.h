// Platform and architecture detection macros.
// Defines MEDIA_PLATFORM_ANDROID/IOS and MEDIA_ARCH_ARM64/X64.
#pragma once

// Platform detection
#if defined(__ANDROID__)
    #define MEDIA_PLATFORM_ANDROID 1
    #define MEDIA_PLATFORM_IOS 0
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IOS || TARGET_OS_IPHONE
        #define MEDIA_PLATFORM_ANDROID 0
        #define MEDIA_PLATFORM_IOS 1
    #else
        #error "Unsupported Apple platform"
    #endif
#else
    #error "Unsupported platform"
#endif

// Architecture detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define MEDIA_ARCH_ARM64 1
    #define MEDIA_ARCH_X64 0
#elif defined(__x86_64__) || defined(_M_X64)
    #define MEDIA_ARCH_ARM64 0
    #define MEDIA_ARCH_X64 1
#else
    #define MEDIA_ARCH_ARM64 0
    #define MEDIA_ARCH_X64 0
#endif
