// GFNI RS16 fold kernel (v1.22.0).
//
// This TU is compiled with /arch:AVX512 (MSVC) or function-level target
// attributes (GCC/Clang) so the GFNI 512-bit intrinsics are legal while the
// rest of the codebase stays baseline x86-64. The dispatcher in rs16.cpp
// only selects rs16_fold_gfni when the CPU reports gfni (leaf 7 ECX bit 8
// WITH OS-enabled ZMM XSTATE) AND the OPENRAR_HAS_GFNI_KERNEL compile-time
// capability flag proves this TU was compiled with the intrinsics.
//
// Bit-exactness contract: the fold computed here must equal the scalar
// table fold byte-for-byte. rs16.cpp probes the instruction's matrix
// convention once at init (see build_gfni_affines) and falls back to the
// scalar path if no convention matches — worst case is "no speedup",
// never wrong parity. The cross-path gate in
// tests/unit/recovery_tests.cpp (test_rs16_gfni_bit_exactness) validates
// the fold on GFNI-capable hardware or under Intel SDE in CI.

#include "rs16.hpp"

#if defined(OPENRAR_HAS_GFNI_KERNEL)

#include "../core/types.hpp"
#include <immintrin.h>

#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
// Clang does not reliably honor #pragma GCC target for intrinsic selection;
// explicit target attributes on each function using intrinsics are the
// portable form for GCC/Clang. MSVC relies on the whole-TU /arch:AVX512.
#define OPENRAR_GFNI_FN __attribute__((target("avx512f,avx512bw,avx512vl,gfni")))
#else
#define OPENRAR_GFNI_FN
#endif

namespace openrar::recovery {

namespace {

// _mm512_shuffle_epi8 operates per 128-bit lane (mask values 0..15, bit 7 =
// zero). Each lane of a 64-byte register holds 8 GF(2^16) words, so the
// de-interleave gathers the 8 low bytes / 8 high bytes into lane positions
// 0..7 and zeroes the rest. All four masks repeat per lane; _mm512_set_epi8
// lists bytes from byte 63 (lane 3, position 15) down to byte 0.

// result[p] = v[2p] for even p (low bytes gathered), zero elsewhere.
OPENRAR_GFNI_FN inline __m512i even_byte_mask() {
    return _mm512_set_epi8(0x80, 14, 0x80, 12, 0x80, 10, 0x80, 8, 0x80, 6, 0x80, 4, 0x80, 2, 0x80,
                           0, 0x80, 14, 0x80, 12, 0x80, 10, 0x80, 8, 0x80, 6, 0x80, 4, 0x80, 2,
                           0x80, 0, 0x80, 14, 0x80, 12, 0x80, 10, 0x80, 8, 0x80, 6, 0x80, 4, 0x80,
                           2, 0x80, 0, 0x80, 14, 0x80, 12, 0x80, 10, 0x80, 8, 0x80, 6, 0x80, 4,
                           0x80, 2, 0x80, 0);
}

// result[p] = v[2p+1] for even p (high bytes gathered), zero elsewhere.
OPENRAR_GFNI_FN inline __m512i odd_byte_mask() {
    return _mm512_set_epi8(15, 0x80, 13, 0x80, 11, 0x80, 9, 0x80, 7, 0x80, 5, 0x80, 3, 0x80, 1,
                           0x80, 15, 0x80, 13, 0x80, 11, 0x80, 9, 0x80, 7, 0x80, 5, 0x80, 3, 0x80,
                           1, 0x80, 15, 0x80, 13, 0x80, 11, 0x80, 9, 0x80, 7, 0x80, 5, 0x80, 3,
                           0x80, 1, 0x80, 15, 0x80, 13, 0x80, 11, 0x80, 9, 0x80, 7, 0x80, 5, 0x80,
                           3, 0x80, 1, 0x80);
}

// Spread out_lo (lane positions 0..7) to even positions, zeroing odds.
OPENRAR_GFNI_FN inline __m512i spread_lo_mask() {
    return _mm512_set_epi8(0x80, 7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0,
                           0x80, 7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0,
                           0x80, 7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0,
                           0x80, 7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0);
}

// Spread out_hi (lane positions 0..7) to odd positions, zeroing evens.
OPENRAR_GFNI_FN inline __m512i spread_hi_mask() {
    return _mm512_set_epi8(7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0, 0x80,
                           7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0, 0x80,
                           7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0, 0x80,
                           7, 0x80, 6, 0x80, 5, 0x80, 4, 0x80, 3, 0x80, 2, 0x80, 1, 0x80, 0, 0x80);
}

} // namespace

// Fold `bytes` bytes of GF(2^16) data words into ecc:
// for each 16-bit word (hi, lo):
//   out_lo = aff(lo, m[0]) ^ aff(hi, m[2])
//   out_hi = aff(lo, m[1]) ^ aff(hi, m[3])
// where each aff() is a per-byte 8x8 GF(2) matrix multiply applied by
// _mm512_gf2p8affine_epi64_epi8 with the 8-byte matrix constant broadcast
// across the register. m[0..1] derive from the coefficient itself (applied
// to the word's low byte), m[2..3] from coeff*x^8 (the word's high byte).
OPENRAR_GFNI_FN void rs16_fold_gfni(const core::byte* data, core::byte* ecc, size_t bytes,
                                    const core::uint64 m[4]) noexcept {
    const __m512i k_even = even_byte_mask();
    const __m512i k_odd = odd_byte_mask();
    const __m512i k_spread_lo = spread_lo_mask();
    const __m512i k_spread_hi = spread_hi_mask();

    const __m512i v_m0 = _mm512_set1_epi64(static_cast<long long>(m[0]));
    const __m512i v_m1 = _mm512_set1_epi64(static_cast<long long>(m[1]));
    const __m512i v_m2 = _mm512_set1_epi64(static_cast<long long>(m[2]));
    const __m512i v_m3 = _mm512_set1_epi64(static_cast<long long>(m[3]));

    size_t i = 0;
    while (i + 64 <= bytes) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(data + i));
        __m512i lo = _mm512_shuffle_epi8(v, k_even);
        __m512i hi = _mm512_shuffle_epi8(v, k_odd);

        __m512i out_lo = _mm512_xor_si512(_mm512_gf2p8affine_epi64_epi8(lo, v_m0, 0),
                                          _mm512_gf2p8affine_epi64_epi8(hi, v_m2, 0));
        __m512i out_hi = _mm512_xor_si512(_mm512_gf2p8affine_epi64_epi8(lo, v_m1, 0),
                                          _mm512_gf2p8affine_epi64_epi8(hi, v_m3, 0));

        __m512i spread = _mm512_or_si512(_mm512_shuffle_epi8(out_lo, k_spread_lo),
                                         _mm512_shuffle_epi8(out_hi, k_spread_hi));
        __m512i e = _mm512_loadu_si512(reinterpret_cast<const void*>(ecc + i));
        _mm512_storeu_si512(reinterpret_cast<void*>(ecc + i), _mm512_xor_si512(e, spread));
        i += 64;
    }
}

} // namespace openrar::recovery

#endif // OPENRAR_HAS_GFNI_KERNEL
