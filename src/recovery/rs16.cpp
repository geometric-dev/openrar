#include "rs16.hpp"
#include "../core/cpu.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>

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
//   coeff * (hi<<8 | lo) = (hi * K1) ^ (lo * K0)
// with K0 = coeff and K1 = coeff * x^8 (both 16-bit constants). Each 8-bit ×
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

core::uint64 gfni_encode_byte_map(const ReedSolomon16& rs, core::uint32 K, bool high_half,
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
            constexpr core::uint32 kCoeff = 0x1234;
            alignas(64) core::byte ref_ecc[64] = {};
            core::uint16 mul_lo[256], mul_hi[256];
            for (core::uint32 b = 0; b < 256; ++b) {
                mul_lo[b] = static_cast<core::uint16>(rs.gf_mul(kCoeff, b));
                mul_hi[b] = static_cast<core::uint16>(rs.gf_mul(kCoeff, b << 8));
            }
            alignas(64) core::byte block[64];
            for (core::uint32 w = 0; w < 32; ++w) {
                const core::uint16 word =
                    static_cast<core::uint16>((w * 251 + 7) | ((w * 31 + 3) << 8));
                block[2 * w] = static_cast<core::byte>(word & 0xFF);
                block[2 * w + 1] = static_cast<core::byte>((word >> 8) & 0xFF);
                const core::uint16 folded =
                    static_cast<core::uint16>(mul_lo[word & 0xFF] ^ mul_hi[(word >> 8) & 0xFF]);
                ref_ecc[2 * w] = static_cast<core::byte>(folded & 0xFF);
                ref_ecc[2 * w + 1] = static_cast<core::byte>((folded >> 8) & 0xFF);
            }
            const core::uint32 K1 = rs.gf_mul(kCoeff, 0x100u);
            core::uint64 m[4];
            m[0] = gfni_encode_byte_map(rs, kCoeff, false, c);
            m[1] = gfni_encode_byte_map(rs, kCoeff, true, c);
            m[2] = gfni_encode_byte_map(rs, K1, false, c);
            m[3] = gfni_encode_byte_map(rs, K1, true, c);

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

// Encode the byte map T(b) = one half of K*b as a qword under the measured
// convention: matrix entry (row, col) — dst_bit(row) = src_bit(col) AND
// entry — is placed at the calibrated qword bit for T's bit pattern.
core::uint64 gfni_encode_byte_map(const ReedSolomon16& rs, core::uint32 K, bool high_half,
                                  const GfniCalib& cal) {
    core::uint64 q = 0;
    for (int k = 0; k < 8; ++k) {
        const core::uint32 product = rs.gf_mul(K, 1u << k);
        const core::uint8 out_b =
            static_cast<core::uint8>(high_half ? (product >> 8) & 0xFF : product & 0xFF);
        for (int r = 0; r < 8; ++r) {
            if (out_b & (1u << r)) {
                q |= 1ULL << cal.qbit_of[r][k];
            }
        }
    }
    return q;
}

#endif // OPENRAR_HAS_GFNI_KERNEL

} // namespace

bool ReedSolomon16::gfni_kernel_active() {
#if defined(OPENRAR_HAS_GFNI_KERNEL)
    const GfniCalib& cal = gfni_calib();
    return cal.valid && cal.composite_ok;
#else
    return false;
#endif
}

ReedSolomon16::ReedSolomon16() {
    init_gf();
}

void ReedSolomon16::init_gf() {
    gf_exp_.resize(4 * GF_SIZE + 1, 0);
    gf_log_.resize(GF_SIZE + 1, 0);

    core::uint32 e = 1;
    for (core::uint32 l = 0; l < GF_SIZE; ++l) {
        gf_log_[e] = l;
        gf_exp_[l] = e;
        gf_exp_[l + GF_SIZE] = e; // Duplicate table to avoid modulo operations
        e <<= 1;
        if (e > GF_SIZE) {
            e ^= 0x1100Bu; // Irreducible primitive polynomial: x^16 + x^12 + x^3 + x + 1
        }
    }

    // Set sentinel for log(0) so exp[log(0) + log(x)] == 0
    gf_log_[0] = 2 * GF_SIZE;
    for (core::uint32 i = 2 * GF_SIZE; i <= 4 * GF_SIZE; ++i) {
        gf_exp_[i] = 0;
    }
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
        mx_.resize(ne_ * nd_);
        make_decoder_matrix();
        invert_decoder_matrix();
    } else {
        mx_.resize(nr_ * nd_);
        make_encoder_matrix();
    }

    return true;
}

void ReedSolomon16::make_encoder_matrix() {
    // Cauchy generator matrix: MX[i][j] = 1 / ((i + ND) ^ j)
    for (core::uint32 i = 0; i < nr_; ++i) {
        for (core::uint32 j = 0; j < nd_; ++j) {
            mx_[i * nd_ + j] = gf_inv(gf_add(i + nd_, j));
        }
    }
}

void ReedSolomon16::make_decoder_matrix() {
    // Select rows corresponding to available valid recovery units for missing data positions
    core::uint32 r = nd_;
    core::uint32 dest = 0;

    for (core::uint32 flag = 0; flag < nd_; ++flag) {
        if (!valid_flags_[flag]) {
            while (!valid_flags_[r]) {
                r++;
            }
            for (core::uint32 j = 0; j < nd_; ++j) {
                mx_[dest * nd_ + j] = gf_inv(gf_add(r, j));
            }
            dest++;
            r++;
        }
    }
}

void ReedSolomon16::invert_decoder_matrix() {
    std::vector<core::uint32> mi(ne_ * nd_, 0);

    core::uint32 kr = 0, kf = 0;
    for (; kr < ne_; kr++, kf++) {
        while (valid_flags_[kf]) {
            kf++;
        }
        mi[kr * nd_ + kf] = 1; // Identity matrix on diagonal
    }

    kr = 0;
    kf = 0;
    for (; kf < nd_; kr++, kf++) {
        // Bounds check first (report INFO 3): valid_flags_ is sized nd_+nr_,
        // so the previous operand order (index before bound) was safe only
        // by allocation accident (nr_ >= 1).
        while (kf < nd_ && valid_flags_[kf]) {
            for (core::uint32 i = 0; i < ne_; ++i) {
                mi[i * nd_ + kf] ^= mx_[i * nd_ + kf];
            }
            kf++;
        }

        if (kf == nd_) break;

        core::uint32* mx_k = &mx_[kr * nd_];
        core::uint32* mi_k = &mi[kr * nd_];

        core::uint32 p_inv = gf_inv(mx_k[kf]);
        for (core::uint32 i = 0; i < nd_; ++i) {
            mx_k[i] = gf_mul(mx_k[i], p_inv);
            mi_k[i] = gf_mul(mi_k[i], p_inv);
        }

        for (core::uint32 i = 0; i < ne_; ++i) {
            if (i != kr) {
                core::uint32* mx_i = &mx_[i * nd_];
                core::uint32* mi_i = &mi[i * nd_];
                core::uint32 mik = mx_i[kf];

                for (core::uint32 j = 0; j < nd_; ++j) {
                    mx_i[j] ^= gf_mul(mx_k[j], mik);
                    mi_i[j] ^= gf_mul(mi_k[j], mik);
                }
            }
        }
    }

    std::copy(mi.begin(), mi.end(), mx_.begin());
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
    // gf_exp_/gf_log_ set with their long dependent-load chain. This also
    // removes the data_log_ caching contract (same data pointer/size first
    // call with ecc_num==0); ecc_num ordering no longer matters.
    const core::uint32 coeff = mx_[ecc_num * nd_ + data_num];
    if (coeff == 0) {
        return; // x^0: product is 0 for every word, nothing to fold in
    }

    core::uint16 mul_lo[256], mul_hi[256];
    for (core::uint32 i = 0; i < 256; ++i) {
        mul_lo[i] = static_cast<core::uint16>(gf_mul(coeff, i));
        mul_hi[i] = static_cast<core::uint16>(gf_mul(coeff, i << 8));
    }

    const core::byte* d = data;
    core::byte* e_ptr = ecc;
    size_t remaining = block_size / 2;
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
        //   coeff * (hi<<8 | lo) = (lo * K0) ^ (hi * K1), K1 = coeff * x^8.
        // Each 8-bit x 16-bit-constant product is a GF(2)-linear byte map,
        // i.e. two 8x8 matrices; the calibration has measured the
        // instruction's byte/bit mapping empirically, so the kernel is
        // bit-identical to the scalar fold.
        core::uint64 m[4];
        m[0] = gfni_encode_byte_map(*this, coeff, /*high_half=*/false, cal);
        m[1] = gfni_encode_byte_map(*this, coeff, /*high_half=*/true, cal);
        const core::uint32 K1 = gf_mul(coeff, 0x100u);
        m[2] = gfni_encode_byte_map(*this, K1, /*high_half=*/false, cal);
        m[3] = gfni_encode_byte_map(*this, K1, /*high_half=*/true, cal);

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

    update_ecc_scalar(data_num, ecc_num, data, ecc, block_size);
}

} // namespace openrar::recovery
