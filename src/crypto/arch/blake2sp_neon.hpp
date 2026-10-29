#ifndef OPENRAR_CRYPTO_ARCH_BLAKE2SP_NEON_HPP
#define OPENRAR_CRYPTO_ARCH_BLAKE2SP_NEON_HPP

#if defined(__aarch64__) || defined(_M_ARM64)

#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include "../../core/types.hpp"
#include <cstring>

namespace openrar::crypto::arch {

namespace {

inline uint32x4_t neon_rotr16(uint32x4_t x) {
    return vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(x)));
}

inline uint32x4_t neon_rotr12(uint32x4_t x) {
    return vsliq_n_u32(vshrq_n_u32(x, 12), x, 20);
}

inline uint32x4_t neon_rotr8(uint32x4_t x) {
    return vsliq_n_u32(vshrq_n_u32(x, 8), x, 24);
}

inline uint32x4_t neon_rotr7(uint32x4_t x) {
    return vsliq_n_u32(vshrq_n_u32(x, 7), x, 25);
}

inline void g4(uint32x4_t& a, uint32x4_t& b, uint32x4_t& c, uint32x4_t& d, uint32x4_t x,
               uint32x4_t y) {
    a = vaddq_u32(vaddq_u32(a, b), x);
    d = neon_rotr16(veorq_u32(d, a));
    c = vaddq_u32(c, d);
    b = neon_rotr12(veorq_u32(b, c));
    a = vaddq_u32(vaddq_u32(a, b), y);
    d = neon_rotr8(veorq_u32(d, a));
    c = vaddq_u32(c, d);
    b = neon_rotr7(veorq_u32(b, c));
}

// 4x4 matrix transpose of 32-bit words in NEON.
// Given 4 rows r0..r3 of 4 words each, transposes them in-place so r_i holds
// the i-th word of all 4 rows.
inline void transpose4x4(uint32x4_t& r0, uint32x4_t& r1, uint32x4_t& r2, uint32x4_t& r3) {
    const uint32x4x2_t t0 = vtrnq_u32(r0, r1);
    const uint32x4x2_t t1 = vtrnq_u32(r2, r3);
    r0 = vcombine_u32(vget_low_u32(t0.val[0]), vget_low_u32(t1.val[0]));
    r1 = vcombine_u32(vget_low_u32(t0.val[1]), vget_low_u32(t1.val[1]));
    r2 = vcombine_u32(vget_high_u32(t0.val[0]), vget_high_u32(t1.val[0]));
    r3 = vcombine_u32(vget_high_u32(t0.val[1]), vget_high_u32(t1.val[1]));
}

// Load and transpose 16 words from 4 rows starting at byte offset `off`.
inline void load_transpose16(const core::byte* const rows[4], size_t off, uint32x4_t out[16]) {
    for (size_t col_blk = 0; col_blk < 4; ++col_blk) {
        uint32x4_t r0 =
            vld1q_u32(reinterpret_cast<const core::uint32*>(rows[0] + off + col_blk * 16));
        uint32x4_t r1 =
            vld1q_u32(reinterpret_cast<const core::uint32*>(rows[1] + off + col_blk * 16));
        uint32x4_t r2 =
            vld1q_u32(reinterpret_cast<const core::uint32*>(rows[2] + off + col_blk * 16));
        uint32x4_t r3 =
            vld1q_u32(reinterpret_cast<const core::uint32*>(rows[3] + off + col_blk * 16));
        transpose4x4(r0, r1, r2, r3);
        out[col_blk * 4 + 0] = r0;
        out[col_blk * 4 + 1] = r1;
        out[col_blk * 4 + 2] = r2;
        out[col_blk * 4 + 3] = r3;
    }
}

const core::uint32 NEON_BLAKE2S_IV[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                         0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

const core::byte NEON_SIGMA[10][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                       {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                                       {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
                                       {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                                       {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
                                       {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                                       {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
                                       {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                                       {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
                                       {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

inline void compress4(uint32x4_t h[8], const core::byte* const rows[4], core::uint32 t0,
                      core::uint32 t1) {
    uint32x4_t m[16];
    load_transpose16(rows, 0, m);

    uint32x4_t v[16];
    for (int i = 0; i < 8; ++i) v[i] = h[i];
    for (int i = 0; i < 4; ++i) v[8 + i] = vdupq_n_u32(NEON_BLAKE2S_IV[i]);
    v[12] = vdupq_n_u32(t0 ^ NEON_BLAKE2S_IV[4]);
    v[13] = vdupq_n_u32(t1 ^ NEON_BLAKE2S_IV[5]);
    v[14] = vdupq_n_u32(NEON_BLAKE2S_IV[6]);
    v[15] = vdupq_n_u32(NEON_BLAKE2S_IV[7]);

    for (int r = 0; r < 10; ++r) {
        const core::byte* s = NEON_SIGMA[r];
        g4(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        g4(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        g4(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        g4(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        g4(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        g4(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        g4(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        g4(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }

    for (int i = 0; i < 8; ++i) {
        h[i] = veorq_u32(h[i], veorq_u32(v[i], v[i + 8]));
    }
}

inline void step8_neon(uint32x4_t hA[8], uint32x4_t hB[8], const core::byte* const rows[8],
                       core::uint32& t0, core::uint32& t1, size_t block_size) {
    t0 += static_cast<core::uint32>(block_size);
    if (t0 < block_size) ++t1;
    compress4(hA, rows, t0, t1);
    compress4(hB, rows + 4, t0, t1);
}

} // namespace

} // namespace openrar::crypto::arch

#endif // __aarch64__ || _M_ARM64
#endif // OPENRAR_CRYPTO_ARCH_BLAKE2SP_NEON_HPP
