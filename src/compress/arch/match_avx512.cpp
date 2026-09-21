// AVX-512 match-length kernel (v1.22.0).
//
// This TU is compiled with /arch:AVX512 (MSVC) or function-level target
// attributes (GCC/Clang) so the 512-bit intrinsics are legal, while the rest
// of the codebase stays baseline x86-64. The runtime dispatcher in
// match_simd.hpp only selects match_length_avx512_kernel when
// core::get_cpu_features().avx512f is set — CPU support AND OS-enabled ZMM
// XSTATE (see cpu.cpp, report M4) — and only when
// OPENRAR_HAS_AVX512_KERNEL proves this TU was compiled with the intrinsics.
//
// Semantics are bit-identical to match_length_scalar: the length of the
// common prefix of p and q, capped at cap. Validated by the cross-path
// bit-exactness test (tests/unit/compress_tests.cpp:
// test_match_length_bit_exactness), which compares every implementation the
// running CPU supports against the scalar reference.

#include "match_simd.hpp"

#if defined(OPENRAR_HAS_AVX512_KERNEL)

#include <immintrin.h>
#include <cstring>

// Clang does not honor #pragma GCC target for intrinsics selection in all
// versions; explicit target attributes on each function using intrinsics
// are the portable form for GCC/Clang. MSVC relies on the whole-TU
// /arch:AVX512 (CMake).
#if defined(__GNUC__) || defined(__clang__)
#define OPENRAR_AVX512_FN_ATTR "avx512f,avx512bw,avx512vl,avx2"
#define OPENRAR_AVX512_FN __attribute__((target(OPENRAR_AVX512_FN_ATTR)))
#else
#define OPENRAR_AVX512_FN
#endif

namespace openrar::compress::arch {

OPENRAR_AVX512_FN
size_t match_length_avx512_kernel(const core::byte* p, const core::byte* q, size_t cap) noexcept {
    if (cap == 0 || p[0] != q[0]) return 0;
    size_t i = 0;
    // 64 bytes per iteration: cmpeq produces a 64-bit lane mask; the first
    // mismatch position is the trailing-zero count of the inverted mask.
    while (i + 64 <= cap) {
        __m512i v_p = _mm512_loadu_si512(reinterpret_cast<const void*>(p + i));
        __m512i v_q = _mm512_loadu_si512(reinterpret_cast<const void*>(q + i));
        __mmask64 cmp = _mm512_cmpeq_epi8_mask(v_p, v_q);
        if (cmp != ~static_cast<__mmask64>(0)) {
            core::uint64 diff = ~static_cast<core::uint64>(cmp);
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward64(&index, diff);
            return i + index;
#else
            return i + static_cast<size_t>(__builtin_ctzll(diff));
#endif
        }
        i += 64;
    }
    // 256-bit / 128-bit / word tails: identical structure to the AVX2
    // kernel's tails (kept in this TU so every vectorized byte lives under
    // the same /arch guard).
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
            return i + static_cast<size_t>(__builtin_ctz(diff));
#endif
        }
        i += 32;
    }
    while (i + 8 <= cap) {
        core::uint64 p_val, q_val;
        std::memcpy(&p_val, p + i, 8);
        std::memcpy(&q_val, q + i, 8);
        if (p_val != q_val) {
#if defined(_MSC_VER)
            unsigned long index;
            _BitScanForward64(&index, p_val ^ q_val);
            return i + index / 8;
#else
            return i + static_cast<size_t>(__builtin_ctzll(p_val ^ q_val)) / 8;
#endif
        }
        i += 8;
    }
    while (i < cap && p[i] == q[i]) {
        i++;
    }
    return i;
}

} // namespace openrar::compress::arch

#endif // OPENRAR_HAS_AVX512_KERNEL
