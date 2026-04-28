// Platform detection macros: MEDIA_PLATFORM_ANDROID / MEDIA_PLATFORM_IOS.
#pragma once

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
