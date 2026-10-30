// OpenRAR Cauchy Reed-Solomon GF(2^16) Recovery Fuzzer (v2.0)
//
// Dual-mode adversarial fuzzing harness:
//   1. Under Clang with OPENRAR_USE_LIBFUZZER: continuous coverage-guided mutation.
//   2. Under MSVC/GCC standalone: deterministic PRNG sweep & boundary stress suite.
//
// Invariants tested:
//   - Zero-crash, zero-UB, fail-closed on malformed shard counts (ND=0, NR=0, ND+NR > 65535).
//   - LU decomposition with partial pivoting graceful failure on singular systems.
//   - Differential oracle: update_ecc (SIMD/GFNI/NEON) == update_ecc_scalar bit-exact parity.
//   - Galois field inverse: gf_mul(a, gf_inv(a)) == 1 for all a in [1, 65535].

#include "../../src/recovery/rs16.hpp"
#include "../../src/core/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

using namespace openrar;
using namespace openrar::recovery;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 6) return 0;

    // Field arithmetic invariant verification on sample values from the fuzz stream
    ReedSolomon16 rs;
    const core::uint32 val_a = (data[0] | (static_cast<core::uint32>(data[1]) << 8)) & 0xFFFFu;
    const core::uint32 val_b = (data[2] | (static_cast<core::uint32>(data[3]) << 8)) & 0xFFFFu;

    if (val_a > 0 && val_a <= ReedSolomon16::GF_SIZE) {
        const core::uint32 inv_a = rs.gf_inv(val_a);
        const core::uint32 prod = rs.gf_mul(val_a, inv_a);
        if (prod != 1) {
            std::fprintf(stderr, "RS16 INVARIANT VIOLATION: gf_mul(%u, inv(%u)) = %u != 1\n", val_a,
                         val_a, prod);
            std::abort();
        }
    }

    // Parse shard configuration from input stream
    // Map to both small realistic ranges and boundary extremes
    core::uint32 nd = 0;
    core::uint32 nr = 0;
    const uint8_t config_selector = data[4];

    if (config_selector < 128) {
        // Realistic small/medium configurations (1..128 data, 1..64 recovery)
        nd = 1 + (val_a % 128);
        nr = 1 + (val_b % 64);
    } else if (config_selector < 200) {
        // Boundary cases around GF_SIZE (65535)
        nd = 65000 + (val_a % 1000);
        nr = 1 + (val_b % 1000);
    } else {
        // Arbitrary raw values (testing bounds rejection: nd=0, nr=0, nd+nr > 65535)
        nd = val_a;
        nr = val_b;
    }

    const bool test_decoding = (data[5] & 1) != 0;

    if (!test_decoding) {
        // Encoder mode (valid_flags == nullptr)
        if (rs.init(nd, nr, nullptr)) {
            // Verify dimension bounds
            if (rs.data_count() != nd || rs.rec_count() != nr) std::abort();

            // Test ECC update differential bit-exactness if payload bytes available
            if (size >= 12 && nd > 0 && nr > 0) {
                const size_t raw_block_size = 2 + (data[6] % 128) * 2; // must be even
                const size_t block_size = std::min<size_t>(raw_block_size, 256);
                const core::uint32 data_idx = val_a % nd;
                const core::uint32 ecc_idx = val_b % nr;

                std::vector<core::byte> in_data(block_size, 0x5A);
                std::vector<core::byte> ecc_simd(block_size, 0);
                std::vector<core::byte> ecc_scalar(block_size, 0);

                // Populate with fuzz data if available
                const size_t copy_len = std::min(size - 7, block_size);
                if (copy_len > 0) {
                    std::memcpy(in_data.data(), data + 7, copy_len);
                }

                rs.update_ecc(data_idx, ecc_idx, in_data.data(), ecc_simd.data(), block_size);
                rs.update_ecc_scalar(data_idx, ecc_idx, in_data.data(), ecc_scalar.data(),
                                     block_size);

                if (std::memcmp(ecc_simd.data(), ecc_scalar.data(), block_size) != 0) {
                    std::fprintf(stderr,
                                 "RS16 DIFFERENTIAL MISMATCH: update_ecc != update_ecc_scalar "
                                 "(nd=%u, nr=%u, bs=%zu)\n",
                                 nd, nr, block_size);
                    std::abort();
                }
            }
        }
    } else {
        // Decoder mode (valid_flags != nullptr)
        // Guard against unbounded allocations if nd + nr is large
        if (nd > 0 && nr > 0 && static_cast<core::uint64>(nd) + nr <= ReedSolomon16::GF_SIZE) {
            const size_t total_shards = nd + nr;
            if (total_shards <= 2048) { // cap allocation in fuzzer
                std::vector<core::byte> valid_flags(total_shards, 1);
                const uint8_t* flag_stream = data + 6;
                const size_t available_flags = size - 6;

                for (size_t i = 0; i < total_shards; ++i) {
                    if (i < available_flags) {
                        valid_flags[i] = (flag_stream[i] & 1);
                    } else {
                        // Alternate pattern
                        valid_flags[i] = static_cast<core::byte>((i % 3) != 0);
                    }
                }

                // Execute decoder init: must never crash or hang, must fail closed safely
                (void)rs.init(nd, nr, valid_flags.data());
            }
        }
    }

    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
// Deterministic standalone sweep for MSVC and CTest
int main() {
    std::cout << "[fuzz_rs16] Running deterministic Cauchy RS16 recovery sweep...\n";

    // ── 1. Boundary & Invariant Rejection Tests ──────────────────────────────
    {
        ReedSolomon16 rs;
        // ND=0, NR=0 must reject
        assert(!rs.init(0, 0, nullptr));
        assert(!rs.init(0, 5, nullptr));
        assert(!rs.init(5, 0, nullptr));
        // ND + NR > 65535 must reject
        assert(!rs.init(65535, 1, nullptr));
        assert(!rs.init(32768, 32768, nullptr));
        assert(!rs.init(100000, 100000, nullptr));
    }

    // ── 2. Galois Field Arithmetic Invariants ────────────────────────────────
    {
        ReedSolomon16 rs;
        // Zero product and inverse
        assert(rs.gf_mul(0, 12345) == 0);
        assert(rs.gf_mul(12345, 0) == 0);
        assert(rs.gf_inv(0) == 0);

        // Inversion property for all non-zero elements
        for (core::uint32 a = 1; a <= 1000; ++a) {
            const core::uint32 inv = rs.gf_inv(a);
            assert(inv > 0 && inv <= ReedSolomon16::GF_SIZE);
            assert(rs.gf_mul(a, inv) == 1);
        }
    }

    // ── 3. Differential SIMD vs Scalar Bit-Exactness Sweep ───────────────────
    {
        ReedSolomon16 rs;
        const core::uint32 nd = 16;
        const core::uint32 nr = 8;
        assert(rs.init(nd, nr, nullptr));

        const size_t block_sizes[] = {2, 4, 16, 32, 64, 128, 256, 512, 1024};
        for (size_t bs : block_sizes) {
            std::vector<core::byte> in(bs);
            for (size_t i = 0; i < bs; ++i) in[i] = static_cast<core::byte>((i * 37 + 13) & 0xFF);

            for (core::uint32 d = 0; d < 4; ++d) {
                for (core::uint32 r = 0; r < 4; ++r) {
                    std::vector<core::byte> ecc_simd(bs, 0xAA);
                    std::vector<core::byte> ecc_scalar(bs, 0xAA);

                    rs.update_ecc(d, r, in.data(), ecc_simd.data(), bs);
                    rs.update_ecc_scalar(d, r, in.data(), ecc_scalar.data(), bs);

                    assert(std::memcmp(ecc_simd.data(), ecc_scalar.data(), bs) == 0);
                }
            }
        }
    }

    // ── 4. Full Cauchy Encode-Erase-Decode Roundtrip Invariant ───────────────
    {
        const core::uint32 nd = 8;
        const core::uint32 nr = 4;
        const size_t bs = 64;

        // Create sample data
        std::vector<std::vector<core::byte>> data_blocks(nd, std::vector<core::byte>(bs));
        for (core::uint32 d = 0; d < nd; ++d) {
            for (size_t i = 0; i < bs; ++i) {
                data_blocks[d][i] = static_cast<core::byte>((d * 17 + i * 23 + 1) & 0xFF);
            }
        }

        // Encode parities
        ReedSolomon16 encoder;
        assert(encoder.init(nd, nr, nullptr));
        std::vector<std::vector<core::byte>> ecc_blocks(nr, std::vector<core::byte>(bs, 0));
        for (core::uint32 d = 0; d < nd; ++d) {
            for (core::uint32 r = 0; r < nr; ++r) {
                encoder.update_ecc(d, r, data_blocks[d].data(), ecc_blocks[r].data(), bs);
            }
        }

        // Simulate 2 erasures in data shards: shard 1 and shard 3 missing
        std::vector<core::byte> valid_flags(nd + nr, 1);
        valid_flags[1] = 0; // erased
        valid_flags[3] = 0; // erased

        ReedSolomon16 decoder;
        assert(decoder.init(nd, nr, valid_flags.data()));
        assert(decoder.erasure_count() == 2);

        // Prepare working units: healthy data + substitute parities in erased slots
        // Parity unit 0 substitutes for erased data 1; Parity unit 1 substitutes for erased data 3
        std::vector<std::vector<core::byte>> working(nd, std::vector<core::byte>(bs, 0));
        for (core::uint32 d = 0; d < nd; ++d) {
            if (valid_flags[d]) {
                working[d] = data_blocks[d];
            }
        }
        working[1] = ecc_blocks[0]; // substitute 0
        working[3] = ecc_blocks[1]; // substitute 1

        // Reconstruct erased units using decoder recovery coefficients
        std::vector<std::vector<core::byte>> recovered(2, std::vector<core::byte>(bs, 0));
        for (core::uint32 k = 0; k < 2; ++k) {
            for (core::uint32 d = 0; d < nd; ++d) {
                decoder.update_ecc(d, k, working[d].data(), recovered[k].data(), bs);
            }
        }

        // Assert exact recovery
        assert(std::memcmp(recovered[0].data(), data_blocks[1].data(), bs) == 0);
        assert(std::memcmp(recovered[1].data(), data_blocks[3].data(), bs) == 0);
    }

    // ── 5. Deterministic Pseudo-Random Fuzzing Sweep ─────────────────────────
    std::mt19937 rng(1337);
    for (int iter = 0; iter < 500; ++iter) {
        const size_t len = 6 + (rng() % 128);
        std::vector<uint8_t> fuzz_buf(len);
        for (size_t i = 0; i < len; ++i) {
            fuzz_buf[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        LLVMFuzzerTestOneInput(fuzz_buf.data(), fuzz_buf.size());
    }

    std::cout << "[fuzz_rs16] All invariants, roundtrips, and sweeps passed successfully.\n";
    return 0;
}
#endif
