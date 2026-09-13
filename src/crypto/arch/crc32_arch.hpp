#ifndef OPENRAR_CRYPTO_ARCH_CRC32_ARCH_HPP
#define OPENRAR_CRYPTO_ARCH_CRC32_ARCH_HPP

#include "../../core/types.hpp"
#include "../../core/cpu.hpp"

#if (defined(__aarch64__) || defined(_M_ARM64)) &&                                                 \
    (defined(__ARM_FEATURE_CRC32) || defined(_MSC_VER))
#if defined(__ARM_ACLE) || defined(_MSC_VER)
#include <arm_acle.h>
#define OPENRAR_HAS_ARM_CRC32 1
#endif
#endif

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) &&           \
    (defined(__PCLMUL__) || defined(_MSC_VER))
#define OPENRAR_HAS_X86_PCLMUL 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <emmintrin.h>
#include <smmintrin.h>
#include <wmmintrin.h>
#endif
#endif

namespace openrar::crypto::arch {

#if defined(OPENRAR_HAS_X86_PCLMUL)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__target__("sse4.2,pclmul")))
#endif
inline core::uint32 crc32_step_pclmul(core::uint32 crc, const void* data, size_t len) {
    const auto* buf = static_cast<const core::byte*>(data);
#if defined(_MSC_VER)
    __declspec(align(16)) static const core::uint64 K1K2[] = {0x0154442bd4, 0x01c6e41596};
    __declspec(align(16)) static const core::uint64 K3K4[] = {0x01751997d0, 0x00ccaa009e};
    __declspec(align(16)) static const core::uint64 K5K0[] = {0x0163cd6124, 0x0000000000};
    __declspec(align(16)) static const core::uint64 POLY[] = {0x01db710641, 0x01f7011641};
#else
    alignas(16) static const core::uint64 K1K2[] = {0x0154442bd4, 0x01c6e41596};
    alignas(16) static const core::uint64 K3K4[] = {0x01751997d0, 0x00ccaa009e};
    alignas(16) static const core::uint64 K5K0[] = {0x0163cd6124, 0x0000000000};
    alignas(16) static const core::uint64 POLY[] = {0x01db710641, 0x01f7011641};
#endif

    __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;

    x1 = _mm_loadu_si128((const __m128i*)(buf + 0x00));
    x2 = _mm_loadu_si128((const __m128i*)(buf + 0x10));
    x3 = _mm_loadu_si128((const __m128i*)(buf + 0x20));
    x4 = _mm_loadu_si128((const __m128i*)(buf + 0x30));

    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128(crc));

    x0 = _mm_load_si128((const __m128i*)K1K2);

    buf += 64;
    len -= 64;

    while (len >= 64) {
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        x8 = _mm_clmulepi64_si128(x4, x0, 0x00);

        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
        x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
        x4 = _mm_clmulepi64_si128(x4, x0, 0x11);

        y5 = _mm_loadu_si128((const __m128i*)(buf + 0x00));
        y6 = _mm_loadu_si128((const __m128i*)(buf + 0x10));
        y7 = _mm_loadu_si128((const __m128i*)(buf + 0x20));
        y8 = _mm_loadu_si128((const __m128i*)(buf + 0x30));

        x1 = _mm_xor_si128(x1, x5);
        x2 = _mm_xor_si128(x2, x6);
        x3 = _mm_xor_si128(x3, x7);
        x4 = _mm_xor_si128(x4, x8);

        x1 = _mm_xor_si128(x1, y5);
        x2 = _mm_xor_si128(x2, y6);
        x3 = _mm_xor_si128(x3, y7);
        x4 = _mm_xor_si128(x4, y8);

        buf += 64;
        len -= 64;
    }

    x0 = _mm_load_si128((const __m128i*)K3K4);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x2);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x3);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x4);
    x1 = _mm_xor_si128(x1, x5);

    while (len >= 16) {
        x2 = _mm_loadu_si128((const __m128i*)buf);
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x1 = _mm_xor_si128(x1, x2);
        x1 = _mm_xor_si128(x1, x5);
        buf += 16;
        len -= 16;
    }

    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_srli_si128(x1, 8);
    x1 = _mm_xor_si128(x1, x2);

    x0 = _mm_loadl_epi64((const __m128i*)K5K0);

    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_and_si128(x1, x3);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    x0 = _mm_load_si128((const __m128i*)POLY);

    x2 = _mm_and_si128(x1, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x10);
    x2 = _mm_and_si128(x2, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    return _mm_extract_epi32(x1, 1);
}
#endif

#if defined(OPENRAR_HAS_ARM_CRC32)
inline core::uint32 crc32_step_arm64(core::uint32 crc, const void* data, size_t size) {
    // INTENDED (double inversion): the ARMv8 CRC32 instructions expect the
    // complemented (final-form) CRC, while crc32_step's accumulator is the
    // raw pre-inversion form. The ~ on entry and on exit convert between the
    // two conventions so this path is bit-identical to the scalar one; do
    // not remove either inversion.
    const auto* p = static_cast<const core::byte*>(data);
    core::uint32 c = ~crc;

    while (size > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
        c = __crc32b(c, *p++);
        size--;
    }

    while (size >= 8) {
        core::uint64 val;
        std::memcpy(&val, p, 8);
        c = static_cast<core::uint32>(__crc32d(c, val));
        p += 8;
        size -= 8;
    }

    if (size >= 4) {
        core::uint32 val;
        std::memcpy(&val, p, 4);
        c = __crc32w(c, val);
        p += 4;
        size -= 4;
    }

    if (size >= 2) {
        core::uint16 val;
        std::memcpy(&val, p, 2);
        c = __crc32h(c, val);
        p += 2;
        size -= 2;
    }

    if (size > 0) {
        c = __crc32b(c, *p);
    }

    return ~c;
}
#endif

} // namespace openrar::crypto::arch

#endif // OPENRAR_CRYPTO_ARCH_CRC32_ARCH_HPP
