#ifndef OPENRAR_RECOVERY_RS16_HPP
#define OPENRAR_RECOVERY_RS16_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <vector>

namespace openrar::recovery {

// Clean-room C++17 Cauchy Reed-Solomon Codec over GF(2^16) = GF(65536)
// Irreducible polynomial: x^16 + x^12 + x^3 + x + 1 (0x1100B)
class ReedSolomon16 {
public:
    static constexpr core::uint32 GF_SIZE = 65535;

    ReedSolomon16();
    ~ReedSolomon16() = default;

    // Move-only
    ReedSolomon16(ReedSolomon16&&) noexcept = default;
    ReedSolomon16& operator=(ReedSolomon16&&) noexcept = default;

    ReedSolomon16(const ReedSolomon16&) = delete;
    ReedSolomon16& operator=(const ReedSolomon16&) = delete;

    // Initialize encoder (valid_flags == nullptr) or decoder (valid_flags != nullptr)
    bool init(core::uint32 data_count, core::uint32 rec_count,
              const core::byte* valid_flags = nullptr);

    // Apply a data sector to an ECC sector (block_size must be even).
    // Dispatches to the GFNI kernel when compiled with it and the CPU
    // supports it (probed once against the scalar path — see
    // build_gfni_affines); otherwise the scalar table path runs. Both paths
    // are bit-identical (test_rs16_gfni_bit_exactness).
    void update_ecc(core::uint32 data_num, core::uint32 ecc_num, const core::byte* data,
                    core::byte* ecc, size_t block_size);

    // Scalar reference path (public for the bit-exactness gate: the test
    // compares this against update_ecc's dispatched GFNI path directly).
    void update_ecc_scalar(core::uint32 data_num, core::uint32 ecc_num, const core::byte* data,
                           core::byte* ecc, size_t block_size);

    // True when this process will actually use the GFNI fold kernel
    // (compiled in, CPU-supported, and convention-probe validated).
    static bool gfni_kernel_active();

    // Field operations (public for testing and validation)
    core::uint32 gf_add(core::uint32 a, core::uint32 b) const { return a ^ b; }
    core::uint32 gf_mul(core::uint32 a, core::uint32 b) const {
        return gf_exp_[gf_log_[a] + gf_log_[b]];
    }
    core::uint32 gf_inv(core::uint32 a) const { return a == 0 ? 0 : gf_exp_[GF_SIZE - gf_log_[a]]; }

    core::uint32 data_count() const { return nd_; }
    core::uint32 rec_count() const { return nr_; }
    core::uint32 erasure_count() const { return ne_; }

private:
    void init_gf();
    void make_encoder_matrix();
    void make_decoder_matrix();
    void invert_decoder_matrix();

    core::uint32 nd_{0}; // Number of data units
    core::uint32 nr_{0}; // Number of recovery units
    core::uint32 ne_{0}; // Number of missing/erased units
    bool decoding_{false};

    std::vector<core::uint32> gf_exp_;
    std::vector<core::uint32> gf_log_;

    std::vector<bool> valid_flags_;
    std::vector<core::uint32> mx_; // Cauchy transformation matrix
};

using RSCoder16 = ReedSolomon16;

} // namespace openrar::recovery

#endif // OPENRAR_RECOVERY_RS16_HPP
