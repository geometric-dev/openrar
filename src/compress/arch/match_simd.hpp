#ifndef OPENRAR_COMPRESS_ARCH_MATCH_SIMD_HPP
#define OPENRAR_COMPRESS_ARCH_MATCH_SIMD_HPP

#include "../../core/types.hpp"
#include "../../core/cpu.hpp"

#if defined(__EMSCRIPTEN__) && defined(OPENRAR_WASM_SIMD)
// Optional WASM SIMD128 kernel (-DOPENRAR_WASM_SIMD=ON adds -msimd128).
// The wasm build is scalar by default so the baseline module keeps older-
// browser compatibility and its size budget.
#include <wasm_simd128.h>
#define OPENRAR_HAS_WASM_SIMD 1
#elif !defined(__EMSCRIPTEN__)
// L14: MSVC x86-64 only. _M_IX86 was dropped: 32-bit x86 MSVC has no
// 64-bit scan intrinsics, and the paths below (and callers like
// Compressor50::distance_to_slot) rely on them.
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
#include <intrin.h>
#include <emmintrin.h>
#include <immintrin.h>
#define OPENRAR_HAS_X86_SIMD 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <emmintrin.h>
#include <immintrin.h>
#define OPENRAR_HAS_X86_SIMD 1
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <arm_neon.h>
#define OPENRAR_HAS_ARM_NEON 1
#endif
#endif

namespace openrar::compress::arch {

inline size_t match_length_scalar(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    while (i + 8 <= cap) {
        core::uint64 p_val, q_val;
        std::memcpy(&p_val, p + i, 8);
        std::memcpy(&q_val, q + i, 8);
        if (p_val != q_val) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64) || defined(_M_ARM64))
            unsigned long index;
            _BitScanForward64(&index, p_val ^ q_val);
            return i + index / 8;
#elif defined(__GNUC__) || defined(__clang__)
            return i + __builtin_ctzll(p_val ^ q_val) / 8;
#else
            // L14: 32-bit x86 MSVC has no _BitScanForward64; stop the word
            // loop and let the bytewise tail below find the exact byte.
            break;
#endif
        }
        i += 8;
    }
    while (i < cap && p[i] == q[i]) {
        i++;
    }
    return i;
}

#if defined(OPENRAR_HAS_X86_SIMD)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
inline size_t
match_length_avx2(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    while (i + 32 <= cap) {
        __m256i v_p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
        __m256i v_q = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q + i));
        __m256i cmp = _mm256_cmpeq_epi8(v_p, v_q);
        int mask = _mm256_movemask_epi8(cmp);
        if (mask != -1) {
            core::uint32 diff = static_cast<core::uint32>(~mask);
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward(&index, diff);
            return i + index;
#else
            return i + __builtin_ctz(diff);
#endif
        }
        i += 32;
    }
    while (i + 16 <= cap) {
        __m128i v_p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        __m128i v_q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + i));
        __m128i cmp = _mm_cmpeq_epi8(v_p, v_q);
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0xFFFF) {
            int diff = ~mask & 0xFFFF;
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward(&index, diff);
            return i + index;
#else
            return i + __builtin_ctz(diff);
#endif
        }
        i += 16;
    }
    if (i < cap) {
        if (cap >= 16) {
            size_t tail = cap - 16;
            __m128i v_p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + tail));
            __m128i v_q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + tail));
            __m128i cmp = _mm_cmpeq_epi8(v_p, v_q);
            int mask = _mm_movemask_epi8(cmp);
            if (mask != 0xFFFF) {
                int diff = ~mask & 0xFFFF;
#if defined(_MSC_VER)
                unsigned long index;
                _BitScanForward(&index, diff);
                return tail + index;
#else
                return tail + __builtin_ctz(diff);
#endif
            }
            return cap;
        }
        if (cap - i >= 8) {
            core::uint64 p_val, q_val;
            std::memcpy(&p_val, p + i, 8);
            std::memcpy(&q_val, q + i, 8);
            if (p_val != q_val) {
#if defined(_MSC_VER)
                unsigned long index;
                _BitScanForward64(&index, p_val ^ q_val);
                return i + index / 8;
#elif defined(__GNUC__) || defined(__clang__)
                return i + __builtin_ctzll(p_val ^ q_val) / 8;
#endif
            }
            i += 8;
        }
        while (i < cap && p[i] == q[i]) {
            i++;
        }
    }
    return i;
}

inline size_t match_length_sse2(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    while (i + 16 <= cap) {
        __m128i v_p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        __m128i v_q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + i));
        __m128i cmp = _mm_cmpeq_epi8(v_p, v_q);
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0xFFFF) {
            int diff = ~mask & 0xFFFF;
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward(&index, diff);
            return i + index;
#else
            return i + __builtin_ctz(diff);
#endif
        }
        i += 16;
    }
    if (i < cap) {
        if (cap >= 16) {
            // INTENDED (overlap-safe edge read): the re-load at cap-16
            // re-reads up to 15 bytes already verified equal by the loop
            // above ([0, i) was fully compared), so the overlap cannot
            // produce a false mismatch. One full-width load is the standard
            // way to cover the <16-byte tail; do not replace it with a
            // bounded partial load.
            size_t tail = cap - 16;
            __m128i v_p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + tail));
            __m128i v_q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + tail));
            __m128i cmp = _mm_cmpeq_epi8(v_p, v_q);
            int mask = _mm_movemask_epi8(cmp);
            if (mask != 0xFFFF) {
                int diff = ~mask & 0xFFFF;
#if defined(_MSC_VER)
                unsigned long index;
                _BitScanForward(&index, diff);
                return tail + index;
#else
                return tail + __builtin_ctz(diff);
#endif
            }
            return cap;
        }
        while (i < cap && p[i] == q[i]) {
            i++;
        }
    }
    return i;
}
#endif

#if defined(OPENRAR_HAS_ARM_NEON)
inline size_t match_length_neon(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    while (i + 16 <= cap) {
        uint8x16_t v_p = vld1q_u8(p + i);
        uint8x16_t v_q = vld1q_u8(q + i);
        uint8x16_t cmp = vceqq_u8(v_p, v_q);
        uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
        uint64_t lo = vgetq_lane_u64(cmp64, 0);
        if (lo != ~0ULL) {
            uint64_t diff = ~lo;
#if defined(_MSC_VER) && defined(_M_ARM64)
            unsigned long index;
            _BitScanForward64(&index, diff);
            return i + (index >> 3);
#elif defined(__GNUC__) || defined(__clang__)
            return i + (static_cast<size_t>(__builtin_ctzll(diff)) >> 3);
#else
            for (size_t k = 0; k < 8; ++k) {
                if (p[i + k] != q[i + k]) return i + k;
            }
#endif
        }
        uint64_t hi = vgetq_lane_u64(cmp64, 1);
        if (hi != ~0ULL) {
            uint64_t diff = ~hi;
#if defined(_MSC_VER) && defined(_M_ARM64)
            unsigned long index;
            _BitScanForward64(&index, diff);
            return i + 8 + (index >> 3);
#elif defined(__GNUC__) || defined(__clang__)
            return i + 8 + (static_cast<size_t>(__builtin_ctzll(diff)) >> 3);
#else
            for (size_t k = 8; k < 16; ++k) {
                if (p[i + k] != q[i + k]) return i + k;
            }
#endif
        }
        i += 16;
    }
    if (i + 8 <= cap) {
        core::uint64 p_val, q_val;
        std::memcpy(&p_val, p + i, 8);
        std::memcpy(&q_val, q + i, 8);
        if (p_val != q_val) {
#if defined(_MSC_VER) && defined(_M_ARM64)
            unsigned long index;
            _BitScanForward64(&index, p_val ^ q_val);
            return i + (index >> 3);
#elif defined(__GNUC__) || defined(__clang__)
            return i + (static_cast<size_t>(__builtin_ctzll(p_val ^ q_val)) >> 3);
#endif
        } else {
            i += 8;
        }
    }
    while (i < cap && p[i] == q[i]) {
        i++;
    }
    return i;
}
#endif

#if defined(OPENRAR_HAS_WASM_SIMD)
// SIMD128 mirror of match_length_sse2: i8x16 equality -> bitmask (bit k =
// byte k equal), ctz of the inverted mask names the first mismatching byte.
// The 8-byte tail matches the SSE2 tail's byte-coverage rules.
inline size_t match_length_wasm(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    while (i + 16 <= cap) {
        v128_t v_p = wasm_v128_load(p + i);
        v128_t v_q = wasm_v128_load(q + i);
        int mask = wasm_i8x16_bitmask(wasm_i8x16_eq(v_p, v_q));
        if (mask != 0xFFFF) {
            int diff = ~mask & 0xFFFF;
            return i + static_cast<size_t>(__builtin_ctz(diff));
        }
        i += 16;
    }
    while (i + 8 <= cap) {
        core::uint64 p_val, q_val;
        std::memcpy(&p_val, p + i, 8);
        std::memcpy(&q_val, q + i, 8);
        if (p_val != q_val) {
            return i + __builtin_ctzll(p_val ^ q_val) / 8;
        }
        i += 8;
    }
    while (i < cap && p[i] == q[i]) {
        i++;
    }
    return i;
}
#endif

using MatchFn = size_t (*)(const core::byte*, const core::byte*, size_t) noexcept;

#if defined(OPENRAR_HAS_X86_SIMD) && defined(OPENRAR_HAS_AVX512_KERNEL)
// Defined in match_avx512.cpp — the one TU compiled with /arch:AVX512
// (MSVC) or function-level target attributes (GCC/Clang). Only reachable
// when cpu.avx512f proves CPU + OS ZMM state support.
size_t match_length_avx512_kernel(const core::byte* p, const core::byte* q, size_t cap) noexcept;
#endif

inline MatchFn resolve_match_fn() noexcept {
#if defined(OPENRAR_HAS_X86_SIMD)
    const auto& cpu = core::get_cpu_features();
#if defined(OPENRAR_HAS_AVX512_KERNEL)
    if (cpu.avx512f) return match_length_avx512_kernel;
#endif
    if (cpu.avx2) return match_length_avx2;
    if (cpu.sse2) return match_length_sse2;
    return match_length_scalar;
#elif defined(OPENRAR_HAS_ARM_NEON)
    if (core::get_cpu_features().neon) return match_length_neon;
    return match_length_scalar;
#elif defined(OPENRAR_HAS_WASM_SIMD)
    return match_length_wasm;
#else
    return match_length_scalar;
#endif
}

inline const MatchFn g_match_fn = resolve_match_fn();

inline size_t match_length_simd(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
#if defined(OPENRAR_HAS_X86_SIMD) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
    if (cap >= 16) {
        __m128i v_p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        __m128i v_q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(v_p, v_q));
        if (mask != 0xFFFF) {
            int diff = ~mask & 0xFFFF;
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward(&index, diff);
            return index;
#else
            return static_cast<size_t>(__builtin_ctz(diff));
#endif
        }
        if (cap == 16) return 16;
        size_t r = 16 + g_match_fn(p + 16, q + 16, cap - 16);
        return r > cap ? cap : r;
    }
    return match_length_scalar(p, q, cap);
#elif defined(OPENRAR_HAS_ARM_NEON) && (defined(__aarch64__) || defined(_M_ARM64))
    if (cap >= 16) {
        uint8x16_t v_p = vld1q_u8(p);
        uint8x16_t v_q = vld1q_u8(q);
        uint64x2_t cmp64 = vreinterpretq_u64_u8(vceqq_u8(v_p, v_q));
        uint64_t lo = vgetq_lane_u64(cmp64, 0);
        if (lo != ~0ULL) {
            uint64_t diff = ~lo;
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward64(&index, diff);
            return index >> 3;
#else
            return static_cast<size_t>(__builtin_ctzll(diff)) >> 3;
#endif
        }
        uint64_t hi = vgetq_lane_u64(cmp64, 1);
        if (hi != ~0ULL) {
            uint64_t diff = ~hi;
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward64(&index, diff);
            return 8 + (index >> 3);
#else
            return 8 + (static_cast<size_t>(__builtin_ctzll(diff)) >> 3);
#endif
        }
        if (cap == 16) return 16;
        size_t r = 16 + g_match_fn(p + 16, q + 16, cap - 16);
        return r > cap ? cap : r;
    }
    return match_length_scalar(p, q, cap);
#else
    size_t r = g_match_fn(p, q, cap);
    return r > cap ? cap : r;
#endif
}

} // namespace openrar::compress::arch

#endif // OPENRAR_COMPRESS_ARCH_MATCH_SIMD_HPP
