#include "rs16.hpp"
#include "../core/cpu.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#elif !defined(__EMSCRIPTEN__) &&                                                                  \
    (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__))
#include <immintrin.h>
#include <tmmintrin.h>
#define OPENRAR_HAS_X86_RS16_SIMD 1
#endif

#if defined(OPENRAR_HAS_GFNI_KERNEL)
namespace openrar::recovery {
// Defined in rs16_gfni.cpp (the only TU compiled with AVX-512/GFNI flags).
// Folds 64-byte-multiples of the block; the remainder is handled scalar-side.
void rs16_fold_gfni(const core::byte* data, core::byte* ecc, size_t bytes,
                    const core::uint64 m[4]) noexcept;
} // namespace openrar::recovery
#endif

namespace openrar::recovery {

namespace {

#if defined(OPENRAR_HAS_GFNI_KERNEL)

// ── GFNI affinity matrices (v1.22.0) ─────────────────────────────────────────
// GF(2^16) multiply-by-constant distributes over the byte halves:
//   coeff * (hi<<8 | lo) = (hi * k1) ^ (lo * K0)
// with K0 = coeff and k1 = coeff * x^8 (both 16-bit constants). Each 8-bit ×
// 16-bit-constant product is a GF(2)-linear byte map 8 -> 16, i.e. TWO 8x8
// GF(2) matrices (one per output byte). Four matrices per coefficient:
//   m[0]: lo -> out_lo   m[1]: lo -> out_hi
//   m[2]: hi -> out_lo   m[3]: hi -> out_hi
//
// The _mm512_gf2p8affine_epi64_epi8 instruction applies an 8x8 GF(2)
// bit-matrix per byte; the convention probe below verifies our encoding of
// that matrix against the scalar table path at first use and falls back to
// the scalar path if no candidate matches — worst case is "no speedup",
// never wrong parity.

// ── GFNI calibration (v1.22.0) ───────────────────────────────────────────────
// The _mm512_gf2p8affine_epi64_epi8 instruction applies a per-byte 8x8 GF(2)
// bit-matrix encoded in the 64-bit lane of the second operand. Rather than
// trusting any spec reading of HOW the 64 qword bits map to the 64 matrix
// entries, the calibration MEASURES it: each one-hot qword (single set bit)
// folded against each one-hot input byte reveals exactly one matrix entry
// (the output row that fires for the input column). 64 qword bits x 8 input
// columns must cover every (row, column) entry exactly once; the composite
// fold is then re-verified against the scalar table reference before the
// kernel is allowed to dispatch. Fail safe end to end: any inconsistency =
// scalar path.

struct GfniCalib {
    bool valid{false};
    bool composite_ok{false};
    int qbit_of[8][8]; // qbit_of[row][col] = qword bit index for that entry
};

core::uint64 gfni_encode_byte_map(const ReedSolomon16& rs, core::uint32 k, bool high_half,
                                  const GfniCalib& cal);

const GfniCalib& gfni_calib() {
    static const GfniCalib cached = [] {
        GfniCalib c{};
        if (!core::get_cpu_features().gfni) return c;
        for (int r = 0; r < 8; ++r) {
            for (int col = 0; col < 8; ++col) c.qbit_of[r][col] = -1;
        }
        ReedSolomon16 rs; // GF tables only; matrix state is irrelevant here

        bool consistent = true;
        for (int p = 0; p < 64 && consistent; ++p) {
            const core::uint64 q = 1ULL << p;
            const core::uint64 m[4] = {q, q, q, q};
            for (int k = 0; k < 8 && consistent; ++k) {
                alignas(64) core::byte data[64] = {};
                alignas(64) core::byte ecc[64] = {};
                data[0] = static_cast<core::byte>(1u << k); // lo byte of word 0
                rs16_fold_gfni(data, ecc, sizeof(data), m);
                if (std::getenv("OPENRAR_GFNI_DIAG") != nullptr) {
                    std::cout << "[gfni-diag] qbit=" << p << " in_bit=" << k << " ecc0=0x"
                              << std::hex << static_cast<unsigned>(ecc[0]) << std::dec << " ecc1=0x"
                              << std::hex << static_cast<unsigned>(ecc[1]) << std::dec << "\n";
                }
                // hi byte of word 0 is zero and the transform is purely
                // linear, so ecc[0] == ecc[1] == the matrix column product.
                if (ecc[0] != ecc[1]) {
                    if (std::getenv("OPENRAR_GFNI_DIAG") != nullptr) {
                        std::cout << "[gfni-diag] INCONSISTENT at qbit=" << p << "\n";
                    }
                    consistent = false;
                    break;
                }
                if (ecc[0] == 0) continue; // this qbit does not touch column k
                // Exactly one output bit must fire: the entry's row.
                int row = -1;
                int ones = 0;
                for (int b = 0; b < 8; ++b) {
                    if (ecc[0] & (1u << b)) {
                        row = b;
                        ones++;
                    }
                }
                if (ones != 1 || c.qbit_of[row][k] >= 0) {
                    consistent = false;
                    break;
                }
                c.qbit_of[row][k] = p;
            }
        }
        if (consistent) {
            for (int r = 0; r < 8 && consistent; ++r) {
                for (int col = 0; col < 8 && consistent; ++col) {
                    if (c.qbit_of[r][col] < 0) consistent = false; // incomplete
                }
            }
        }
        if (std::getenv("OPENRAR_GFNI_DIAG") != nullptr) {
            std::cout << "[gfni-diag] consistent=" << consistent << "\n";
        }
        c.valid = consistent;

        if (c.valid) {
            // Composite check: full 64-byte fold through the measured encoding
            // vs the scalar table reference, for a non-trivial coefficient.
            constexpr core::uint32 COEFF = 0x1234;
            alignas(64) core::byte ref_ecc[64] = {};
            core::uint16 mul_lo[256], mul_hi[256];
            for (core::uint32 b = 0; b < 256; ++b) {
                mul_lo[b] = static_cast<core::uint16>(rs.gf_mul(COEFF, b));
                mul_hi[b] = static_cast<core::uint16>(rs.gf_mul(COEFF, b << 8));
            }
            for (core::uint32 w = 0; w < 32; ++w) {
                const core::uint16 word =
                    static_cast<core::uint16>((w * 251 + 7) | ((w * 31 + 3) << 8));
                const core::uint16 folded =
                    static_cast<core::uint16>(mul_lo[word & 0xFF] ^ mul_hi[(word >> 8) & 0xFF]);
                ref_ecc[2 * w] = static_cast<core::byte>(folded & 0xFF);
                ref_ecc[2 * w + 1] = static_cast<core::byte>((folded >> 8) & 0xFF);
            }
            const core::uint32 k1 = rs.gf_mul(COEFF, 0x100u);
            core::uint64 m[4];
            m[0] = gfni_encode_byte_map(rs, COEFF, false, c);
            m[1] = gfni_encode_byte_map(rs, COEFF, true, c);
            m[2] = gfni_encode_byte_map(rs, k1, false, c);
            m[3] = gfni_encode_byte_map(rs, k1, true, c);

            // Split composites: isolate the lo-byte path (m0/m1) from the
            // hi-byte path (m2/m3 + odd de-interleave), so a calibration
            // that validates one half but not the other is diagnosable.
            const bool diag = std::getenv("OPENRAR_GFNI_DIAG") != nullptr;
            for (int variant = 0; variant < 3 && c.valid; ++variant) {
                alignas(64) core::byte blk[64] = {};
                for (core::uint32 w = 0; w < 32; ++w) {
                    const core::uint16 word =
                        static_cast<core::uint16>((w * 251 + 7) | ((w * 31 + 3) << 8));
                    if (variant == 0) { // full
                        blk[2 * w] = static_cast<core::byte>(word & 0xFF);
                        blk[2 * w + 1] = static_cast<core::byte>((word >> 8) & 0xFF);
                    } else if (variant == 1) { // lo bytes only (hi = 0)
                        blk[2 * w] = static_cast<core::byte>(word & 0xFF);
                    } else { // hi bytes only (lo = 0)
                        blk[2 * w + 1] = static_cast<core::byte>((word >> 8) & 0xFF);
                    }
                }
                alignas(64) core::byte ecc[64] = {};
                rs16_fold_gfni(blk, ecc, sizeof(ecc), m);
                const bool ok = std::memcmp(ecc, ref_ecc, sizeof(ecc)) == 0;
                if (diag) {
                    std::cout << "[gfni-diag] composite variant=" << variant
                              << (variant == 0   ? " (full)"
                                  : variant == 1 ? " (lo-only)"
                                                 : " (hi-only)")
                              << " ok=" << ok << "\n";
                    for (int w = 0; w < 4; ++w) {
                        std::cout << "[gfni-diag]   w" << w << " ref=0x" << std::hex
                                  << static_cast<unsigned>(ref_ecc[2 * w]) << std::hex
                                  << static_cast<unsigned>(ref_ecc[2 * w + 1]) << std::dec
                                  << " got=0x" << std::hex << static_cast<unsigned>(ecc[2 * w])
                                  << std::hex << static_cast<unsigned>(ecc[2 * w + 1]) << std::dec
                                  << " in=0x" << std::hex << static_cast<unsigned>(blk[2 * w])
                                  << std::hex << static_cast<unsigned>(blk[2 * w + 1]) << std::dec
                                  << "\n";
                    }
                }
                if (variant == 0) c.composite_ok = ok;
            }
        }
        return c;
    }();
    return cached;
}

// Encode the byte map T(b) = one half of k*b as a qword under the measured
// convention: matrix entry (row, col) — dst_bit(row) = src_bit(col) AND
// entry — is placed at the calibrated qword bit for T's bit pattern.
core::uint64 gfni_encode_byte_map(const ReedSolomon16& rs, core::uint32 k, bool high_half,
                                  const GfniCalib& cal) {
    core::uint64 q = 0;
    // The loop walks the eight INPUT bit positions; `k` is the multiply
    // coefficient. It must not be named `k` too: a rename pass previously made
    // both `k`, so the coefficient parameter was shadowed and ignored and the
    // byte map was built for the bit index instead — a silent GFNI
    // miscompilation that only the calibration check masked (v1.41.1).
    for (int bit = 0; bit < 8; ++bit) {
        const core::uint32 product = rs.gf_mul(k, 1u << bit);
        const core::uint8 out_b =
            static_cast<core::uint8>(high_half ? (product >> 8) & 0xFF : product & 0xFF);
        for (int r = 0; r < 8; ++r) {
            if (out_b & (1u << r)) {
                q |= 1ULL << cal.qbit_of[r][bit];
            }
        }
    }
    return q;
}

#endif // OPENRAR_HAS_GFNI_KERNEL

#if defined(__aarch64__) || defined(_M_ARM64)

// ── NEON fold kernel (v1.22.0) ───────────────────────────────────────────────
// GF(2^16) multiply-by-constant distributes over the byte halves:
//   coeff * (hi<<8 | lo) = (lo * K0) ^ (hi * k1), K0 = coeff, k1 = coeff * x^8
// (the same algebra as mul_lo / mul_hi in update_ecc_scalar). Each byte *
// 16-bit-constant product is a GF(2)-linear byte map 8 -> 16, and a linear
// map splits over the input nibbles:
//   m(b) = m(b & 0xF) ^ m((b >> 4) << 4)
// so each map is four 16-entry byte tables (two output bytes x two nibble
// positions) applied with vqtbl1q — 16 words per iteration from two loads,
// one deinterleave pair, eight table lookups and a re-interleaved XOR into
// the ECC stream. Bit-exact by construction: the tables are built from the
// same gf_mul the scalar fold uses.
//
// Design note (the roadmap sketch named vmull_p64): a carry-less product of
// a byte by a degree-15 constant is NOT yet a field element — reducing it
// mod P costs a chain of further PMULLs, so lane math only pays off for
// 32/128-bit fields (CRC, GHASH). For degree-16 coefficients the nibble-map
// lookups above are fewer instructions per byte than any PMULL arrangement,
// and need no encoding calibration; the GFNI kernel covers the
// wide-instruction niche on x86.
struct NeonFoldTables {
    uint8x16_t n0_lo; // map(b) low output byte,  n = b & 0xF
    uint8x16_t n0_hi; // map(b) high output byte, n = b & 0xF
    uint8x16_t n1_lo; // map(b) low output byte,  n = b >> 4
    uint8x16_t n1_hi; // map(b) high output byte, n = b >> 4
};

NeonFoldTables make_neon_fold_tables(const ReedSolomon16& rs, core::uint32 k) {
    alignas(16) core::byte lo0[16];
    alignas(16) core::byte hi0[16];
    alignas(16) core::byte lo1[16];
    alignas(16) core::byte hi1[16];
    for (core::uint32 n = 0; n < 16; ++n) {
        const core::uint32 p0 = rs.gf_mul(k, n);
        const core::uint32 p1 = rs.gf_mul(k, n << 4);
        lo0[n] = static_cast<core::byte>(p0 & 0xFF);
        hi0[n] = static_cast<core::byte>((p0 >> 8) & 0xFF);
        lo1[n] = static_cast<core::byte>(p1 & 0xFF);
        hi1[n] = static_cast<core::byte>((p1 >> 8) & 0xFF);
    }
    return NeonFoldTables{vld1q_u8(lo0), vld1q_u8(hi0), vld1q_u8(lo1), vld1q_u8(hi1)};
}

// Folds 32-byte multiples of the block (16 words per iteration); the caller
// handles the remainder scalar-side.
void rs16_fold_neon(const core::byte* data, core::byte* ecc, size_t bytes, const NeonFoldTables& k0,
                    const NeonFoldTables& k1) noexcept {
    const uint8x16_t mask0f = vdupq_n_u8(0x0F);
    for (size_t off = 0; off + 32 <= bytes; off += 32) {
        const uint8x16_t d_a = vld1q_u8(data + off);
        const uint8x16_t d_b = vld1q_u8(data + off + 16);
        // Little-endian words: even bytes are the low half, odd the high.
        const uint8x16_t lo = vuzp1q_u8(d_a, d_b);
        const uint8x16_t hi = vuzp2q_u8(d_a, d_b);
        const uint8x16_t lon0 = vandq_u8(lo, mask0f);
        const uint8x16_t lon1 = vshrq_n_u8(lo, 4);
        const uint8x16_t hin0 = vandq_u8(hi, mask0f);
        const uint8x16_t hin1 = vshrq_n_u8(hi, 4);
        const uint8x16_t p_lo_lo = veorq_u8(vqtbl1q_u8(k0.n0_lo, lon0), vqtbl1q_u8(k0.n1_lo, lon1));
        const uint8x16_t p_lo_hi = veorq_u8(vqtbl1q_u8(k0.n0_hi, lon0), vqtbl1q_u8(k0.n1_hi, lon1));
        const uint8x16_t p_hi_lo = veorq_u8(vqtbl1q_u8(k1.n0_lo, hin0), vqtbl1q_u8(k1.n1_lo, hin1));
        const uint8x16_t p_hi_hi = veorq_u8(vqtbl1q_u8(k1.n0_hi, hin0), vqtbl1q_u8(k1.n1_hi, hin1));
        const uint8x16_t prod_lo = veorq_u8(p_lo_lo, p_hi_lo); // low bytes of the products
        const uint8x16_t prod_hi = veorq_u8(p_lo_hi, p_hi_hi); // high bytes
        // Back to memory order: word i = [low byte, high byte].
        const uint8x16_t e_a = vld1q_u8(ecc + off);
        const uint8x16_t e_b = vld1q_u8(ecc + off + 16);
        vst1q_u8(ecc + off, veorq_u8(e_a, vzip1q_u8(prod_lo, prod_hi)));
        vst1q_u8(ecc + off + 16, veorq_u8(e_b, vzip2q_u8(prod_lo, prod_hi)));
    }
}

#endif // __aarch64__ || _M_ARM64

#if defined(OPENRAR_HAS_X86_RS16_SIMD)

struct X86FoldTables {
    alignas(16) core::byte lo0[16];
    alignas(16) core::byte hi0[16];
    alignas(16) core::byte lo1[16];
    alignas(16) core::byte hi1[16];
};

X86FoldTables make_x86_fold_tables(const ReedSolomon16& rs, core::uint32 k) {
    X86FoldTables t{};
    for (core::uint32 n = 0; n < 16; ++n) {
        const core::uint32 p0 = rs.gf_mul(k, n);
        const core::uint32 p1 = rs.gf_mul(k, n << 4);
        t.lo0[n] = static_cast<core::byte>(p0 & 0xFF);
        t.hi0[n] = static_cast<core::byte>((p0 >> 8) & 0xFF);
        t.lo1[n] = static_cast<core::byte>(p1 & 0xFF);
        t.hi1[n] = static_cast<core::byte>((p1 >> 8) & 0xFF);
    }
    return t;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
void rs16_fold_ssse3(const core::byte* data, core::byte* ecc, size_t bytes, const X86FoldTables& k0,
                     const X86FoldTables& k1) noexcept {
    const __m128i mask0f = _mm_set1_epi8(0x0F);
    const __m128i shuf_deint = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15);
    const __m128i k0_lo0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k0.lo0));
    const __m128i k0_hi0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k0.hi0));
    const __m128i k0_lo1 = _mm_load_si128(reinterpret_cast<const __m128i*>(k0.lo1));
    const __m128i k0_hi1 = _mm_load_si128(reinterpret_cast<const __m128i*>(k0.hi1));
    const __m128i k1_lo0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1.lo0));
    const __m128i k1_hi0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1.hi0));
    const __m128i k1_lo1 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1.lo1));
    const __m128i k1_hi1 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1.hi1));

    for (size_t off = 0; off + 32 <= bytes; off += 32) {
        const __m128i d_a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + off));
        const __m128i d_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + off + 16));
        const __m128i a = _mm_shuffle_epi8(d_a, shuf_deint);
        const __m128i b = _mm_shuffle_epi8(d_b, shuf_deint);
        const __m128i lo = _mm_unpacklo_epi64(a, b);
        const __m128i hi = _mm_unpackhi_epi64(a, b);
        const __m128i lon0 = _mm_and_si128(lo, mask0f);
        const __m128i lon1 = _mm_and_si128(_mm_srli_epi16(lo, 4), mask0f);
        const __m128i hin0 = _mm_and_si128(hi, mask0f);
        const __m128i hin1 = _mm_and_si128(_mm_srli_epi16(hi, 4), mask0f);
        const __m128i p_lo_lo =
            _mm_xor_si128(_mm_shuffle_epi8(k0_lo0, lon0), _mm_shuffle_epi8(k0_lo1, lon1));
        const __m128i p_lo_hi =
            _mm_xor_si128(_mm_shuffle_epi8(k0_hi0, lon0), _mm_shuffle_epi8(k0_hi1, lon1));
        const __m128i p_hi_lo =
            _mm_xor_si128(_mm_shuffle_epi8(k1_lo0, hin0), _mm_shuffle_epi8(k1_lo1, hin1));
        const __m128i p_hi_hi =
            _mm_xor_si128(_mm_shuffle_epi8(k1_hi0, hin0), _mm_shuffle_epi8(k1_hi1, hin1));
        const __m128i prod_lo = _mm_xor_si128(p_lo_lo, p_hi_lo);
        const __m128i prod_hi = _mm_xor_si128(p_lo_hi, p_hi_hi);
        const __m128i e_a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ecc + off));
        const __m128i e_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ecc + off + 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ecc + off),
                         _mm_xor_si128(e_a, _mm_unpacklo_epi8(prod_lo, prod_hi)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ecc + off + 16),
                         _mm_xor_si128(e_b, _mm_unpackhi_epi8(prod_lo, prod_hi)));
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
void rs16_fold_avx2(const core::byte* data, core::byte* ecc, size_t bytes, const X86FoldTables& k0,
                    const X86FoldTables& k1) noexcept {
    const __m256i mask0f = _mm256_set1_epi8(0x0F);
    const __m256i shuf_deint = _mm256_broadcastsi128_si256(
        _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15));
    const __m256i k0_lo0 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k0.lo0)));
    const __m256i k0_hi0 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k0.hi0)));
    const __m256i k0_lo1 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k0.lo1)));
    const __m256i k0_hi1 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k0.hi1)));
    const __m256i k1_lo0 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k1.lo0)));
    const __m256i k1_hi0 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k1.hi0)));
    const __m256i k1_lo1 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k1.lo1)));
    const __m256i k1_hi1 =
        _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(k1.hi1)));

    for (size_t off = 0; off + 64 <= bytes; off += 64) {
        const __m256i d_a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + off));
        const __m256i d_b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + off + 32));
        const __m256i a = _mm256_shuffle_epi8(d_a, shuf_deint);
        const __m256i b = _mm256_shuffle_epi8(d_b, shuf_deint);
        const __m256i lo = _mm256_unpacklo_epi64(a, b);
        const __m256i hi = _mm256_unpackhi_epi64(a, b);
        const __m256i lon0 = _mm256_and_si256(lo, mask0f);
        const __m256i lon1 = _mm256_and_si256(_mm256_srli_epi16(lo, 4), mask0f);
        const __m256i hin0 = _mm256_and_si256(hi, mask0f);
        const __m256i hin1 = _mm256_and_si256(_mm256_srli_epi16(hi, 4), mask0f);
        const __m256i p_lo_lo =
            _mm256_xor_si256(_mm256_shuffle_epi8(k0_lo0, lon0), _mm256_shuffle_epi8(k0_lo1, lon1));
        const __m256i p_lo_hi =
            _mm256_xor_si256(_mm256_shuffle_epi8(k0_hi0, lon0), _mm256_shuffle_epi8(k0_hi1, lon1));
        const __m256i p_hi_lo =
            _mm256_xor_si256(_mm256_shuffle_epi8(k1_lo0, hin0), _mm256_shuffle_epi8(k1_lo1, hin1));
        const __m256i p_hi_hi =
            _mm256_xor_si256(_mm256_shuffle_epi8(k1_hi0, hin0), _mm256_shuffle_epi8(k1_hi1, hin1));
        const __m256i prod_lo = _mm256_xor_si256(p_lo_lo, p_hi_lo);
        const __m256i prod_hi = _mm256_xor_si256(p_lo_hi, p_hi_hi);
        const __m256i e_a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ecc + off));
        const __m256i e_b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ecc + off + 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(ecc + off),
                            _mm256_xor_si256(e_a, _mm256_unpacklo_epi8(prod_lo, prod_hi)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(ecc + off + 32),
                            _mm256_xor_si256(e_b, _mm256_unpackhi_epi8(prod_lo, prod_hi)));
    }
}

#endif // OPENRAR_HAS_X86_RS16_SIMD

// ── Field tables ─────────────────────────────────────────────────────────────
// The multiplicative group of GF(2^16) is cyclic of order 65535, so every
// nonzero element is some power of the generator alpha = x. Two flat arrays
// hold the full mapping:
//
//   power[i]      = alpha^i          for i in [0, 65535)
//   exponent[v]   = i | alpha^i = v  for v in (0, 65535]
//
// exponent[0] is written but never read: gf_mul/gf_inv test the zero operand
// explicitly before touching the tables. That keeps both arrays at their exact
// mathematical size and removes every special-case region.
struct GfField {
    static constexpr core::uint32 ORDER = ReedSolomon16::GF_SIZE; // 65535 = 2^16 - 1

    core::uint16 power[ORDER];
    core::uint32 exponent[ORDER + 1];

    GfField() {
        core::uint32 v = 1;
        for (core::uint32 i = 0; i < ORDER; ++i) {
            exponent[v] = i;
            power[i] = static_cast<core::uint16>(v);
            // Multiply by alpha and reduce modulo x^16+x^12+x^3+x+1. The top
            // bit of v is the coefficient of x^15, so shifting it out past
            // x^16 is exactly the condition for subtracting the modulus;
            // XOR is subtraction in characteristic two. Only the low 16 bits
            // of the modulus matter because the x^16 term cancels.
            const core::uint32 carry = v >> 15;
            v = ((v << 1) & 0xFFFFu) ^ (carry * 0x100Bu);
        }
        exponent[0] = 0; // unused; see gf_mul/gf_inv
    }
};

const GfField& shared_gf_field() {
    static const GfField field{};
    return field;
}

} // namespace

bool ReedSolomon16::gfni_kernel_active() {
#if defined(OPENRAR_HAS_GFNI_KERNEL)
    const GfniCalib& cal = gfni_calib();
    return cal.valid && cal.composite_ok;
#else
    return false;
#endif
}

bool ReedSolomon16::neon_kernel_active() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

ReedSolomon16::ReedSolomon16() {
    init_gf();
}

void ReedSolomon16::init_gf() {
    const GfField& field = shared_gf_field();
    antilog_ = field.power;
    logarithm_ = field.exponent;
}

bool ReedSolomon16::init(core::uint32 data_count, core::uint32 rec_count,
                         const core::byte* valid_flags) {
    nd_ = data_count;
    nr_ = rec_count;
    ne_ = 0;

    if (nd_ == 0 || nr_ == 0 || nd_ + nr_ > GF_SIZE) {
        return false;
    }

    decoding_ = (valid_flags != nullptr);

    if (decoding_) {
        valid_flags_.assign(valid_flags, valid_flags + nd_ + nr_);
        for (core::uint32 i = 0; i < nd_; ++i) {
            if (!valid_flags_[i]) ne_++;
        }

        core::uint32 valid_ecc = 0;
        for (core::uint32 i = nd_; i < nd_ + nr_; ++i) {
            if (valid_flags_[i]) valid_ecc++;
        }

        if (ne_ > valid_ecc || ne_ == 0 || valid_ecc == 0) {
            return false; // Not recoverable
        }

        // B10-class sizing note: ne_/nd_/nr_ are bounded by init()'s
        // nd_ + nr_ <= 65535 check, so these products stay far below 2^32
        // elements — but they ARE archive-controlled (crafted .rev/RR
        // headers). If that bound is ever loosened, route the product
        // through an overflow-checked helper first.
        mx_.resize(static_cast<size_t>(ne_) * nd_);
        derive_recovery_coefficients();
    } else {
        mx_.resize(static_cast<size_t>(nr_) * nd_);
        fill_parity_coefficients();
    }

    return true;
}

void ReedSolomon16::fill_parity_coefficients() {
    // Systematic Cauchy generator. Parity unit i holds the sum over data units
    // d of D_d / (x_i ^ y_d), where the x-coordinates are the parity slots and
    // the y-coordinates the data slots. All x_i differ from all y_d and from
    // each other, which makes every square submatrix of this matrix invertible.
    for (core::uint32 i = 0; i < nr_; ++i) {
        core::uint32* row = &mx_[static_cast<size_t>(i) * nd_];
        const core::uint32 x_coord = nd_ + i;
        for (core::uint32 d = 0; d < nd_; ++d) {
            row[d] = gf_inv(gf_add(x_coord, d));
        }
    }
}

void ReedSolomon16::derive_recovery_coefficients() {
    // Contract with the callers in recovery_record.cpp and recovery_writer.cpp:
    // in the working slot vector the r-th missing data unit is replaced by the
    // r-th available parity unit, in ascending slot order. The rows produced
    // here are applied to exactly that vector, so a healthy slot contributes
    // its own data and a missing slot contributes its substitute.
    std::vector<core::uint32> erased(ne_);
    std::vector<core::uint32> substitutes(ne_);

    core::uint32 e = 0;
    for (core::uint32 slot = 0; slot < nd_ && e < ne_; ++slot) {
        if (!valid_flags_[slot]) erased[e++] = slot;
    }
    core::uint32 p = 0;
    for (core::uint32 slot = nd_; slot < nd_ + nr_ && p < ne_; ++slot) {
        if (valid_flags_[slot]) substitutes[p++] = slot;
    }
    if (e != ne_ || p != ne_) {
        mx_.assign(static_cast<size_t>(ne_) * nd_, 0);
        return; // unreachable after init()'s recoverability check
    }

    // Square system restricted to the erased units: row i is the parity
    // relation of substitute i, column r the erased unit r. A Cauchy matrix is
    // nonsingular for distinct coordinates, so this always has a solution.
    std::vector<core::uint32> system(static_cast<size_t>(ne_) * ne_);
    for (core::uint32 i = 0; i < ne_; ++i) {
        for (core::uint32 r = 0; r < ne_; ++r) {
            system[static_cast<size_t>(i) * ne_ + r] = gf_inv(gf_add(substitutes[i], erased[r]));
        }
    }
    if (!invert_square(system, ne_)) {
        mx_.assign(static_cast<size_t>(ne_) * nd_, 0);
        return;
    }

    mx_.assign(static_cast<size_t>(ne_) * nd_, 0);
    for (core::uint32 k = 0; k < ne_; ++k) {
        core::uint32* row = &mx_[static_cast<size_t>(k) * nd_];
        const core::uint32* solved = &system[static_cast<size_t>(k) * ne_];
        // Healthy data units are known, so their contribution is removed from
        // every substitute equation before solving. Removing is XOR, because
        // the field has characteristic two.
        for (core::uint32 d = 0; d < nd_; ++d) {
            if (!valid_flags_[d]) continue;
            core::uint32 acc = 0;
            for (core::uint32 i = 0; i < ne_; ++i) {
                acc ^= gf_mul(solved[i], gf_inv(gf_add(substitutes[i], d)));
            }
            row[d] = acc;
        }
        // A substitute parity unit occupies the slot of the erased unit it
        // stands in for, so its coefficient is the solved row itself.
        for (core::uint32 r = 0; r < ne_; ++r) {
            row[erased[r]] = solved[r];
        }
    }
}

bool ReedSolomon16::invert_square(std::vector<core::uint32>& m, core::uint32 n) const {
    const size_t stride = n;
    std::vector<core::uint32> lu(m);
    std::vector<core::uint32> original_row(n);
    for (core::uint32 i = 0; i < n; ++i) {
        original_row[i] = i;
    }

    // LU factorisation with partial pivoting. A Cauchy matrix never needs a
    // swap, but searching for the pivot keeps the routine correct for any
    // nonsingular input and turns a singular one into an explicit failure.
    for (core::uint32 col = 0; col < n; ++col) {
        core::uint32 sel = col;
        while (sel < n && lu[static_cast<size_t>(sel) * stride + col] == 0) {
            ++sel;
        }
        if (sel == n) return false;
        if (sel != col) {
            std::swap_ranges(lu.begin() + static_cast<std::ptrdiff_t>(sel) * stride,
                             lu.begin() + static_cast<std::ptrdiff_t>(sel + 1) * stride,
                             lu.begin() + static_cast<std::ptrdiff_t>(col) * stride);
            std::swap(original_row[sel], original_row[col]);
        }
        const core::uint32 pivot_inverse = gf_inv(lu[static_cast<size_t>(col) * stride + col]);
        for (core::uint32 row = col + 1; row < n; ++row) {
            const core::uint32 factor =
                gf_mul(lu[static_cast<size_t>(row) * stride + col], pivot_inverse);
            lu[static_cast<size_t>(row) * stride + col] = factor;
            if (factor == 0) continue;
            for (core::uint32 j = col + 1; j < n; ++j) {
                lu[static_cast<size_t>(row) * stride + j] ^=
                    gf_mul(factor, lu[static_cast<size_t>(col) * stride + j]);
            }
        }
    }

    // Invert by solving A x = e_c for every unit vector and storing the
    // solutions as columns of the result.
    std::vector<core::uint32> interim(n);
    std::vector<core::uint32> inverse(static_cast<size_t>(n) * n, 0);
    for (core::uint32 c = 0; c < n; ++c) {
        for (core::uint32 row = 0; row < n; ++row) { // forward: L y = P e_c
            core::uint32 acc = (original_row[row] == c) ? 1u : 0u;
            for (core::uint32 j = 0; j < row; ++j) {
                acc ^= gf_mul(lu[static_cast<size_t>(row) * stride + j], interim[j]);
            }
            interim[row] = acc;
        }
        for (core::uint32 row = n; row-- > 0;) { // back: U x = y
            core::uint32 acc = interim[row];
            for (core::uint32 j = row + 1; j < n; ++j) {
                acc ^= gf_mul(lu[static_cast<size_t>(row) * stride + j],
                              inverse[static_cast<size_t>(j) * n + c]);
            }
            inverse[static_cast<size_t>(row) * n + c] =
                gf_mul(acc, gf_inv(lu[static_cast<size_t>(row) * stride + row]));
        }
    }
    m.swap(inverse);
    return true;
}

void ReedSolomon16::update_ecc_scalar(core::uint32 data_num, core::uint32 ecc_num,
                                      const core::byte* data, core::byte* ecc, size_t block_size) {
    if (data_num == 0) {
        std::memset(ecc, 0, block_size);
    }

    // Multiply-by-constant tables for this (data_num, ecc_num) pair:
    // GF(2^16) mul distributes over XOR, so mul(coeff, (hi<<8)|lo) =
    // mul(coeff, lo) ^ mul(coeff, hi<<8). Two 256-entry uint16 tables (1 KiB
    // total, L1-hot) replace the log/exp pair-lookups against the 256 KiB
    // antilog_/logarithm_ set with their long dependent-load chain. That also
    // removes the need for the caller to have pre-computed per-word
    // logarithms of this block: the coefficient no longer depends on call
    // order, so any (data_num, ecc_num) pair folds correctly at any time.
    const core::uint32 coeff = mx_[ecc_num * nd_ + data_num];
    if (coeff == 0) {
        return; // x^0: product is 0 for every word, nothing to fold in
    }

    const core::byte* d = data;
    core::byte* e_ptr = ecc;
    size_t remaining = block_size / 2;

    // Building mul_lo/mul_hi is a FIXED 512 gf_mul calls (~1 KiB of stores)
    // no matter how small the block is — measured at roughly 1 us, which is
    // about half the entire cost of folding a 200-byte group. group_count is
    // ceil(archive_size / d), so every small archive folds through groups that
    // never amortise it. Below the threshold, fold with direct log/exp
    // multiplies instead: the same gf_mul the tables are built from, so the
    // result is bit-identical by construction
    // (mul_lo[b & 0xFF] ^ mul_hi[b >> 8] == gf_mul(coeff, b)), and at or above
    // the threshold nothing changes at all. The GFNI and NEON remainder loops
    // already make exactly this trade (v1.41.1).
    constexpr size_t TABLE_AMORTISE_BYTES = 2048;
    if (block_size < TABLE_AMORTISE_BYTES) {
        while (remaining-- > 0) {
            core::uint16 b = static_cast<core::uint16>(d[0] | (d[1] << 8));
            d += 2;
            core::uint16 ec = static_cast<core::uint16>(e_ptr[0] | (e_ptr[1] << 8));
            ec ^= static_cast<core::uint16>(gf_mul(coeff, b));
            e_ptr[0] = static_cast<core::byte>(ec & 0xFF);
            e_ptr[1] = static_cast<core::byte>((ec >> 8) & 0xFF);
            e_ptr += 2;
        }
        return;
    }

    core::uint16 mul_lo[256], mul_hi[256];
    for (core::uint32 i = 0; i < 256; ++i) {
        mul_lo[i] = static_cast<core::uint16>(gf_mul(coeff, i));
        mul_hi[i] = static_cast<core::uint16>(gf_mul(coeff, i << 8));
    }

    // x4 unroll; read/write bytes explicitly so bounds are exact.
    while (remaining >= 4) {
        for (int k = 0; k < 4; ++k) {
            core::uint16 b = static_cast<core::uint16>(d[0] | (d[1] << 8));
            d += 2;
            core::uint16 ec = static_cast<core::uint16>(e_ptr[0] | (e_ptr[1] << 8));
            ec ^= mul_lo[b & 0xFF] ^ mul_hi[(b >> 8) & 0xFF];
            e_ptr[0] = static_cast<core::byte>(ec & 0xFF);
            e_ptr[1] = static_cast<core::byte>((ec >> 8) & 0xFF);
            e_ptr += 2;
        }
        remaining -= 4;
    }
    while (remaining-- > 0) {
        core::uint16 b = static_cast<core::uint16>(d[0] | (d[1] << 8));
        d += 2;
        core::uint16 ec = static_cast<core::uint16>(e_ptr[0] | (e_ptr[1] << 8));
        ec ^= mul_lo[b & 0xFF] ^ mul_hi[(b >> 8) & 0xFF];
        e_ptr[0] = static_cast<core::byte>(ec & 0xFF);
        e_ptr[1] = static_cast<core::byte>((ec >> 8) & 0xFF);
        e_ptr += 2;
    }
}

void ReedSolomon16::update_ecc(core::uint32 data_num, core::uint32 ecc_num, const core::byte* data,
                               core::byte* ecc, size_t block_size) {
    if (data_num == 0) {
        std::memset(ecc, 0, block_size);
    }

    const core::uint32 coeff = mx_[ecc_num * nd_ + data_num];
    if (coeff == 0) {
        return;
    }

#if defined(OPENRAR_HAS_GFNI_KERNEL)
    const GfniCalib& cal = gfni_calib();
    if (cal.valid && cal.composite_ok) {
        // GF(2^16) mul distributes over the byte halves:
        //   coeff * (hi<<8 | lo) = (lo * K0) ^ (hi * k1), k1 = coeff * x^8.
        // Each 8-bit x 16-bit-constant product is a GF(2)-linear byte map,
        // i.e. two 8x8 matrices; the calibration has measured the
        // instruction's byte/bit mapping empirically, so the kernel is
        // bit-identical to the scalar fold.
        core::uint64 m[4];
        m[0] = gfni_encode_byte_map(*this, coeff, /*high_half=*/false, cal);
        m[1] = gfni_encode_byte_map(*this, coeff, /*high_half=*/true, cal);
        const core::uint32 k1 = gf_mul(coeff, 0x100u);
        m[2] = gfni_encode_byte_map(*this, k1, /*high_half=*/false, cal);
        m[3] = gfni_encode_byte_map(*this, k1, /*high_half=*/true, cal);

        const size_t body = block_size & ~static_cast<size_t>(63);
        if (body > 0) {
            rs16_fold_gfni(data, ecc, body, m);
        }
        const size_t done = body;
        // Scalar remainder (< 64 bytes): direct log/exp muls, cheaper than
        // building 1 KiB of tables for a handful of words.
        for (size_t off = done; off + 2 <= block_size; off += 2) {
            const core::uint16 word = static_cast<core::uint16>(data[off] | (data[off + 1] << 8));
            const core::uint16 product = static_cast<core::uint16>(gf_mul(coeff, word));
            const core::uint16 ec = static_cast<core::uint16>(
                static_cast<core::uint16>(ecc[off] | (ecc[off + 1] << 8)) ^ product);
            ecc[off] = static_cast<core::byte>(ec & 0xFF);
            ecc[off + 1] = static_cast<core::byte>((ec >> 8) & 0xFF);
        }
        return;
    }
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    // NEON fold kernel: Advanced SIMD is architecturally mandatory on
    // AArch64 (and baseline on MSVC ARM64), so compile-time gating is the
    // whole dispatch — no runtime probe, no calibration (the tables are
    // built from the same gf_mul the scalar path uses).
    const size_t body = block_size & ~static_cast<size_t>(31);
    if (body > 0) {
        const NeonFoldTables k0 = make_neon_fold_tables(*this, coeff);
        const NeonFoldTables k1 = make_neon_fold_tables(*this, gf_mul(coeff, 0x100u));
        rs16_fold_neon(data, ecc, body, k0, k1);
        // Scalar remainder (< 32 bytes): direct log/exp muls, cheaper than
        // building tables for a handful of words.
        for (size_t off = body; off + 2 <= block_size; off += 2) {
            const core::uint16 word = static_cast<core::uint16>(data[off] | (data[off + 1] << 8));
            const core::uint16 product = static_cast<core::uint16>(gf_mul(coeff, word));
            const core::uint16 ec = static_cast<core::uint16>(
                static_cast<core::uint16>(ecc[off] | (ecc[off + 1] << 8)) ^ product);
            ecc[off] = static_cast<core::byte>(ec & 0xFF);
            ecc[off + 1] = static_cast<core::byte>((ec >> 8) & 0xFF);
        }
        return;
    }
#endif

#if defined(OPENRAR_HAS_X86_RS16_SIMD)
    const auto& cpu = core::get_cpu_features();
    if (cpu.avx2 && block_size >= 64) {
        const size_t body = block_size & ~static_cast<size_t>(63);
        const X86FoldTables k0 = make_x86_fold_tables(*this, coeff);
        const X86FoldTables k1 = make_x86_fold_tables(*this, gf_mul(coeff, 0x100u));
        rs16_fold_avx2(data, ecc, body, k0, k1);
        for (size_t off = body; off + 2 <= block_size; off += 2) {
            const core::uint16 word = static_cast<core::uint16>(data[off] | (data[off + 1] << 8));
            const core::uint16 product = static_cast<core::uint16>(gf_mul(coeff, word));
            const core::uint16 ec = static_cast<core::uint16>(
                static_cast<core::uint16>(ecc[off] | (ecc[off + 1] << 8)) ^ product);
            ecc[off] = static_cast<core::byte>(ec & 0xFF);
            ecc[off + 1] = static_cast<core::byte>((ec >> 8) & 0xFF);
        }
        return;
    }
    if (cpu.ssse3 && block_size >= 32) {
        const size_t body = block_size & ~static_cast<size_t>(31);
        const X86FoldTables k0 = make_x86_fold_tables(*this, coeff);
        const X86FoldTables k1 = make_x86_fold_tables(*this, gf_mul(coeff, 0x100u));
        rs16_fold_ssse3(data, ecc, body, k0, k1);
        for (size_t off = body; off + 2 <= block_size; off += 2) {
            const core::uint16 word = static_cast<core::uint16>(data[off] | (data[off + 1] << 8));
            const core::uint16 product = static_cast<core::uint16>(gf_mul(coeff, word));
            const core::uint16 ec = static_cast<core::uint16>(
                static_cast<core::uint16>(ecc[off] | (ecc[off + 1] << 8)) ^ product);
            ecc[off] = static_cast<core::byte>(ec & 0xFF);
            ecc[off + 1] = static_cast<core::byte>((ec >> 8) & 0xFF);
        }
        return;
    }
#endif

    update_ecc_scalar(data_num, ecc_num, data, ecc, block_size);
}

} // namespace openrar::recovery
