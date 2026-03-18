#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "CompilerHints.h"

namespace media {
namespace simd {

// Modern compilers (Clang 14+) optimize memcpy extremely well for ARM64.
// Hand-written NEON rarely outperforms and adds maintenance burden.

FORCE_INLINE HOT_FUNCTION
void copyInt16(int16_t* RESTRICT dst, const int16_t* RESTRICT src, size_t count) noexcept {
    std::memcpy(dst, src, count * sizeof(int16_t));
}

FORCE_INLINE HOT_FUNCTION
void copyBytes(uint8_t* RESTRICT dst, const uint8_t* RESTRICT src, size_t count) noexcept {
    std::memcpy(dst, src, count);
}

FORCE_INLINE HOT_FUNCTION
void zeroInt16(int16_t* dst, size_t count) noexcept {
    std::memset(dst, 0, count * sizeof(int16_t));
}

}  // namespace simd
}  // namespace media
