#ifndef OPENRAR_CORE_CPU_HPP
#define OPENRAR_CORE_CPU_HPP

#include <vector>

namespace openrar::core {

struct CpuFeatures {
    // x86 / x86-64 features. Consumed today: sse2 + avx2 (match_simd),
    // aes_ni (aes256), sha_ni (sha256), pclmulqdq (crc32 + rs16 parity).
    // avx512f/vaes/vpclmulqdq are detected for planned 256/512-bit kernels;
    // like avx2 they require the OS to have enabled the relevant XSTATE.
    bool sse2{false};
    bool ssse3{false};
    bool avx2{false};
    bool aes_ni{false};
    bool pclmulqdq{false};
    bool sha_ni{false};
    bool avx512f{false};
    bool gfni{false}; // GFNI (leaf 7 ECX bit 8), gated on ZMM OS state
    bool vaes{false};
    bool vpclmulqdq{false};

    // ARM / ARM64 features
    bool neon{false};
    bool arm_crc32{false};
    bool arm_aes{false};
    bool arm_sha2{false};
    bool arm_pmull{false};
};

// Returns a cached reference to detected CPU features.
const CpuFeatures& get_cpu_features();

struct AccelerationReport {
    // Human-readable tags for the accelerated kernels actually in use on
    // this build/platform, in display order (e.g. "AES-NI", "AVX2 LZ").
    // Empty vector means every path runs scalar (WASM baseline, ancient CPU).
    std::vector<const char*> tags;
};

// Summarizes which accelerated paths the dispatchers select on this machine.
// Mirrors the runtime dispatch rules (match_simd.hpp, aes256, sha256, crc32)
// so the CLI banner cannot drift from what the code really executes.
AccelerationReport describe_acceleration();

} // namespace openrar::core

#endif // OPENRAR_CORE_CPU_HPP
