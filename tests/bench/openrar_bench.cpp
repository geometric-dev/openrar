// openrar_bench — kernel throughput report for the v1.22.0 SIMD arc, plus
// the v1.25.0 mapped-listing benchmark.
//
// Measures, on whatever the running host supports:
//   1. RS16 Cauchy parity fold (the .rev recovery hot loop): scalar table
//      fold vs the dispatched kernel (NEON on AArch64, GFNI on GFNI-capable
//      x86, scalar otherwise). This is the "5-10x" roadmap claim.
//   2. Match-length comparison: scalar vs the SSE2/AVX2/NEON kernels the
//      host supports.
//   3. v1.25: 50 GB-class archive listing — mapped scan engine vs the
//      buffered engine (sparse corpus; the engine pins volume size at
//      open). Informational, never a gate.
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
//   [bench] listing_50gb mapped        = ...
// Exit code is always 0: this report is informational, never a gate.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../src/archive/archive_reader.hpp"
#include "../src/compress/arch/match_simd.hpp"
#include "../src/core/cpu.hpp"
#include "../src/format/header_writer.hpp"
#include "../src/io/file_stream.hpp"
#include "../src/io/mapped_file.hpp"
#include "../src/recovery/rs16.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
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
} // namespace

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

// ── v1.25: 50 GB-class listing benchmark (mapped vs buffered scan) ──────────
// Builds a SPARSE ~50 GB archive (100k stored entries x 500 KiB payload
// holes — hole bytes are never allocated) and lists it with the mapped scan
// engine vs the buffered engine. Per the design review: the sparse archive
// measures mapping cost itself; the 100k-header chain is the header-dense
// case where header reads dominate. Informational, never a gate.
static void bench_listing_50gb() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "openrar_bench_listing";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    constexpr size_t kEntries = 100000;
    constexpr size_t kHole = 500 * 1024; // payload hole per entry
    // Windows: mark the file sparse BEFORE writing anything, via a dedicated
    // handle (FSCTL_SET_SPARSE = CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 35,
    // METHOD_BUFFERED, FILE_ANY_ACCESS); the value is the documented ABI
    // constant — winioctl.h is excluded by WIN32_LEAN_AND_MEAN). If sparse
    // is unsupported, skip: NTFS would otherwise allocate every 500 KiB gap.
    bool sparse_ok = true;
#ifdef _WIN32
    {
        HANDLE h = CreateFileW((dir / "sparse_50g.rar").wstring().c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("[bench] listing_50gb SKIPPED (cannot create file)\n");
            fs::remove_all(dir, ec);
            return;
        }
        constexpr DWORD kFsctlSetSparse = 0x0009008Cu;
        DWORD unused = 0;
        sparse_ok =
            DeviceIoControl(h, kFsctlSetSparse, nullptr, 0, nullptr, 0, &unused, nullptr) != FALSE;
        if (!sparse_ok) std::printf("[bench] listing_50gb FSCTL failed gle=%lu\n", GetLastError());
        CloseHandle(h);
    }
    if (!sparse_ok) {
        std::printf("[bench] listing_50gb SKIPPED (sparse unsupported, corpus too large)\n");
        fs::remove_all(dir, ec);
        return;
    }
#endif
    const fs::path arc = dir / "sparse_50g.rar";
    {
        io::FileStream out;
        if (!out.open(arc, io::FileMode::CreateAlways)) {
            std::printf("[bench] listing_50gb SKIPPED (cannot create file)\n");
            fs::remove_all(dir, ec);
            return;
        }
        format::HeaderWriter::write_signature(out);
        format::MainBlock mb;
        format::HeaderWriter::write_main_block(out, mb);
        std::string name;
        for (size_t i = 0; i < kEntries; ++i) {
            format::FileBlock fb;
            name = "e" + std::to_string(i) + ".bin";
            fb.file_name = name;
            fb.host_os = 1;
            fb.attributes = 0100644u;
            fb.unp_size = kHole - 300; // header ~300 bytes precedes the hole
            fb.pack_size = static_cast<core::int64>(fb.unp_size);
            fb.method = 0;
            fb.win_size = 0;
            if (!format::HeaderWriter::write_file_block(out, fb)) {
                std::printf("[bench] listing_50gb SKIPPED (header write failed)\n");
                fs::remove_all(dir, ec);
                return;
            }
            // Leave the payload as a hole: jump past it; the ENDARC lands at
            // the file end.
            out.seek(static_cast<core::int64>(fb.pack_size), io::SeekOrigin::Current);
        }
        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb);
    }

    auto run = [&](bool mapped) -> double {
        archive::ArchiveReader reader;
        reader.set_use_mapped_scan(mapped);
        int status = 0;
        std::string detail;
        const auto t0 = std::chrono::steady_clock::now();
        if (!reader.open_ex(arc.string(), "", status, detail)) return -1.0;
        const auto t1 = std::chrono::steady_clock::now();
        const size_t n = reader.entries().size();
        reader.close();
        if (n != kEntries) return -2.0;
        return std::chrono::duration<double>(t1 - t0).count();
    };

    const double mapped_s = run(true);
    const double buffered_s = run(false);
    const double gib = static_cast<double>(kEntries) * kHole / (1024.0 * 1024.0 * 1024.0);
    std::printf("[bench] listing_50gb sparse=%.1f GiB headers=%zu\n", gib, kEntries);
    if (mapped_s > 0)
        std::printf("[bench] listing_50gb mapped      = %8.3f s\n", mapped_s);
    else
        std::printf("[bench] listing_50gb mapped      = FAILED (%.1f)\n", mapped_s);
    if (buffered_s > 0)
        std::printf("[bench] listing_50gb buffered    = %8.3f s   (ratio %.2fx)\n", buffered_s,
                    buffered_s / mapped_s);
    else
        std::printf("[bench] listing_50gb buffered    = FAILED (%.1f)\n", buffered_s);

    fs::remove_all(dir, ec);
}

int main() {
    std::printf("[bench] openrar kernel throughput report\n");
    std::printf("[bench] emulated hosts (Intel SDE): ratios are non-normative\n");
    bench_rs16();
    bench_match_length();
    bench_listing_50gb();
    std::printf("[bench] sink=%llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
