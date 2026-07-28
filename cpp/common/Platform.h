// Platform detection: MEDIA_PLATFORM_ANDROID / MEDIA_PLATFORM_IOS / MEDIA_PLATFORM_HOST.
//
// MEDIA_PLATFORM_HOST covers desktop builds of the shared C++ — the sanitizer
// test suite runs there. Without it, a macOS test build has to claim to be iOS
// (-DTARGET_OS_IOS=1) just to get past platform detection, which then pulls in
// iOS-only headers on a platform that merely happens to tolerate them.
#pragma once

#if defined(__ANDROID__)
#define MEDIA_PLATFORM_ANDROID 1
#define MEDIA_PLATFORM_IOS 0
#define MEDIA_PLATFORM_HOST 0
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_IPHONE
#define MEDIA_PLATFORM_ANDROID 0
#define MEDIA_PLATFORM_IOS 1
#define MEDIA_PLATFORM_HOST 0
#else
#define MEDIA_PLATFORM_ANDROID 0
#define MEDIA_PLATFORM_IOS 0
#define MEDIA_PLATFORM_HOST 1
#endif
#else
#define MEDIA_PLATFORM_ANDROID 0
#define MEDIA_PLATFORM_IOS 0
#define MEDIA_PLATFORM_HOST 1
#endif
