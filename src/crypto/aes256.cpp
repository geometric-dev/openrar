#include "aes256.hpp"
#include "../core/cpu.hpp"
#include <algorithm>
#include <cstring>

// ─── Hardware Feature Detection & Intrinsics ─────────────────────────────────
// WASM (Emscripten) has no AES-NI / NEON intrinsics in this translation unit.
#if defined(__EMSCRIPTEN__)
// force scalar fallback
#elif defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
#define OPENRAR_AES_NI 1
#include <wmmintrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#define OPENRAR_AES_NI 1
#include <wmmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#define OPENRAR_AES_NEON 1
#include <arm64_neon.h>
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64) ||                          \
    defined(__ARM_FEATURE_CRYPTO) || defined(__ARM_FEATURE_AES)
#define OPENRAR_AES_NEON 1
#include <arm_neon.h>
#endif

namespace openrar::crypto {

namespace {

// ─── Scalar Tables and Fallback Helpers ───────────────────────────────────────

const core::byte SBOX[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16};

const core::byte INV_SBOX[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D};

const core::uint32 RCON[10] = {0x01000000u, 0x02000000u, 0x04000000u, 0x08000000u, 0x10000000u,
                               0x20000000u, 0x40000000u, 0x80000000u, 0x1B000000u, 0x36000000u};

inline core::byte xtime(core::byte x) {
    return (x << 1) ^ ((x & 0x80) ? 0x1B : 0x00);
}

inline core::byte multiply(core::byte x, core::byte y) {
    core::byte result = 0;
    while (y != 0) {
        if (y & 1) result ^= x;
        x = xtime(x);
        y >>= 1;
    }
    return result;
}

inline core::uint32 sub_word(core::uint32 w) {
    return (static_cast<core::uint32>(SBOX[(w >> 24) & 0xFF]) << 24) |
           (static_cast<core::uint32>(SBOX[(w >> 16) & 0xFF]) << 16) |
           (static_cast<core::uint32>(SBOX[(w >> 8) & 0xFF]) << 8) |
           (static_cast<core::uint32>(SBOX[w & 0xFF]));
}

inline core::uint32 rot_word(core::uint32 w) {
    return (w << 8) | (w >> 24);
}

inline core::uint32 inv_mix_column(core::uint32 w) {
    core::byte a = (w >> 24) & 0xFF;
    core::byte b = (w >> 16) & 0xFF;
    core::byte c = (w >> 8) & 0xFF;
    core::byte d = w & 0xFF;

    core::byte r0 = multiply(a, 0x0E) ^ multiply(b, 0x0B) ^ multiply(c, 0x0D) ^ multiply(d, 0x09);
    core::byte r1 = multiply(a, 0x09) ^ multiply(b, 0x0E) ^ multiply(c, 0x0B) ^ multiply(d, 0x0D);
    core::byte r2 = multiply(a, 0x0D) ^ multiply(b, 0x09) ^ multiply(c, 0x0E) ^ multiply(d, 0x0B);
    core::byte r3 = multiply(a, 0x0B) ^ multiply(b, 0x0D) ^ multiply(c, 0x09) ^ multiply(d, 0x0E);

    return (static_cast<core::uint32>(r0) << 24) | (static_cast<core::uint32>(r1) << 16) |
           (static_cast<core::uint32>(r2) << 8) | static_cast<core::uint32>(r3);
}

// ─── AES-NI Hardware Acceleration (x86 / x64) ────────────────────────────────
#ifdef OPENRAR_AES_NI

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes,sse4.1")))
#endif
static __m128i aes_xor_assist(__m128i a, __m128i b) noexcept {
    __m128i t = _mm_slli_si128(a, 4);
    a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4);
    a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4);
    a = _mm_xor_si128(a, t);
    return _mm_xor_si128(a, b);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes,sse4.1")))
#endif
static void aesni_expand_key(const core::byte* key, core::byte* enc_buf,
                             core::byte* dec_buf) noexcept {
    auto* ek = reinterpret_cast<__m128i*>(enc_buf);
    auto* dk = reinterpret_cast<__m128i*>(dec_buf);

    __m128i t1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
    __m128i t2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key + 16));
    ek[0] = t1;
    ek[1] = t2;

#define KE1(rc)                                                                                    \
    t1 = aes_xor_assist(t1, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(t2, (rc)), 0xFF))
#define KE2() t2 = aes_xor_assist(t2, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(t1, 0x00), 0xAA))
    KE1(0x01);
    ek[2] = t1;
    KE2();
    ek[3] = t2;
    KE1(0x02);
    ek[4] = t1;
    KE2();
    ek[5] = t2;
    KE1(0x04);
    ek[6] = t1;
    KE2();
    ek[7] = t2;
    KE1(0x08);
    ek[8] = t1;
    KE2();
    ek[9] = t2;
    KE1(0x10);
    ek[10] = t1;
    KE2();
    ek[11] = t2;
    KE1(0x20);
    ek[12] = t1;
    KE2();
    ek[13] = t2;
    KE1(0x40);
    ek[14] = t1;
#undef KE1
#undef KE2

    dk[0] = ek[14];
    for (int i = 1; i <= 13; ++i) {
        dk[i] = _mm_aesimc_si128(ek[14 - i]);
    }
    dk[14] = ek[0];
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes")))
#endif
static inline __m128i aesni_enc_block(const __m128i& pt, const __m128i* ek) noexcept {
    __m128i m = _mm_xor_si128(pt, ek[0]);
    m = _mm_aesenc_si128(m, ek[1]);
    m = _mm_aesenc_si128(m, ek[2]);
    m = _mm_aesenc_si128(m, ek[3]);
    m = _mm_aesenc_si128(m, ek[4]);
    m = _mm_aesenc_si128(m, ek[5]);
    m = _mm_aesenc_si128(m, ek[6]);
    m = _mm_aesenc_si128(m, ek[7]);
    m = _mm_aesenc_si128(m, ek[8]);
    m = _mm_aesenc_si128(m, ek[9]);
    m = _mm_aesenc_si128(m, ek[10]);
    m = _mm_aesenc_si128(m, ek[11]);
    m = _mm_aesenc_si128(m, ek[12]);
    m = _mm_aesenc_si128(m, ek[13]);
    return _mm_aesenclast_si128(m, ek[14]);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes")))
#endif
static inline __m128i aesni_dec_block(const __m128i& ct, const __m128i* dk) noexcept {
    __m128i m = _mm_xor_si128(ct, dk[0]);
    m = _mm_aesdec_si128(m, dk[1]);
    m = _mm_aesdec_si128(m, dk[2]);
    m = _mm_aesdec_si128(m, dk[3]);
    m = _mm_aesdec_si128(m, dk[4]);
    m = _mm_aesdec_si128(m, dk[5]);
    m = _mm_aesdec_si128(m, dk[6]);
    m = _mm_aesdec_si128(m, dk[7]);
    m = _mm_aesdec_si128(m, dk[8]);
    m = _mm_aesdec_si128(m, dk[9]);
    m = _mm_aesdec_si128(m, dk[10]);
    m = _mm_aesdec_si128(m, dk[11]);
    m = _mm_aesdec_si128(m, dk[12]);
    m = _mm_aesdec_si128(m, dk[13]);
    return _mm_aesdeclast_si128(m, dk[14]);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes")))
#endif
static void aesni_encrypt_cbc(core::byte* data, size_t size, core::byte* iv,
                              const core::byte* enc_buf) noexcept {
    const auto* ek = reinterpret_cast<const __m128i*>(enc_buf);
    __m128i chain = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
    size_t blocks = size / 16;
    auto* p = data;

    for (size_t b = 0; b < blocks; ++b, p += 16) {
        chain = aesni_enc_block(
            _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), chain), ek);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p), chain);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(iv), chain);
}

// 8-way pipelined CBC decryption hides ~4-cycle latency of AES decrypt instructions
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("aes")))
#endif
static void aesni_decrypt_cbc(core::byte* data, size_t size, core::byte* iv,
                              const core::byte* dec_buf) noexcept {
    const auto* dk = reinterpret_cast<const __m128i*>(dec_buf);
    __m128i prev = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
    size_t blocks = size / 16;
    auto* p = data;

    while (blocks >= 8) {
        __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
        __m128i c2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32));
        __m128i c3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48));
        __m128i c4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 64));
        __m128i c5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 80));
        __m128i c6 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 96));
        __m128i c7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 112));

        __m128i d0 = aesni_dec_block(c0, dk);
        __m128i d1 = aesni_dec_block(c1, dk);
        __m128i d2 = aesni_dec_block(c2, dk);
        __m128i d3 = aesni_dec_block(c3, dk);
        __m128i d4 = aesni_dec_block(c4, dk);
        __m128i d5 = aesni_dec_block(c5, dk);
        __m128i d6 = aesni_dec_block(c6, dk);
        __m128i d7 = aesni_dec_block(c7, dk);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_xor_si128(d0, prev));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 16), _mm_xor_si128(d1, c0));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 32), _mm_xor_si128(d2, c1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 48), _mm_xor_si128(d3, c2));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 64), _mm_xor_si128(d4, c3));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 80), _mm_xor_si128(d5, c4));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 96), _mm_xor_si128(d6, c5));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 112), _mm_xor_si128(d7, c6));

        prev = c7;
        p += 128;
        blocks -= 8;
    }

    while (blocks-- > 0) {
        __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p),
                         _mm_xor_si128(aesni_dec_block(c, dk), prev));
        prev = c;
        p += 16;
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(iv), prev);
}

#endif // OPENRAR_AES_NI

// ─── ARM NEON AES Acceleration (Linux, macOS/Apple Silicon, Windows ARM64) ───
#ifdef OPENRAR_AES_NEON

static inline uint8x16_t neon_enc_block(uint8x16_t pt, const core::byte* enc_buf) noexcept {
    const auto* rk = reinterpret_cast<const uint8x16_t*>(enc_buf);
    uint8x16_t m = veorq_u8(pt, rk[0]);
    for (int r = 1; r <= 13; ++r) {
        m = vaeseq_u8(m, vdupq_n_u8(0));
        m = vaesmcq_u8(m);
        m = veorq_u8(m, rk[r]);
    }
    m = vaeseq_u8(m, vdupq_n_u8(0));
    return veorq_u8(m, rk[14]);
}

static inline uint8x16_t neon_dec_block(uint8x16_t ct, const core::byte* dec_buf) noexcept {
    const auto* rk = reinterpret_cast<const uint8x16_t*>(dec_buf);
    uint8x16_t m = veorq_u8(ct, rk[0]);
    for (int r = 1; r <= 13; ++r) {
        m = vaesdq_u8(m, vdupq_n_u8(0));
        m = vaesimcq_u8(m);
        m = veorq_u8(m, rk[r]);
    }
    m = vaesdq_u8(m, vdupq_n_u8(0));
    return veorq_u8(m, rk[14]);
}

#endif // OPENRAR_AES_NEON

} // namespace

// ─── Aes256 Class Methods ────────────────────────────────────────────────────

// Best-effort wipe of sensitive material: volatile loop so the compiler
// cannot elide the dead store. (Compilers are permitted to remove plain
// memset-on-exit; volatile forces the write.)
static void secure_wipe(void* p, size_t n) noexcept {
    volatile core::byte* b = static_cast<volatile core::byte*>(p);
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}

Aes256::Aes256() : has_ni_(false) {
    std::memset(enc_round_keys_, 0, sizeof(enc_round_keys_));
    std::memset(dec_round_keys_, 0, sizeof(dec_round_keys_));
    std::memset(ni_enc_keys_, 0, sizeof(ni_enc_keys_));
    std::memset(ni_dec_keys_, 0, sizeof(ni_dec_keys_));
}

Aes256::Aes256(const core::byte* key) : has_ni_(false) {
    std::memset(enc_round_keys_, 0, sizeof(enc_round_keys_));
    std::memset(dec_round_keys_, 0, sizeof(dec_round_keys_));
    std::memset(ni_enc_keys_, 0, sizeof(ni_enc_keys_));
    std::memset(ni_dec_keys_, 0, sizeof(ni_dec_keys_));
    set_key(key);
}

void Aes256::set_key(const core::byte* key) {
    expand_key(key);
}

Aes256::~Aes256() noexcept {
    secure_wipe(enc_round_keys_, sizeof(enc_round_keys_));
    secure_wipe(dec_round_keys_, sizeof(dec_round_keys_));
    secure_wipe(ni_enc_keys_, sizeof(ni_enc_keys_));
    secure_wipe(ni_dec_keys_, sizeof(ni_dec_keys_));
}

void Aes256::expand_key(const core::byte* key) {
    has_ni_ = false;

#ifdef OPENRAR_AES_NI
    if (core::get_cpu_features().aes_ni) {
        aesni_expand_key(key, ni_enc_keys_, ni_dec_keys_);
        has_ni_ = true;
        return;
    }
#endif

    // Scalar key expansion (Nk = 8, Nr = 14)
    for (int i = 0; i < 8; ++i) {
        enc_round_keys_[i] = core::read_be32(key + i * 4);
    }

    for (int i = 8; i < 60; ++i) {
        core::uint32 temp = enc_round_keys_[i - 1];
        if (i % 8 == 0) {
            temp = sub_word(rot_word(temp)) ^ RCON[(i / 8) - 1];
        } else if (i % 8 == 4) {
            temp = sub_word(temp);
        }
        enc_round_keys_[i] = enc_round_keys_[i - 8] ^ temp;
    }

    for (int r = 0; r <= 14; ++r) {
        int enc_r = 14 - r;
        for (int c = 0; c < 4; ++c) {
            core::uint32 w = enc_round_keys_[enc_r * 4 + c];
            if (r == 0 || r == 14) {
                dec_round_keys_[r * 4 + c] = w;
            } else {
                dec_round_keys_[r * 4 + c] = inv_mix_column(w);
            }
        }
    }

#ifdef OPENRAR_AES_NEON
    if (core::get_cpu_features().arm_aes) {
        // Repack scalar keys into NEON byte layout
        for (int r = 0; r < 15; ++r) {
            for (int w = 0; w < 4; ++w) {
                core::uint32 v = enc_round_keys_[r * 4 + w];
                ni_enc_keys_[r * 16 + w * 4 + 0] = static_cast<core::byte>((v >> 24) & 0xFF);
                ni_enc_keys_[r * 16 + w * 4 + 1] = static_cast<core::byte>((v >> 16) & 0xFF);
                ni_enc_keys_[r * 16 + w * 4 + 2] = static_cast<core::byte>((v >> 8) & 0xFF);
                ni_enc_keys_[r * 16 + w * 4 + 3] = static_cast<core::byte>(v & 0xFF);

                v = dec_round_keys_[r * 4 + w];
                ni_dec_keys_[r * 16 + w * 4 + 0] = static_cast<core::byte>((v >> 24) & 0xFF);
                ni_dec_keys_[r * 16 + w * 4 + 1] = static_cast<core::byte>((v >> 16) & 0xFF);
                ni_dec_keys_[r * 16 + w * 4 + 2] = static_cast<core::byte>((v >> 8) & 0xFF);
                ni_dec_keys_[r * 16 + w * 4 + 3] = static_cast<core::byte>(v & 0xFF);
            }
        }
        has_ni_ = true;
    }
#endif
}

void Aes256::encrypt_block(const core::byte* in, core::byte* out) const {
#ifdef OPENRAR_AES_NI
    if (has_ni_) {
        auto ek = reinterpret_cast<const __m128i*>(ni_enc_keys_);
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(out),
            aesni_enc_block(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), ek));
        return;
    }
#endif
#ifdef OPENRAR_AES_NEON
    if (has_ni_) {
        vst1q_u8(out, neon_enc_block(vld1q_u8(in), ni_enc_keys_));
        return;
    }
#endif

    // Scalar fallback
    core::byte state[4][4];
    for (int i = 0; i < 16; ++i) {
        state[i % 4][i / 4] = in[i];
    }

    for (int c = 0; c < 4; ++c) {
        core::uint32 rk = enc_round_keys_[c];
        state[0][c] ^= (rk >> 24) & 0xFF;
        state[1][c] ^= (rk >> 16) & 0xFF;
        state[2][c] ^= (rk >> 8) & 0xFF;
        state[3][c] ^= rk & 0xFF;
    }

    for (int round = 1; round <= 14; ++round) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) state[r][c] = SBOX[state[r][c]];

        core::byte temp = state[1][0];
        state[1][0] = state[1][1];
        state[1][1] = state[1][2];
        state[1][2] = state[1][3];
        state[1][3] = temp;

        temp = state[2][0];
        core::byte temp2 = state[2][1];
        state[2][0] = state[2][2];
        state[2][1] = state[2][3];
        state[2][2] = temp;
        state[2][3] = temp2;

        temp = state[3][3];
        state[3][3] = state[3][2];
        state[3][2] = state[3][1];
        state[3][1] = state[3][0];
        state[3][0] = temp;

        if (round < 14) {
            for (int c = 0; c < 4; ++c) {
                core::byte a0 = state[0][c], a1 = state[1][c], a2 = state[2][c], a3 = state[3][c];
                state[0][c] = xtime(a0 ^ a1) ^ a1 ^ a2 ^ a3;
                state[1][c] = xtime(a1 ^ a2) ^ a2 ^ a3 ^ a0;
                state[2][c] = xtime(a2 ^ a3) ^ a3 ^ a0 ^ a1;
                state[3][c] = xtime(a3 ^ a0) ^ a0 ^ a1 ^ a2;
            }
        }

        for (int c = 0; c < 4; ++c) {
            core::uint32 rk = enc_round_keys_[round * 4 + c];
            state[0][c] ^= (rk >> 24) & 0xFF;
            state[1][c] ^= (rk >> 16) & 0xFF;
            state[2][c] ^= (rk >> 8) & 0xFF;
            state[3][c] ^= rk & 0xFF;
        }
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = state[i % 4][i / 4];
    }
}

void Aes256::decrypt_block(const core::byte* in, core::byte* out) const {
#ifdef OPENRAR_AES_NI
    if (has_ni_) {
        auto dk = reinterpret_cast<const __m128i*>(ni_dec_keys_);
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(out),
            aesni_dec_block(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), dk));
        return;
    }
#endif
#ifdef OPENRAR_AES_NEON
    if (has_ni_) {
        vst1q_u8(out, neon_dec_block(vld1q_u8(in), ni_dec_keys_));
        return;
    }
#endif

    // Scalar fallback
    core::byte state[4][4];
    for (int i = 0; i < 16; ++i) {
        state[i % 4][i / 4] = in[i];
    }

    for (int c = 0; c < 4; ++c) {
        core::uint32 rk = dec_round_keys_[c];
        state[0][c] ^= (rk >> 24) & 0xFF;
        state[1][c] ^= (rk >> 16) & 0xFF;
        state[2][c] ^= (rk >> 8) & 0xFF;
        state[3][c] ^= rk & 0xFF;
    }

    for (int round = 1; round <= 14; ++round) {
        core::byte temp = state[1][3];
        state[1][3] = state[1][2];
        state[1][2] = state[1][1];
        state[1][1] = state[1][0];
        state[1][0] = temp;

        temp = state[2][2];
        core::byte temp2 = state[2][3];
        state[2][2] = state[2][0];
        state[2][3] = state[2][1];
        state[2][0] = temp;
        state[2][1] = temp2;

        temp = state[3][0];
        state[3][0] = state[3][1];
        state[3][1] = state[3][2];
        state[3][2] = state[3][3];
        state[3][3] = temp;

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) state[r][c] = INV_SBOX[state[r][c]];

        if (round < 14) {
            for (int c = 0; c < 4; ++c) {
                core::byte a0 = state[0][c], a1 = state[1][c], a2 = state[2][c], a3 = state[3][c];
                state[0][c] = multiply(a0, 0x0E) ^ multiply(a1, 0x0B) ^ multiply(a2, 0x0D) ^
                              multiply(a3, 0x09);
                state[1][c] = multiply(a0, 0x09) ^ multiply(a1, 0x0E) ^ multiply(a2, 0x0B) ^
                              multiply(a3, 0x0D);
                state[2][c] = multiply(a0, 0x0D) ^ multiply(a1, 0x09) ^ multiply(a2, 0x0E) ^
                              multiply(a3, 0x0B);
                state[3][c] = multiply(a0, 0x0B) ^ multiply(a1, 0x0D) ^ multiply(a2, 0x09) ^
                              multiply(a3, 0x0E);
            }
        }

        for (int c = 0; c < 4; ++c) {
            core::uint32 rk = dec_round_keys_[round * 4 + c];
            state[0][c] ^= (rk >> 24) & 0xFF;
            state[1][c] ^= (rk >> 16) & 0xFF;
            state[2][c] ^= (rk >> 8) & 0xFF;
            state[3][c] ^= rk & 0xFF;
        }
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = state[i % 4][i / 4];
    }
}

void Aes256::encrypt_cbc(core::byte* data, size_t size, core::byte* iv) const {
#ifdef OPENRAR_AES_NI
    if (has_ni_) {
        aesni_encrypt_cbc(data, size, iv, ni_enc_keys_);
        return;
    }
#endif
#ifdef OPENRAR_AES_NEON
    if (has_ni_) {
        uint8x16_t chain = vld1q_u8(iv);
        size_t blocks = size / BLOCK_SIZE;
        auto* p = data;
        for (size_t b = 0; b < blocks; ++b, p += 16) {
            chain = neon_enc_block(veorq_u8(vld1q_u8(p), chain), ni_enc_keys_);
            vst1q_u8(p, chain);
        }
        vst1q_u8(iv, chain);
        return;
    }
#endif

    // Scalar fallback
    size_t blocks = size / BLOCK_SIZE;
    auto* p = data;
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            p[i] ^= iv[i];
        }
        encrypt_block(p, p);
        std::memcpy(iv, p, BLOCK_SIZE);
        p += BLOCK_SIZE;
    }
}

void Aes256::decrypt_cbc(core::byte* data, size_t size, core::byte* iv) const {
    // INTENDED (output-parameter mutation): on return `iv` holds the LAST
    // CIPHERTEXT block of this call, not the caller's original value. Every
    // implementation below (scalar, AES-NI, NEON) writes it back, and the
    // encrypted-header reader chains successive header blocks through it
    // (format/header_reader.cpp). Do not change decrypt_cbc into a
    // non-mutating take-IV-by-value API.
#ifdef OPENRAR_AES_NI
    if (has_ni_) {
        aesni_decrypt_cbc(data, size, iv, ni_dec_keys_);
        return;
    }
#endif
#ifdef OPENRAR_AES_NEON
    if (has_ni_) {
        uint8x16_t prev = vld1q_u8(iv);
        size_t blocks = size / BLOCK_SIZE;
        auto* p = data;
        for (size_t b = 0; b < blocks; ++b, p += 16) {
            uint8x16_t c = vld1q_u8(p);
            vst1q_u8(p, veorq_u8(neon_dec_block(c, ni_dec_keys_), prev));
            prev = c;
        }
        vst1q_u8(iv, prev);
        return;
    }
#endif

    // Scalar fallback
    size_t blocks = size / BLOCK_SIZE;
    auto* p = data;
    core::byte next_iv[BLOCK_SIZE];

    for (size_t b = 0; b < blocks; ++b) {
        std::memcpy(next_iv, p, BLOCK_SIZE);
        decrypt_block(p, p);
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            p[i] ^= iv[i];
        }
        std::memcpy(iv, next_iv, BLOCK_SIZE);
        p += BLOCK_SIZE;
    }
}

} // namespace openrar::crypto
