#pragma once

#include "Platform.h"

// SIMD detection and includes
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
    #include <arm_neon.h>
    #define MEDIA_HAS_NEON 1
    #define MEDIA_HAS_SSE 0
#elif defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    #include <emmintrin.h>  // SSE2
    #define MEDIA_HAS_NEON 0
    #define MEDIA_HAS_SSE 1
#else
    #define MEDIA_HAS_NEON 0
    #define MEDIA_HAS_SSE 0
#endif

// Unified SIMD availability check
#define MEDIA_HAS_SIMD (MEDIA_HAS_NEON || MEDIA_HAS_SSE)

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

// Inlining control
#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#else
    #define FORCE_INLINE inline
#endif

// Function optimization hints
#if defined(__GNUC__) || defined(__clang__)
    #define HOT_FUNCTION __attribute__((hot))
    #define COLD_FUNCTION __attribute__((cold))
#else
    #define HOT_FUNCTION
    #define COLD_FUNCTION
#endif

// Aliasing hints
#if defined(__GNUC__) || defined(__clang__)
    #define RESTRICT __restrict__
#elif defined(_MSC_VER)
    #define RESTRICT __restrict
#else
    #define RESTRICT
#endif

// Cache prefetch hints (useful for audio buffer processing)
#if defined(__GNUC__) || defined(__clang__)
    #define PREFETCH_READ(addr) __builtin_prefetch(addr, 0, 3)
#else
    #define PREFETCH_READ(addr) ((void)0)
#endif
