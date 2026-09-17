#include "sha256.hpp"
#include "../core/cpu.hpp"
#include <algorithm>
#include <cstring>

// ─── Hardware Feature Detection & Intrinsics ─────────────────────────────────
#if defined(__EMSCRIPTEN__)
// WASM: scalar fallback only (no x86/ARM intrinsics)
#elif defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
#define OPENRAR_SHA_NI 1
#include <immintrin.h>
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#define OPENRAR_SHA_NI 1
#include <immintrin.h>
#include <cpuid.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#define OPENRAR_SHA_ARM 1
#include <arm64_neon.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64) ||                          \
    defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
#define OPENRAR_SHA_ARM 1
#include <arm_neon.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

namespace openrar::crypto {

namespace {

// ─── Scalar Implementation & Constants ───────────────────────────────────────

inline core::uint32 rotr(core::uint32 x, unsigned int n) {
    return (x >> n) | (x << (32 - n));
}

inline core::uint32 ch(core::uint32 x, core::uint32 y, core::uint32 z) {
    return (x & y) ^ (~x & z);
}

inline core::uint32 maj(core::uint32 x, core::uint32 y, core::uint32 z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline core::uint32 sig0(core::uint32 x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline core::uint32 sig1(core::uint32 x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline core::uint32 theta0(core::uint32 x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline core::uint32 theta1(core::uint32 x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

const core::uint32 K256[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u,
    0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu,
    0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u,
    0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u,
    0xC67178F2u};

static void sha256_transform_scalar(core::uint32* state, const core::byte* block) {
    core::uint32 w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = core::read_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = theta1(w[i - 2]) + w[i - 7] + theta0(w[i - 15]) + w[i - 16];
    }

    core::uint32 a = state[0];
    core::uint32 b = state[1];
    core::uint32 c = state[2];
    core::uint32 d = state[3];
    core::uint32 e = state[4];
    core::uint32 f = state[5];
    core::uint32 g = state[6];
    core::uint32 h = state[7];

    for (int i = 0; i < 64; ++i) {
        core::uint32 t1 = h + sig1(e) + ch(e, f, g) + K256[i] + w[i];
        core::uint32 t2 = sig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// ─── Intel SHA Extensions (SHA-NI) ───────────────────────────────────────────
#ifdef OPENRAR_SHA_NI

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha,sse4.1")))
#endif
static void
sha256_transform_shani(core::uint32* state, const core::byte* block) {
    // Transcribed 1:1 from Jeffrey Walton's public-domain SHA-NI implementation
    // (github.com/noloader/SHA-Intrinsics, sha256-x86.c), itself based on
    // Intel's SHA Extensions sample and the Linux kernel sha256_ni_transform
    // (same instruction schedule in both reference sources). The previous
    // in-tree version used _mm_xor_si128 in the message-schedule update
    // (the Wt-7 term must be *added* via paddd with the aligned tail of the
    // previous quad) and a state pack order that no canonical reference uses,
    // which produced wrong digests on every SHA-NI-capable CPU. This file has
    // no SHA-NI hardware on all test machines, so the runtime self-check in
    // Sha256::transform below gates the accelerated path on a KAT.
    __m128i STATE0, STATE1, MSG, TMP;
    __m128i MSG0, MSG1, MSG2, MSG3;
    __m128i ABEF_SAVE, CDGH_SAVE;
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    /* Load initial values */
    TMP = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    STATE1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));

    TMP = _mm_shuffle_epi32(TMP, 0xB1);          /* CDAB */
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);    /* EFGH */
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);    /* ABEF */
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0); /* CDGH */

    /* Save current hash */
    ABEF_SAVE = STATE0;
    CDGH_SAVE = STATE1;

    /* Rounds 0-15: messages */
    MSG0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 0)), MASK);
    MSG1 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), MASK);
    MSG2 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), MASK);
    MSG3 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), MASK);

    /* Rounds 0-3 */
    MSG = _mm_add_epi32(MSG0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[0])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 4-7 */
    MSG = _mm_add_epi32(MSG1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[4])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 8-11 */
    MSG = _mm_add_epi32(MSG2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[8])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 12-15 */
    MSG = _mm_add_epi32(MSG3, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[12])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 16-19 */
    MSG = _mm_add_epi32(MSG0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[16])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 20-23 */
    MSG = _mm_add_epi32(MSG1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[20])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 24-27 */
    MSG = _mm_add_epi32(MSG2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[24])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 28-31 */
    MSG = _mm_add_epi32(MSG3, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[28])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 32-35 */
    MSG = _mm_add_epi32(MSG0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[32])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 36-39 */
    MSG = _mm_add_epi32(MSG1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[36])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 40-43 */
    MSG = _mm_add_epi32(MSG2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[40])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 44-47 */
    MSG = _mm_add_epi32(MSG3, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[44])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 48-51 */
    MSG = _mm_add_epi32(MSG0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[48])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 52-55 */
    MSG = _mm_add_epi32(MSG1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[52])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 56-59 */
    MSG = _mm_add_epi32(MSG2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[56])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 60-63 */
    MSG = _mm_add_epi32(MSG3, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K256[60])));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Combine state */
    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

    /* Write hash values back in the correct order */
    TMP = _mm_shuffle_epi32(STATE0, 0x1B);       /* FEBA */
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);    /* DCHG */
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0); /* DCBA */
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);    /* HGFE */

    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), STATE0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), STATE1);
}

static bool shani_selftest() {
    core::uint32 st[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                          0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    core::byte blk[64] = {};
    blk[0] = 'a';
    blk[1] = 'b';
    blk[2] = 'c';
    blk[3] = 0x80;
    blk[63] = 24; // message bit-length 24 ("abc" = 3 bytes), big-endian low byte
    sha256_transform_shani(st, blk);
    // Full KAT for SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223
    //                               b00300a3 c8d026b4 410ff61f f20015ad.
    // All 8 words are checked (report INFO 2): comparing only st[0..3] and
    // st[6..7] let a defect corrupting st[4]/st[5] pass the gate.
    return st[0] == 0xba7816bfu && st[1] == 0x8f01cfeau && st[2] == 0x414140deu &&
           st[3] == 0x5dae2223u && st[4] == 0xb00300a3u && st[5] == 0xc8d026b4u &&
           st[6] == 0x410ff61fu && st[7] == 0xf20015adu;
}

#endif // OPENRAR_SHA_NI

// ─── ARMv8 SHA-256 Intrinsics (Linux, macOS/Apple Silicon, Windows ARM64) ────
#ifdef OPENRAR_SHA_ARM

static void sha256_transform_arm(core::uint32* state, const core::byte* block) {
    uint32x4_t q0 = vld1q_u32(reinterpret_cast<const uint32_t*>(block + 0));
    uint32x4_t q1 = vld1q_u32(reinterpret_cast<const uint32_t*>(block + 16));
    uint32x4_t q2 = vld1q_u32(reinterpret_cast<const uint32_t*>(block + 32));
    uint32x4_t q3 = vld1q_u32(reinterpret_cast<const uint32_t*>(block + 48));

    q0 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(q0)));
    q1 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(q1)));
    q2 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(q2)));
    q3 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(q3)));

    uint32x4_t abcd = vld1q_u32(&state[0]);
    uint32x4_t efgh = vld1q_u32(&state[4]);
    uint32x4_t abcd_orig = abcd;
    uint32x4_t efgh_orig = efgh;

    for (int i = 0; i < 48; i += 16) {
        uint32x4_t k0 = vld1q_u32(&K256[i + 0]);
        uint32x4_t k1 = vld1q_u32(&K256[i + 4]);
        uint32x4_t k2 = vld1q_u32(&K256[i + 8]);
        uint32x4_t k3 = vld1q_u32(&K256[i + 12]);

        uint32x4_t msg0 = vaddq_u32(q0, k0);
        uint32x4_t msg1 = vaddq_u32(q1, k1);
        uint32x4_t msg2 = vaddq_u32(q2, k2);
        uint32x4_t msg3 = vaddq_u32(q3, k3);

        uint32x4_t abcd_prev = abcd;
        abcd = vsha256hq_u32(abcd, efgh, msg0);
        efgh = vsha256h2q_u32(efgh, abcd_prev, msg0);
        q0 = vsha256su1q_u32(vsha256su0q_u32(q0, q1), q2, q3);

        abcd_prev = abcd;
        abcd = vsha256hq_u32(abcd, efgh, msg1);
        efgh = vsha256h2q_u32(efgh, abcd_prev, msg1);
        q1 = vsha256su1q_u32(vsha256su0q_u32(q1, q2), q3, q0);

        abcd_prev = abcd;
        abcd = vsha256hq_u32(abcd, efgh, msg2);
        efgh = vsha256h2q_u32(efgh, abcd_prev, msg2);
        q2 = vsha256su1q_u32(vsha256su0q_u32(q2, q3), q0, q1);

        abcd_prev = abcd;
        abcd = vsha256hq_u32(abcd, efgh, msg3);
        efgh = vsha256h2q_u32(efgh, abcd_prev, msg3);
        q3 = vsha256su1q_u32(vsha256su0q_u32(q3, q0), q1, q2);
    }

    uint32x4_t k0 = vld1q_u32(&K256[48]);
    uint32x4_t k1 = vld1q_u32(&K256[52]);
    uint32x4_t k2 = vld1q_u32(&K256[56]);
    uint32x4_t k3 = vld1q_u32(&K256[60]);

    uint32x4_t msg0 = vaddq_u32(q0, k0);
    uint32x4_t msg1 = vaddq_u32(q1, k1);
    uint32x4_t msg2 = vaddq_u32(q2, k2);
    uint32x4_t msg3 = vaddq_u32(q3, k3);

    uint32x4_t abcd_prev = abcd;
    abcd = vsha256hq_u32(abcd, efgh, msg0);
    efgh = vsha256h2q_u32(efgh, abcd_prev, msg0);

    abcd_prev = abcd;
    abcd = vsha256hq_u32(abcd, efgh, msg1);
    efgh = vsha256h2q_u32(efgh, abcd_prev, msg1);

    abcd_prev = abcd;
    abcd = vsha256hq_u32(abcd, efgh, msg2);
    efgh = vsha256h2q_u32(efgh, abcd_prev, msg2);

    abcd_prev = abcd;
    abcd = vsha256hq_u32(abcd, efgh, msg3);
    efgh = vsha256h2q_u32(efgh, abcd_prev, msg3);

    abcd = vaddq_u32(abcd, abcd_orig);
    efgh = vaddq_u32(efgh, efgh_orig);

    vst1q_u32(&state[0], abcd);
    vst1q_u32(&state[4], efgh);
}

#endif // OPENRAR_SHA_ARM

} // namespace

// ─── Sha256 Class Methods ────────────────────────────────────────────────────

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    state_[0] = 0x6A09E667u;
    state_[1] = 0xBB67AE85u;
    state_[2] = 0x3C6EF372u;
    state_[3] = 0xA54FF53Au;
    state_[4] = 0x510E527Fu;
    state_[5] = 0x9B05688Cu;
    state_[6] = 0x1F83D9ABu;
    state_[7] = 0x5BE0CD19u;
    count_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::transform(const core::byte* block) {
#ifdef OPENRAR_SHA_NI
    // Single source of truth for feature detection: core::get_cpu_features()
    // already reads CPUID leaf 7 EBX.SHA at startup, so no local duplicate.
    static const bool has_shani = core::get_cpu_features().sha_ni;
    // Runtime self-check: validate its output against the FIPS 180-4 "abc"
    // vector once; a broken variant disables the accelerated path for the
    // process lifetime instead of silently corrupting digests. (Only executed
    // on CPUs that report the feature, since the instructions are illegal
    // elsewhere.)
    static const bool shani_ok = has_shani ? shani_selftest() : false;
    if (has_shani && shani_ok) {
        sha256_transform_shani(state_, block);
        return;
    }
#endif
#ifdef OPENRAR_SHA_ARM
    static const bool has_sha_arm = core::get_cpu_features().arm_sha2;
    if (has_sha_arm) {
        sha256_transform_arm(state_, block);
        return;
    }
#endif

    sha256_transform_scalar(state_, block);
}

void Sha256::update(const void* data, size_t size) {
    const auto* p = static_cast<const core::byte*>(data);
    size_t idx = static_cast<size_t>(count_ & 0x3F);
    count_ += size;

    if (idx > 0) {
        size_t take = std::min(size, BLOCK_SIZE - idx);
        std::memcpy(buffer_ + idx, p, take);
        idx += take;
        p += take;
        size -= take;
        if (idx == BLOCK_SIZE) {
            transform(buffer_);
            idx = 0;
        }
    }

    while (size >= BLOCK_SIZE) {
        transform(p);
        p += BLOCK_SIZE;
        size -= BLOCK_SIZE;
    }

    if (size > 0) {
        std::memcpy(buffer_ + idx, p, size);
    }
}

void Sha256::finish(void* out_digest) {
    size_t idx = static_cast<size_t>(count_ & 0x3F);
    buffer_[idx++] = 0x80;

    if (idx > 56) {
        std::memset(buffer_ + idx, 0, BLOCK_SIZE - idx);
        transform(buffer_);
        idx = 0;
    }
    std::memset(buffer_ + idx, 0, 56 - idx);

    core::uint64 bits = count_ * 8;
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<core::byte>((bits >> (56 - i * 8)) & 0xFF);
    }
    transform(buffer_);

    auto* out = static_cast<core::byte*>(out_digest);
    for (int i = 0; i < 8; ++i) {
        core::write_be32(out + i * 4, state_[i]);
    }
}

void Sha256::compute(const void* data, size_t size, void* out_digest) {
    Sha256 ctx;
    ctx.update(data, size);
    ctx.finish(out_digest);
}

// ─── HMAC-SHA256 ─────────────────────────────────────────────────────────────

HmacSha256::HmacSha256(const void* key, size_t key_len) {
    core::byte k[Sha256::BLOCK_SIZE];
    std::memset(k, 0, sizeof(k));

    if (key_len > Sha256::BLOCK_SIZE) {
        Sha256::compute(key, key_len, k);
    } else {
        std::memcpy(k, key, key_len);
    }

    for (size_t i = 0; i < Sha256::BLOCK_SIZE; ++i) {
        k_ipad_[i] = k[i] ^ 0x36;
        k_opad_[i] = k[i] ^ 0x5C;
    }

    init_midstate();
}

void HmacSha256::init_midstate() {
    // Compression states after consuming the full ipad/opad blocks. reset()
    // then restores these directly; PBKDF2 triggers rebuild via reset() once
    // per iteration and the pad compression result is constant every time.
    inner_.reset();
    inner_.update(k_ipad_, Sha256::BLOCK_SIZE);
    core::uint64 bytes = 0;
    inner_.get_state(ipad_state_, bytes);

    outer_.reset();
    outer_.update(k_opad_, Sha256::BLOCK_SIZE);
    outer_.get_state(opad_state_, bytes);
}

namespace {
// File-local wipe for password derivatives stored in HmacSha256.
void hmac_secure_wipe(void* p, size_t n) noexcept {
    volatile core::byte* b = static_cast<volatile core::byte*>(p);
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}
} // namespace

HmacSha256::~HmacSha256() noexcept {
    hmac_secure_wipe(k_ipad_, sizeof(k_ipad_));
    hmac_secure_wipe(k_opad_, sizeof(k_opad_));
    hmac_secure_wipe(ipad_state_, sizeof(ipad_state_));
    hmac_secure_wipe(opad_state_, sizeof(opad_state_));
}

void HmacSha256::reset() {
    // Restore the cached pad midstates instead of re-feeding the full
    // 64-byte ipad/opad blocks: PBKDF2 calls reset() once per iteration
    // (2^15+ times), and the pad compression is identical every time.
    // The BLOCK_SIZE argument is the total byte counter after the whole
    // ipad/opad block: the standard HMAC length-compensation trick, so the
    // reused midstate continues as if the 64-byte pad block had been
    // streamed (report Q4: the "magic 64" is not a bug).
    inner_.set_state(ipad_state_, Sha256::BLOCK_SIZE);
    outer_.set_state(opad_state_, Sha256::BLOCK_SIZE);
}

void HmacSha256::update(const void* data, size_t size) {
    inner_.update(data, size);
}

void HmacSha256::finish(void* out_digest) {
    core::byte inner_digest[Sha256::DIGEST_SIZE];
    inner_.finish(inner_digest);

    outer_.update(inner_digest, sizeof(inner_digest));
    outer_.finish(out_digest);
}

void HmacSha256::compute(const void* key, size_t key_len, const void* data, size_t size,
                         void* out_digest) {
    HmacSha256 hmac(key, key_len);
    hmac.update(data, size);
    hmac.finish(out_digest);
}

} // namespace openrar::crypto
