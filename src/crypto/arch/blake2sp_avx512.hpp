#ifndef OPENRAR_CRYPTO_ARCH_BLAKE2SP_AVX512_HPP
#define OPENRAR_CRYPTO_ARCH_BLAKE2SP_AVX512_HPP

#include "../../core/types.hpp"
#include <cstddef>

#if defined(OPENRAR_HAS_AVX512_KERNEL)

namespace openrar::crypto::arch {

// 8-way AVX-512VL bulk leaf update using 32 EVEX vector registers (zero spills)
// and native 32-bit vector rotation instructions (_mm256_ror_epi32).
// Inputs:
//   h: 8 uint32[8] state vectors for leaves 0..7
//   t0, t1: shared byte counters
//   pending0, pending1: 8 pointers each to pending leaf buffers
//   in: base pointer to `chunks` whole 512-byte chunks
//   chunks: number of 512-byte chunks (>= 2)
void blake2sp_update_bulk_avx512(core::uint32 h[8][8], core::uint32& t0, core::uint32& t1,
                                 const core::byte* const pending0[8],
                                 const core::byte* const pending1[8], const core::byte* in,
                                 size_t chunks) noexcept;

} // namespace openrar::crypto::arch

#endif // OPENRAR_HAS_AVX512_KERNEL

#endif // OPENRAR_CRYPTO_ARCH_BLAKE2SP_AVX512_HPP
