#include "rs16.hpp"
#include <algorithm>
#include <cstring>

namespace openrar::recovery {

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

void ReedSolomon16::update_ecc(core::uint32 data_num, core::uint32 ecc_num, const core::byte* data,
                               core::byte* ecc, size_t block_size) {
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

} // namespace openrar::recovery
