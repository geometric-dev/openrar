// src/compress/arch/hash_simd.hpp
// AVX2 8-wide multiplicative hash kernel for Compressor50::hash_range (Hypothesis H-19,
// Milestone v2.3). Governed by Invariant I-1 (the SIMD hashes must equal the scalar
// calc_hash() values exactly) and Rule C-07 (little-endian hosts only; the caller keeps the
// !OPENRAR_BIG_ENDIAN guard).
//
// The kernel is a standalone function so that (a) GCC/Clang can compile it with a
// function-level target attribute without enabling AVX2 for the whole translation unit
// (the build passes no -mavx2; runtime dispatch must stay safe on older CPUs), and (b) the
// unit test exercises the same code the compressor runs.

#ifndef OPENRAR_COMPRESS_ARCH_HASH_SIMD_HPP
#define OPENRAR_COMPRESS_ARCH_HASH_SIMD_HPP

#include "../../core/types.hpp"

#include <cstdint>

#if (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))) ||                               \
    ((defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__)))
#include <immintrin.h>
#define OPENRAR_HAS_AVX2_HASH 1
#endif

#if defined(OPENRAR_HAS_AVX2_HASH)

#if defined(__GNUC__) || defined(__clang__)
#define OPENRAR_HASH_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define OPENRAR_HASH_TARGET_AVX2
#endif

namespace openrar::compress::arch {

// Lane i gathers bytes [i, i+3] of the 16-byte window, i.e. the little-endian 32-bit word
// at offset i. vpshufb is lane-local, so the 128-bit source is broadcast to both halves and
// each half selects its own four windows (offsets 0-3 low half, 4-7 high half).
alignas(32) inline constexpr uint8_t HASH_SHUFFLE_MASK256[32] = {0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4,
                                                                 5, 3, 4, 5, 6, 4, 5, 6, 7, 5, 6,
                                                                 7, 8, 6, 7, 8, 9, 7, 8, 9, 10};

// out[i] = (load_le32(p + i) * 0x9e3779b1u) >> (32 - HashBits), for i in [0, 8).
// Reads exactly 16 bytes from p (the last window needs bytes 7..10).
template <unsigned HashBits>
OPENRAR_HASH_TARGET_AVX2 inline void hash8_avx2(const core::byte* p, core::uint32* out) noexcept {
    static_assert(HashBits >= 1 && HashBits <= 31, "HashBits must leave a non-trivial shift");
    const __m256i mask = _mm256_load_si256(reinterpret_cast<const __m256i*>(HASH_SHUFFLE_MASK256));
    const __m256i mul = _mm256_set1_epi32(static_cast<int>(0x9e3779b1u));
    const __m128i raw16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m256i raw256 = _mm256_broadcastsi128_si256(raw16);
    const __m256i words = _mm256_shuffle_epi8(raw256, mask);
    const __m256i prod = _mm256_mullo_epi32(words, mul);
    const __m256i hashes = _mm256_srli_epi32(prod, 32 - HashBits);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), hashes);
}

} // namespace openrar::compress::arch

#endif // OPENRAR_HAS_AVX2_HASH

#endif // OPENRAR_COMPRESS_ARCH_HASH_SIMD_HPP
