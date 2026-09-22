#include "cpu.hpp"
#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86) || defined(__x86_64__) ||             \
    defined(__i386__)
// x86-only: MSVC's immintrin.h #errors on ARM targets.
#include <immintrin.h>
#endif
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#if defined(__aarch64__) || defined(__arm__)
#include <asm/hwcap.h>
#endif
#endif

namespace openrar::core {

namespace {

CpuFeatures detect_cpu_features() {
    CpuFeatures f{};
#if defined(__EMSCRIPTEN__)
    // WASM: no CPUID / hardware detection; return baseline (scalar paths)
    return f;
#endif

    // Testing / diagnostics override: force every dispatch onto its scalar
    // path (and flip the CLI banner to the amber warning) so accelerated vs
    // baseline runs are comparable on the same machine. Read once, cached
    // with the rest of the feature set.
    if (getenv("OPENRAR_DISABLE_CPU_EXT") != nullptr) {
        return f;
    }

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86 / x86-64 detection via CPUID
    int info[4] = {0};

    auto cpuid_call = [](int leaf, int subleaf, int regs[4]) {
#if defined(_MSC_VER)
        __cpuidex(regs, leaf, subleaf);
#elif defined(__GNUC__) || defined(__clang__)
        __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#else
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
#endif
    };

    // XCR0 read for the OS-state check below. AVX-class code needs the OS to
    // have enabled YMM save/restore; there is no portable intrinsic outside
    // these two toolchains, so unknown compilers keep avx2 off.
#if defined(_MSC_VER)
    auto read_xcr0 = [](unsigned int ctr) -> unsigned long long {
        return _xgetbv(ctr);
    };
#elif defined(__GNUC__) || defined(__clang__)
    auto read_xcr0 = [](unsigned int ctr) -> unsigned long long {
        unsigned int eax = 0, edx = 0;
        __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(ctr));
        return (static_cast<unsigned long long>(edx) << 32) | eax;
    };
#endif

    // Leaf 0: max basic leaf
    cpuid_call(0, 0, info);
    int max_leaf = info[0];

    int ecx_leaf1 = 0;
    if (max_leaf >= 1) {
        cpuid_call(1, 0, info);
        // info[2] is ECX, info[3] is EDX
        ecx_leaf1 = info[2];
        int ecx = info[2];
        int edx = info[3];

        f.sse2 = (edx & (1 << 26)) != 0;
        f.ssse3 = (ecx & (1 << 9)) != 0;
        f.pclmulqdq = (ecx & (1 << 1)) != 0;
        f.aes_ni = (ecx & (1 << 25)) != 0;
    }

    if (max_leaf >= 7) {
        cpuid_call(7, 0, info);
        // info[1] is EBX, info[2] is ECX
        int ebx = info[1];
        int ecx = info[2];

        // CPUID advertises CPU capability only: executing AVX-class code
        // additionally requires the OS to manage vector state. AVX2-class
        // (YMM) needs OSXSAVE + XCR0 bits 2:1; AVX-512 additionally needs
        // XCR0 bits 7:5 (opmask, ZMM_Hi256, Hi16_ZMM). Dispatching on CPUID
        // alone made the process die with #UD on OSes/hypervisors that expose
        // the CPU feature without enabling XSAVE (report M4). SHA-NI is
        // SSE-class and needs no XSTATE, so it stays CPUID-only.
        bool os_ymm = false;
        bool os_zmm = false;
        if ((ecx_leaf1 & (1 << 27)) != 0) {
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
            unsigned long long xcr0 = read_xcr0(0);
            os_ymm = (xcr0 & 0x6) == 0x6;
            os_zmm = os_ymm && (xcr0 & 0xE0) == 0xE0;
#endif
        }

        f.avx2 = os_ymm && (ebx & (1 << 5)) != 0;
        f.sha_ni = (ebx & (1 << 29)) != 0;
        f.avx512f = os_zmm && (ebx & (1 << 16)) != 0;
        // GFNI (leaf 7 ECX bit 8) is required in its 512-bit form for the
        // RS16 parity kernel, so it is gated on ZMM OS state like AVX-512F
        // (v1.22.0 groundwork). The 128/256-bit GFNI forms (AVX2-class /
        // SSE-class) remain unused.
        f.gfni = f.avx512f && (ecx & (1 << 8)) != 0;
        // VAES / VPCLMULQDQ exist in 128/256-bit (AVX2-class) and 512-bit
        // forms; we only commit to the AVX2-class forms here, so the OS
        // requirement is YMM state, not ZMM.
        f.vaes = f.avx2 && (ecx & (1 << 9)) != 0;
        f.vpclmulqdq = f.avx2 && (ecx & (1 << 10)) != 0;
    }

#elif defined(__aarch64__) || defined(_M_ARM64)
    // 64-bit ARM always has NEON
    f.neon = true;

#if defined(_WIN32)
#if defined(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)
    f.arm_crc32 = IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) != 0;
#endif
#if defined(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)
    bool crypto = IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0;
    f.arm_aes = crypto;
    f.arm_sha2 = crypto;
    f.arm_pmull = crypto;
#endif

#elif defined(__APPLE__)
    auto check_sysctl = [](const char* name) -> bool {
        int val = 0;
        size_t len = sizeof(val);
        return sysctlbyname(name, &val, &len, nullptr, 0) == 0 && val != 0;
    };
    f.arm_crc32 = check_sysctl("hw.optional.armv8_crc32");
    f.arm_aes = check_sysctl("hw.optional.arm.FEAT_AES");
    f.arm_sha2 = check_sysctl("hw.optional.arm.FEAT_SHA256");
    f.arm_pmull = check_sysctl("hw.optional.arm.FEAT_PMULL");

#elif defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
#if defined(HWCAP_CRC32)
    f.arm_crc32 = (hwcap & HWCAP_CRC32) != 0;
#endif
#if defined(HWCAP_AES)
    f.arm_aes = (hwcap & HWCAP_AES) != 0;
#endif
#if defined(HWCAP_SHA2)
    f.arm_sha2 = (hwcap & HWCAP_SHA2) != 0;
#endif
#if defined(HWCAP_PMULL)
    f.arm_pmull = (hwcap & HWCAP_PMULL) != 0;
#endif
#endif

#elif defined(__arm__) || defined(_M_ARM)
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    f.neon = true;
#endif
#endif

    return f;
}

} // namespace

const CpuFeatures& get_cpu_features() {
    static const CpuFeatures features = detect_cpu_features();
    return features;
}

AccelerationReport describe_acceleration() {
    AccelerationReport r;

#if defined(__EMSCRIPTEN__)
    // WASM baseline build: every kernel runs scalar. (The pthread/SIMD wasm
    // configuration would report through the same tags once wired.)
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // Mirrors match_simd.hpp dispatch. MSVC 32-bit has no 64-bit scan
    // intrinsics, so match_simd keeps x86-64-only SIMD there (L14).
    const CpuFeatures& f = get_cpu_features();
#if defined(_MSC_VER) && !defined(_M_X64)
    constexpr bool match_simd_x86 = false;
#else
    constexpr bool match_simd_x86 = true;
#endif
    if (match_simd_x86) {
#if defined(OPENRAR_HAS_AVX512_KERNEL)
        if (f.avx512f)
            r.tags.push_back("AVX512 LZ");
        else
#endif
            if (f.avx2)
            r.tags.push_back("AVX2 LZ");
        else if (f.sse2)
            r.tags.push_back("SSE2 LZ");
    }
    if (f.aes_ni) r.tags.push_back("AES-NI");
    if (f.sha_ni) r.tags.push_back("SHA-NI");
    if (f.pclmulqdq) r.tags.push_back("PCLMUL CRC");
#elif defined(__aarch64__) || defined(_M_ARM64)
    const CpuFeatures& f = get_cpu_features();
    if (f.neon) r.tags.push_back("NEON LZ");
    if (f.arm_aes) r.tags.push_back("AES");
    if (f.arm_sha2) r.tags.push_back("SHA");
    if (f.arm_crc32) r.tags.push_back("CRC");
#elif defined(__arm__) || defined(_M_ARM)
    // ARM32 (Raspberry Pi armhf): every kernel runs scalar — the SIMD match
    // and RS16 fold kernels target AArch64 only.
    r.tags.push_back("scalar");
#endif

    return r;
}

} // namespace openrar::core
