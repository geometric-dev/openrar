#ifndef OPENRAR_RECOVERY_RS16_HPP
#define OPENRAR_RECOVERY_RS16_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <vector>

namespace openrar::recovery {

// Independent C++17 implementation of a Cauchy Reed-Solomon erasure code over
// GF(2^16) = GF(65536), field polynomial x^16 + x^12 + x^3 + x + 1 (0x1100B).
//
// Encoding is systematic: parity unit i is the Cauchy-weighted sum of all data
// units. Decoding exploits the fact that every square submatrix of a Cauchy
// matrix is invertible, so any set of missing units up to the parity count can
// be recovered by inverting a square system over just the missing units.
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

    // Apply a data sector to an ECC sector (arbitrary block sizes supported).
    // Dispatches to the GFNI kernel when compiled with it and the CPU
    // supports it (probed once against the scalar path — see
    // gfni_calib); otherwise the scalar table path runs. Both paths
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

    // True when the NEON fold kernel is compiled in. On AArch64 Advanced
    // SIMD is architecturally mandatory (and baseline on MSVC ARM64), so
    // compile-time gating is the whole dispatch — no runtime probe, and no
    // calibration: the kernel's tables are built from the same gf_mul the
    // scalar path uses.
    static bool neon_kernel_active();

    // Field operations (public for testing and validation).
    //
    // Product and inverse are read from log/antilog tables. Zero has no
    // logarithm, so it is handled before the lookup instead of being given a
    // sentinel exponent: the tables then need no oversized or duplicated
    // region and cannot silently produce a wrong product.
    constexpr core::uint32 gf_add(core::uint32 a, core::uint32 b) const { return a ^ b; }
    core::uint32 gf_mul(core::uint32 a, core::uint32 b) const {
        if (a == 0 || b == 0 || a > GF_SIZE || b > GF_SIZE) return 0;
        const core::uint32 sum = logarithm_[a] + logarithm_[b];
        return antilog_[sum >= GF_SIZE ? sum - GF_SIZE : sum];
    }
    core::uint32 gf_inv(core::uint32 a) const {
        if (a == 0 || a > GF_SIZE) return 0;
        const core::uint32 e = logarithm_[a];
        return e == 0 ? 1u : antilog_[GF_SIZE - e];
    }

    core::uint32 data_count() const { return nd_; }
    core::uint32 rec_count() const { return nr_; }
    core::uint32 erasure_count() const { return ne_; }

private:
    void init_gf();
    void fill_parity_coefficients();
    void derive_recovery_coefficients();
    bool invert_square(std::vector<core::uint32>& m, core::uint32 n) const;

    core::uint32 nd_{0}; // Number of data units
    core::uint32 nr_{0}; // Number of recovery units
    core::uint32 ne_{0}; // Number of missing/erased units

    const core::uint16* antilog_{nullptr};
    const core::uint32* logarithm_{nullptr};

    std::vector<bool> valid_flags_;
    std::vector<core::uint32> mx_; // Cauchy transformation matrix
};

} // namespace openrar::recovery

#endif // OPENRAR_RECOVERY_RS16_HPP
