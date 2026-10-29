#include "blake2sp_avx512.hpp"

#if defined(OPENRAR_HAS_AVX512_KERNEL)

#include <immintrin.h>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#define OPENRAR_AVX512_FN __attribute__((target("avx512f,avx512bw,avx512vl,avx2")))
#else
#define OPENRAR_AVX512_FN
#endif

namespace openrar::crypto::arch {

namespace {

constexpr size_t BLOCK_SIZE = 64;
constexpr size_t PARALLELISM = 8;

const core::uint32 BLAKE2S_IV[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

const core::byte SIGMA[10][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                  {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                                  {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
                                  {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                                  {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
                                  {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                                  {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
                                  {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                                  {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
                                  {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

OPENRAR_AVX512_FN inline void g8_avx512(__m256i& a, __m256i& b, __m256i& c, __m256i& d, __m256i x,
                                        __m256i y) {
    a = _mm256_add_epi32(_mm256_add_epi32(a, b), x);
    d = _mm256_ror_epi32(_mm256_xor_si256(d, a), 16);
    c = _mm256_add_epi32(c, d);
    b = _mm256_ror_epi32(_mm256_xor_si256(b, c), 12);
    a = _mm256_add_epi32(_mm256_add_epi32(a, b), y);
    d = _mm256_ror_epi32(_mm256_xor_si256(d, a), 8);
    c = _mm256_add_epi32(c, d);
    b = _mm256_ror_epi32(_mm256_xor_si256(b, c), 7);
}

OPENRAR_AVX512_FN inline void transpose8x8(const core::byte* const rows[8], size_t off,
                                           __m256i out[8]) {
    __m256i r[8];
    for (int l = 0; l < 8; ++l) {
        r[l] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[l] + off));
    }
    const __m256i t0 = _mm256_unpacklo_epi32(r[0], r[1]), t1 = _mm256_unpackhi_epi32(r[0], r[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(r[2], r[3]), t3 = _mm256_unpackhi_epi32(r[2], r[3]);
    const __m256i t4 = _mm256_unpacklo_epi32(r[4], r[5]), t5 = _mm256_unpackhi_epi32(r[4], r[5]);
    const __m256i t6 = _mm256_unpacklo_epi32(r[6], r[7]), t7 = _mm256_unpackhi_epi32(r[6], r[7]);
    const __m256i u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);
    const __m256i u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);
    const __m256i u4 = _mm256_unpacklo_epi64(t4, t6), u5 = _mm256_unpackhi_epi64(t4, t6);
    const __m256i u6 = _mm256_unpacklo_epi64(t5, t7), u7 = _mm256_unpackhi_epi64(t5, t7);
    out[0] = _mm256_permute2x128_si256(u0, u4, 0x20);
    out[1] = _mm256_permute2x128_si256(u1, u5, 0x20);
    out[2] = _mm256_permute2x128_si256(u2, u6, 0x20);
    out[3] = _mm256_permute2x128_si256(u3, u7, 0x20);
    out[4] = _mm256_permute2x128_si256(u0, u4, 0x31);
    out[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
    out[6] = _mm256_permute2x128_si256(u2, u6, 0x31);
    out[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}

OPENRAR_AVX512_FN void compress8_avx512(__m256i h[8], const core::byte* const rows[8],
                                        core::uint32 t0, core::uint32 t1) {
    __m256i m[16];
    transpose8x8(rows, 0, m);
    transpose8x8(rows, 32, m + 8);

    __m256i v[16];
    for (int i = 0; i < 8; ++i) v[i] = h[i];
    for (int i = 0; i < 4; ++i) v[8 + i] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[i]));
    v[12] = _mm256_set1_epi32(static_cast<int>(t0 ^ BLAKE2S_IV[4]));
    v[13] = _mm256_set1_epi32(static_cast<int>(t1 ^ BLAKE2S_IV[5]));
    v[14] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[6]));
    v[15] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[7]));

    for (int r = 0; r < 10; ++r) {
        const core::byte* s = SIGMA[r];
        g8_avx512(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        g8_avx512(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        g8_avx512(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        g8_avx512(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        g8_avx512(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        g8_avx512(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        g8_avx512(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        g8_avx512(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h[i] = _mm256_xor_si256(h[i], _mm256_xor_si256(v[i], v[i + 8]));
}

OPENRAR_AVX512_FN void step8_avx512(__m256i h[8], const core::byte* const rows[8], core::uint32& t0,
                                    core::uint32& t1) {
    t0 += static_cast<core::uint32>(BLOCK_SIZE);
    if (t0 < BLOCK_SIZE) ++t1;
    compress8_avx512(h, rows, t0, t1);
}

} // namespace

OPENRAR_AVX512_FN void blake2sp_update_bulk_avx512(core::uint32 h_inout[8][8], core::uint32& t0,
                                                   core::uint32& t1,
                                                   const core::byte* const pending0[8],
                                                   const core::byte* const pending1[8],
                                                   const core::byte* in, size_t chunks) noexcept {
    alignas(32) core::uint32 lanes[8][8];
    for (int i = 0; i < 8; ++i) {
        for (int l = 0; l < 8; ++l) lanes[i][l] = h_inout[l][i];
    }
    __m256i h[8];
    for (int i = 0; i < 8; ++i)
        h[i] = _mm256_load_si256(reinterpret_cast<const __m256i*>(lanes[i]));

    step8_avx512(h, pending0, t0, t1);
    step8_avx512(h, pending1, t0, t1);

    const core::byte* rows[8];
    for (size_t c = 0; c + 2 < chunks; ++c) {
        const core::byte* base = in + c * PARALLELISM * BLOCK_SIZE;
        for (int l = 0; l < 8; ++l) rows[l] = base + static_cast<size_t>(l) * BLOCK_SIZE;
        step8_avx512(h, rows, t0, t1);
    }

    for (int i = 0; i < 8; ++i) _mm256_store_si256(reinterpret_cast<__m256i*>(lanes[i]), h[i]);
    for (int l = 0; l < 8; ++l) {
        for (int i = 0; i < 8; ++i) h_inout[l][i] = lanes[i][l];
    }
}

} // namespace openrar::crypto::arch

#endif // OPENRAR_HAS_AVX512_KERNEL
