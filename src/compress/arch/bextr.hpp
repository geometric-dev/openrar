#ifndef OPENRAR_COMPRESS_ARCH_BEXTR_HPP
#define OPENRAR_COMPRESS_ARCH_BEXTR_HPP

#include "../../core/types.hpp"
#include "../../core/cpu.hpp"

#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
#include <immintrin.h>
#define OPENRAR_HAS_BMI1 1
#define OPENRAR_HAS_BMI1_64 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define OPENRAR_HAS_BMI1 1
#if defined(__x86_64__)
#define OPENRAR_HAS_BMI1_64 1
#endif
#endif

namespace openrar::compress::arch {

#if defined(OPENRAR_HAS_BMI1)

#if defined(_MSC_VER)
inline core::uint32 bextr_u32(core::uint32 val, unsigned int start, unsigned int len) noexcept {
    return _bextr_u32(val, start, len);
}
#if defined(OPENRAR_HAS_BMI1_64)
inline core::uint64 bextr_u64(core::uint64 val, unsigned int start, unsigned int len) noexcept {
    return _bextr_u64(val, start, len);
}
#endif

#elif defined(__GNUC__) || defined(__clang__)
#if defined(__BMI__)
inline core::uint32 bextr_u32(core::uint32 val, unsigned int start, unsigned int len) noexcept {
    return _bextr_u32(val, start, len);
}
#if defined(OPENRAR_HAS_BMI1_64)
inline core::uint64 bextr_u64(core::uint64 val, unsigned int start, unsigned int len) noexcept {
    return _bextr_u64(val, start, len);
}
#endif
#else
__attribute__((target("bmi"))) inline core::uint32 bextr_u32(core::uint32 val, unsigned int start,
                                                             unsigned int len) noexcept {
    return __builtin_ia32_bextr_u32(val, (start & 0xFF) | ((len & 0xFF) << 8));
}
#if defined(OPENRAR_HAS_BMI1_64)
__attribute__((target("bmi"))) inline core::uint64 bextr_u64(core::uint64 val, unsigned int start,
                                                             unsigned int len) noexcept {
    return __builtin_ia32_bextr_u64(val, static_cast<unsigned long long>(start & 0xFF) |
                                             (static_cast<unsigned long long>(len & 0xFF) << 8));
}
#endif
#endif
#endif

#endif // OPENRAR_HAS_BMI1

inline core::uint32 bextr_u32_scalar(core::uint32 val, unsigned int start,
                                     unsigned int len) noexcept {
    if (start >= 32 || len == 0) return 0;
    if (start + len > 32) len = 32 - start;
    return (len >= 32) ? val : ((val >> start) & ((core::uint32(1) << len) - 1));
}

inline core::uint64 bextr_u64_scalar(core::uint64 val, unsigned int start,
                                     unsigned int len) noexcept {
    if (start >= 64 || len == 0) return 0;
    if (start + len > 64) len = 64 - start;
    return (len >= 64) ? val : ((val >> start) & ((core::uint64(1) << len) - 1));
}

} // namespace openrar::compress::arch

#endif // OPENRAR_COMPRESS_ARCH_BEXTR_HPP
