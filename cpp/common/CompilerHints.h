#pragma once

#include "Platform.h"

// Only clang is supported (Android NDK + Xcode). MSVC/GCC fallback paths were
// dead code; the build system cannot target them.
#if !defined(__clang__)
#error "webm-player requires clang (Android NDK or Xcode toolchain)"
#endif

// SIMD detection and includes. arm64 (iOS device, Android arm64-v8a) always
// has NEON. x86_64 (iOS simulator) always has SSE2. 32-bit ARM is gated.
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
    #include <arm_neon.h>
    #define MEDIA_HAS_NEON 1
    #define MEDIA_HAS_SSE 0
#elif defined(__SSE2__) || defined(__x86_64__)
    #include <emmintrin.h>  // SSE2
    #define MEDIA_HAS_NEON 0
    #define MEDIA_HAS_SSE 1
#else
#error "Unsupported target ISA — neither NEON nor SSE2 available"
#endif

#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define HOT_FUNCTION __attribute__((hot))
#define RESTRICT     __restrict__

