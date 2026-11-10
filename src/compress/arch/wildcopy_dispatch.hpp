// src/compress/arch/wildcopy_dispatch.hpp
// Overlapping LZ wildcopy and pattern-doubling SIMD kernels (Hypothesis H-18, Milestone v2.3)
// Governed by Invariant I-1 (Digest Identity), I-15 (64-Byte Buffer Tail Margin),
// and Rule G-1 (Single Decision Function).

#ifndef OPENRAR_COMPRESS_ARCH_WILDCOPY_DISPATCH_HPP
#define OPENRAR_COMPRESS_ARCH_WILDCOPY_DISPATCH_HPP

#include "../../core/cpu.hpp"
#include "../../core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
#include <emmintrin.h>
#include <immintrin.h>
#include <tmmintrin.h> // SSSE3 _mm_shuffle_epi8
#define OPENRAR_HAS_X86_WILDCOPY 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <emmintrin.h>
#include <immintrin.h>
#include <tmmintrin.h>
#define OPENRAR_HAS_X86_WILDCOPY 1
#endif

// GCC/Clang refuse to inline SSSE3/AVX2 intrinsics into functions compiled for the baseline
// x86-64 target, and the build does not pass -mssse3/-mavx2 globally (runtime dispatch must
// stay safe on older CPUs). Each kernel therefore carries a function-level target attribute,
// the same convention as match_simd.hpp. MSVC needs nothing.
#if defined(OPENRAR_HAS_X86_WILDCOPY) && (defined(__GNUC__) || defined(__clang__))
#define OPENRAR_WILDCOPY_TARGET_SSSE3 __attribute__((target("ssse3")))
#define OPENRAR_WILDCOPY_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define OPENRAR_WILDCOPY_TARGET_SSSE3
#define OPENRAR_WILDCOPY_TARGET_AVX2
#endif

#if !defined(OPENRAR_HAS_X86_WILDCOPY) &&                                                          \
    (defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)) &&                          \
    !defined(OPENRAR_BIG_ENDIAN)
#include <arm_neon.h>
#define OPENRAR_HAS_NEON_WILDCOPY 1
#endif

namespace openrar::compress::arch {

using WildcopyFn = void (*)(core::byte* dst, const core::byte* src, size_t distance, size_t length,
                            bool can_overshoot) noexcept;

// ============================================================================
// 1. Scalar Reference Kernel (Portable Fallback)
// ============================================================================

inline void wildcopy_scalar(core::byte* dst, const core::byte* src, size_t distance, size_t length,
                            [[maybe_unused]] bool can_overshoot) noexcept {
    if (distance == 1) {
        std::memset(dst, *src, length);
        return;
    }
    if (distance >= length) {
        std::memcpy(dst, src, length);
        return;
    }

    // Overlapping match (distance < length):
    // Pattern doubling: copy initial pattern of size distance, then double
    // using std::memcpy on already written destination bytes.
    size_t done = distance;
    std::memcpy(dst, src, distance);
    while (done < length) {
        const size_t step = std::min(done, length - done);
        std::memcpy(dst + done, dst, step);
        done += step;
    }
}

// ============================================================================
// 2. SSSE3 / AVX2 Accelerated Wildcopy Kernel
// ============================================================================

#if defined(OPENRAR_HAS_X86_WILDCOPY)

alignas(16) inline constexpr uint8_t SHUFFLE_MASKS[16][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},     // 0 (unused)
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},     // 1
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},     // 2
    {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},     // 3
    {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3},     // 4
    {0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0},     // 5
    {0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3},     // 6
    {0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1},     // 7
    {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7},     // 8
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3, 4, 5, 6},     // 9
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5},     // 10
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 1, 2, 3, 4},    // 11
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3},   // 12
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 1, 2},  // 13
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1}, // 14
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0} // 15
};

OPENRAR_WILDCOPY_TARGET_SSSE3
inline void wildcopy_ssse3(core::byte* dst, const core::byte* src, size_t distance, size_t length,
                           [[maybe_unused]] bool can_overshoot) noexcept {
    if (distance == 1) {
        std::memset(dst, *src, length);
        return;
    }

    if (distance >= 16) {
        if (length < 16) {
            std::memcpy(dst, src, length);
            return;
        }
        core::byte* d = dst;
        const core::byte* s = src;
        size_t rem = length;
        while (rem >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d),
                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
            d += 16;
            s += 16;
            rem -= 16;
        }
        if (rem > 0) {
            std::memcpy(d, s, rem);
        }
        return;
    }

    // Overlapping small distances: distance in [2..15]
    if (distance == 2) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 2, length, false);
            return;
        }
        uint16_t v16;
        std::memcpy(&v16, src, 2);
        const __m128i val = _mm_set1_epi16(static_cast<short>(v16));
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            _mm_store_si128(reinterpret_cast<__m128i*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    if (distance == 4) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 4, length, false);
            return;
        }
        uint32_t v32;
        std::memcpy(&v32, src, 4);
        const __m128i val = _mm_set1_epi32(static_cast<int>(v32));
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            _mm_store_si128(reinterpret_cast<__m128i*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    if (distance == 8) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 8, length, false);
            return;
        }
        int64_t v64;
        std::memcpy(&v64, src, 8);
        const __m128i val = _mm_set1_epi64x(v64);
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            _mm_store_si128(reinterpret_cast<__m128i*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    // Distance in [3, 5, 6, 7, 9..15]
    // Replicate pattern to 16 bytes via pshufb
    const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
    const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(SHUFFLE_MASKS[distance]));
    const __m128i pat = _mm_shuffle_epi8(raw, mask);

    if (length <= 16) {
        if (length == 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), pat);
        } else {
            alignas(16) core::byte tmp[16];
            _mm_store_si128(reinterpret_cast<__m128i*>(tmp), pat);
            std::memcpy(dst, tmp, length);
        }
        return;
    }

    // length > 16: fill first 16 bytes with pshufb pattern, then extend sequentially
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), pat);
    for (size_t i = 16; i < length; ++i) {
        dst[i] = dst[i - distance];
    }
}

OPENRAR_WILDCOPY_TARGET_AVX2
inline void wildcopy_avx2(core::byte* dst, const core::byte* src, size_t distance, size_t length,
                          bool can_overshoot) noexcept {
    // If distance >= 32 and length >= 32, copy in 32-byte vector increments
    if (distance >= 32 && length >= 32) {
        core::byte* d = dst;
        const core::byte* s = src;
        size_t rem = length;
        while (rem >= 32) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)));
            d += 32;
            s += 32;
            rem -= 32;
        }
        if (rem > 0) {
            wildcopy_ssse3(d, s, distance, rem, false);
        }
        return;
    }

    // Fall back to SSSE3 128-bit pattern doubling
    wildcopy_ssse3(dst, src, distance, length, can_overshoot);
}

#endif // OPENRAR_HAS_X86_WILDCOPY

// ============================================================================
// 3. ARM NEON Accelerated Wildcopy Kernel
// ============================================================================

#if defined(OPENRAR_HAS_NEON_WILDCOPY)

inline void wildcopy_neon(core::byte* dst, const core::byte* src, size_t distance, size_t length,
                          [[maybe_unused]] bool can_overshoot) noexcept {
    if (distance == 1) {
        std::memset(dst, *src, length);
        return;
    }

    if (distance >= 16) {
        if (length < 16) {
            std::memcpy(dst, src, length);
            return;
        }
        core::byte* d = dst;
        const core::byte* s = src;
        size_t rem = length;
        while (rem >= 16) {
            vst1q_u8(reinterpret_cast<uint8_t*>(d), vld1q_u8(reinterpret_cast<const uint8_t*>(s)));
            d += 16;
            s += 16;
            rem -= 16;
        }
        if (rem > 0) {
            std::memcpy(d, s, rem);
        }
        return;
    }

    if (distance == 2) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 2, length, false);
            return;
        }
        uint16_t v16;
        std::memcpy(&v16, src, 2);
        const uint16x8_t val = vdupq_n_u16(v16);
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            vst1q_u16(reinterpret_cast<uint16_t*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            vst1q_u16(reinterpret_cast<uint16_t*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    if (distance == 4) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 4, length, false);
            return;
        }
        uint32_t v32;
        std::memcpy(&v32, src, 4);
        const uint32x4_t val = vdupq_n_u32(v32);
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            vst1q_u32(reinterpret_cast<uint32_t*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            vst1q_u32(reinterpret_cast<uint32_t*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    if (distance == 8) {
        if (length < 16) {
            wildcopy_scalar(dst, src, 8, length, false);
            return;
        }
        uint64_t v64;
        std::memcpy(&v64, src, 8);
        const uint64x2_t val = vdupq_n_u64(v64);
        core::byte* d = dst;
        size_t rem = length;
        while (rem >= 16) {
            vst1q_u64(reinterpret_cast<uint64_t*>(d), val);
            d += 16;
            rem -= 16;
        }
        if (rem > 0) {
            alignas(16) core::byte tmp[16];
            vst1q_u64(reinterpret_cast<uint64_t*>(tmp), val);
            std::memcpy(d, tmp, rem);
        }
        return;
    }

    // Small arbitrary distances on NEON: fall back to scalar pattern doubling
    wildcopy_scalar(dst, src, distance, length, false);
}

#endif // OPENRAR_HAS_NEON_WILDCOPY

// ============================================================================
// 4. Runtime Kernel Selector (Rule G-1: Single Decision Function)
// ============================================================================

inline WildcopyFn select_wildcopy_kernel() noexcept {
    const auto& feats = core::get_cpu_features();

#if defined(OPENRAR_HAS_X86_WILDCOPY)
    if (feats.avx2) {
        return wildcopy_avx2;
    }
    if (feats.ssse3) {
        return wildcopy_ssse3;
    }
#elif defined(OPENRAR_HAS_NEON_WILDCOPY)
    if (feats.neon) {
        return wildcopy_neon;
    }
#else
    (void)feats;
#endif

    return wildcopy_scalar;
}

} // namespace openrar::compress::arch

#endif // OPENRAR_COMPRESS_ARCH_WILDCOPY_DISPATCH_HPP
