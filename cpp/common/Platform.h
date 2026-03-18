#pragma once

// Platform detection
#if defined(__ANDROID__)
    #define MEDIA_PLATFORM_ANDROID 1
    #define MEDIA_PLATFORM_IOS 0
    #define MEDIA_PLATFORM_NAME "Android"
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IOS || TARGET_OS_IPHONE
        #define MEDIA_PLATFORM_ANDROID 0
        #define MEDIA_PLATFORM_IOS 1
        #define MEDIA_PLATFORM_NAME "iOS"
    #else
        #error "Unsupported Apple platform"
    #endif
#else
    #error "Unsupported platform"
#endif

// Architecture detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define MEDIA_ARCH_ARM64 1
    #define MEDIA_ARCH_ARM32 0
    #define MEDIA_ARCH_X64 0
#elif defined(__arm__) || defined(_M_ARM)
    #define MEDIA_ARCH_ARM64 0
    #define MEDIA_ARCH_ARM32 1
    #define MEDIA_ARCH_X64 0
#elif defined(__x86_64__) || defined(_M_X64)
    #define MEDIA_ARCH_ARM64 0
    #define MEDIA_ARCH_ARM32 0
    #define MEDIA_ARCH_X64 1
#else
    #define MEDIA_ARCH_ARM64 0
    #define MEDIA_ARCH_ARM32 0
    #define MEDIA_ARCH_X64 0
#endif

#define MEDIA_ARCH_ARM (MEDIA_ARCH_ARM64 || MEDIA_ARCH_ARM32)
