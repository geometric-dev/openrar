// openrar_bench — kernel throughput report for the v1.22.0 SIMD arc.
//
// Measures, on whatever the running host supports:
//   1. RS16 Cauchy parity fold (the .rev recovery hot loop): scalar table
//      fold vs the dispatched kernel (NEON on AArch64, GFNI on GFNI-capable
//      x86, scalar otherwise). This is the "5-10x" roadmap claim.
//   2. Match-length comparison: scalar vs the SSE2/AVX2/NEON kernels the
//      host supports.
//
// Methodology: deterministic corpus, 3 passes, best-of-3 (shared runners
// and SDE are noisy; best-of damps scheduler spikes). GFNI/AVX-512 numbers
// printed under Intel SDE are EMULATED — relative-to-scalar ratios under
// SDE are NOT native performance and are labeled as such; only native runs
// (CI macOS arm64 for NEON, any AVX2 x86 runner for AVX2) produce
// normative numbers.
//
// Output contract (grep-able, one measurement per line):
//   [bench] rs16_fold scalar           = 1234.5 MiB/s
//   [bench] rs16_fold dispatched(NEON) = 5678.9 MiB/s   (ratio 4.60x)
//   [bench] match_length scalar        = ...
// Exit code is always 0: this report is informational, never a gate.

#include "../src/compress/arch/match_simd.hpp"
#include "../src/core/cpu.hpp"
#include "../src/recovery/rs16.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

namespace {

constexpr size_t kBlock = 64u * 1024; // per-update block size
constexpr size_t kUpdates = 256;      // blocks per pass (16 MiB folded)
constexpr size_t kMatchBuf = 64u * 1024;
constexpr size_t kMatchDist = 4096;
constexpr size_t kMatchCap = 4096;
constexpr size_t kMatchIters = 8192;

template <typename Fn> static double best_of_3(Fn&& fn) {
    double best = 0.0;
    for (int pass = 0; pass < 3; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        if (sec > 0) {
            const double mib_s = static_cast<double>(kUpdates) * static_cast<double>(kBlock) / sec /
                                 (1024.0 * 1024.0);
            best = std::max(best, mib_s);
        }
    }
    return best;
}

static void print(const std::string& name, double mib_s, double ref = 0.0) {
    if (ref > 0.0) {
        std::printf("[bench] %-30s = %10.1f MiB/s   (ratio %.2fx)\n", name.c_str(), mib_s,
                    mib_s / ref);
    } else {
        std::printf("[bench] %-30s = %10.1f MiB/s\n", name.c_str(), mib_s);
    }
}

static void bench_rs16() {
    recovery::ReedSolomon16 rs;
    if (!rs.init(4, 2)) return;

    std::mt19937 rng(0xBEEF);
    std::vector<core::byte> data(kBlock);
    for (auto& b : data) b = static_cast<core::byte>(rng());
    std::vector<core::byte> ecc(kBlock, core::byte(0x3C));

    const double scalar = best_of_3([&] {
        for (size_t i = 0; i < kUpdates; ++i)
            rs.update_ecc_scalar(i % 4, 0, data.data(), ecc.data(), kBlock);
    });
    print("rs16_fold scalar", scalar);

    // Dispatched path: NEON on AArch64, GFNI on GFNI-capable x86, scalar
    // otherwise (then the ratio is 1.00x by construction — the host has no
    // kernel and the report says so honestly).
    std::string kernel = "scalar (none)";
#if defined(__aarch64__) || defined(_M_ARM64)
    kernel = "NEON";
#elif defined(OPENRAR_HAS_GFNI_KERNEL)
    if (recovery::ReedSolomon16::gfni_kernel_active()) kernel = "GFNI";
#endif
    const double dispatched = best_of_3([&] {
        for (size_t i = 0; i < kUpdates; ++i)
            rs.update_ecc(i % 4, 0, data.data(), ecc.data(), kBlock);
    });
    print("rs16_fold dispatched(" + kernel + ")", dispatched, scalar);
}

static uint64_t g_sink = 0; // defeats dead-code elimination across measurements

static void bench_match_length() {
    const auto& cpu = core::get_cpu_features();
    std::vector<core::byte> buf(kMatchBuf);
    // Periodic with period kMatchDist: every compare runs the full cap, so
    // this measures kernel THROUGHPUT, not mismatch early-exit.
    for (size_t i = 0; i < kMatchBuf; ++i)
        buf[i] = static_cast<core::byte>((i % kMatchDist) ^ 0x5A);

    auto measure = [&](arch::MatchFn fn) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t sink = 0;
        for (size_t i = 0; i < kMatchIters; ++i) {
            const size_t p = (i * 64) % (kMatchBuf - kMatchCap - kMatchDist);
            sink += fn(buf.data() + p, buf.data() + p + kMatchDist, kMatchCap);
        }
        const auto t1 = std::chrono::steady_clock::now();
        g_sink += sink;
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        return sec > 0 ? static_cast<double>(kMatchIters) * static_cast<double>(kMatchCap) / sec /
                             (1024.0 * 1024.0)
                       : 0.0;
    };

    const double scalar = measure(arch::match_length_scalar);
    print("match_length scalar", scalar);

#if defined(OPENRAR_HAS_X86_SIMD)
    if (cpu.sse2) print("match_length SSE2", measure(arch::match_length_sse2), scalar);
    if (cpu.avx2) print("match_length AVX2", measure(arch::match_length_avx2), scalar);
#endif
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)
    if (cpu.neon) print("match_length NEON", measure(arch::match_length_neon), scalar);
#endif
}

} // namespace

int main() {
    std::printf("[bench] openrar kernel throughput report\n");
    std::printf("[bench] emulated hosts (Intel SDE): ratios are non-normative\n");
    bench_rs16();
    bench_match_length();
    std::printf("[bench] sink=%llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
