// openrar_bench — v1.28 benchmark engine (protocol + --json for CI trends).
//
// Suites (each reports one value per pass; the protocol collapses passes):
//   1. rs16_fold_scalar / rs16_fold_dispatched  — the .rev recovery hot loop
//      (scalar table fold vs the dispatched NEON/GFNI kernel; dispatched
//      falls back to scalar where the host has no kernel, ratio 1.00x).
//   2. match_length scalar / SSE2 / AVX2 / NEON — the host-supported kernels.
//   3. listing_50gb mapped / buffered — v1.25 sparse-archive listing (kind
//      "io"; the corpus build IS the warm-up per the plan; excluded from the
//      <5% variance claim — disk cache state dominates).
//   4. extract_throughput_store / _m3, add_throughput_m3 — v1.28 (kind "io":
//      temp-disk driven).
//   5. cdc_fingerprint — v1.28 chunker throughput on the engineered corpus.
//   6. cdc_three_number_store / _plain_solid / _cdc_packed — v1.28 rollover:
//      the three-number reduction gate (archive BYTES on the engineered
//      corpus) as a bench suite; deterministic by construction, so the
//      spread should be 0.
//
// Measurement protocol (docs/v1.28-implementation-plan.md §1.6):
//   1 warm-up pass (untimed) + N timed passes (7 default, 3 with --quick);
//   result = MEDIAN; spread = (max-min)/median over the timed passes.
//   The "<5% variance" claim is scoped to COMPUTE suites on a quiescent
//   host; io suites report their spread honestly and are excluded.
//   GFNI/AVX-512 numbers under Intel SDE are EMULATED and non-normative.
//
// Output contract:
//   default: grep-able "[bench]" lines on stdout, one measurement per line;
//   --json: stdout carries ONLY the JSON document (schema_version 1, host +
//   protocol + suites), human lines move to stderr (the --json-summary
//   purity pattern; CI consumers of [bench] lines run WITHOUT the flag).
//   --strict: exit 1 when a compute suite's spread exceeds the threshold
//   (--spread-threshold-pct, default 5.0) — opt-in for quiescent CI hosts;
//   otherwise this tool is informational and always exits 0.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/compress/arch/match_simd.hpp"
#include "../../src/compress/cdc_chunker.hpp"
#include "../../src/compress/cdc_planner.hpp"
#include "../../src/compress/compress_plan.hpp"
#include "../../src/compress/solid_packer.hpp"
#include "../../src/core/cpu.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/mapped_file.hpp"
#include "../../src/recovery/rs16.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

using namespace openrar;
using namespace openrar::compress;

namespace {

constexpr size_t kBlock = 64u * 1024; // per-update block size (rs16)
constexpr size_t kUpdates = 256;      // blocks per pass (16 MiB folded)
constexpr size_t kMatchBuf = 64u * 1024;
constexpr size_t kMatchDist = 4096;
constexpr size_t kMatchCap = 4096;
constexpr size_t kMatchIters = 8192;
constexpr size_t kExtractCorpusBytes = 64u * 1024 * 1024;   // store corpus
constexpr size_t kExtractCorpusM3Bytes = 32u * 1024 * 1024; // m3 corpus
constexpr size_t kCdcFiles = 8;
constexpr size_t kCdcFileBytes = 2u * 1024 * 1024;

uint64_t g_sink = 0; // defeats dead-code elimination across measurements

std::filesystem::path bench_dir() {
    return std::filesystem::temp_directory_path() / "openrar_bench_v2";
}

// Deterministic LCG byte generator (no <random> distribution variance).
core::byte lcg_byte(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<core::byte>((state >> 24) & 0xFFu);
}

// Builds (once) the corpora + packed archives the suites measure against.
// Everything is deterministic; failures are fatal for the tool run.
struct BenchCorpora {
    bool built = false;
    std::filesystem::path dir;
    std::vector<core::byte> rs16_data;            // 64 KiB noise block
    std::vector<core::byte> match_buf;            // periodic match buffer
    std::filesystem::path store_arc;              // 64 MiB stored, 1 entry
    std::filesystem::path m3_arc;                 // 32 MiB m3, 1 entry
    std::filesystem::path m3_src;                 // the m3 corpus file
    uint64_t m3_unp = 0;                          // m3 corpus unp size
    std::vector<std::filesystem::path> cdc_files; // 8 x 2 MiB engineered
    bool listing_ready = false;
    std::filesystem::path listing_arc; // ~50 GiB sparse

    void build() {
        if (built) return;
        namespace fs = std::filesystem;
        dir = bench_dir();
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        // rs16 noise block
        rs16_data.resize(kBlock);
        uint32_t st = 0xBEEF;
        for (auto& b : rs16_data) b = lcg_byte(st);

        // periodic match buffer: every compare runs the full cap
        match_buf.resize(kMatchBuf);
        for (size_t i = 0; i < kMatchBuf; ++i)
            match_buf[i] = static_cast<core::byte>((i % kMatchDist) ^ 0x5A);

        // extract/store corpus: one 64 MiB mixed file (half compressible
        // phrase blocks, half noise), stored into a raw archive.
        const fs::path store_src = dir / "store_corpus.bin";
        {
            std::ofstream f(store_src, std::ios::binary);
            uint32_t a = 12345, b = 777;
            std::string phrase = "OpenRAR benchmark corpus block ";
            for (size_t off = 0; off < kExtractCorpusBytes;) {
                if ((off / 4096) % 2 == 0) {
                    for (int k = 0; k < 512 && off < kExtractCorpusBytes; ++k) {
                        f.write(phrase.data(), std::streamsize(phrase.size() - 1));
                        f.put(static_cast<char>('0' + (a++ % 10)));
                        off += phrase.size();
                    }
                } else {
                    for (int k = 0; k < 4096 && off < kExtractCorpusBytes; ++k) {
                        f.put(static_cast<char>(lcg_byte(b)));
                        ++off;
                    }
                }
            }
        }
        store_arc = dir / "store_corpus.rar";
        raw_store_archive(store_arc, store_src, "store_corpus.bin");

        // m3 corpus: 32 MiB compressible text-ish file, packed by the mutator.
        m3_src = dir / "m3_corpus.txt";
        {
            std::ofstream f(m3_src, std::ios::binary);
            uint32_t a = 4242;
            std::string line = "the quick brown fox jumps over the lazy dog 0123456789 ";
            for (size_t off = 0; off < kExtractCorpusM3Bytes; off += line.size() - 1) {
                line[4] = static_cast<char>('a' + (a++ % 26)); // tiny variation
                f.write(line.data(), std::streamsize(line.size() - 1));
            }
        }
        m3_arc = dir / "m3_corpus.rar";
        if (!archive::ArchiveMutator::add_file_to_archive(m3_arc, m3_src, "m3_corpus.txt", 3)) {
            std::fprintf(stderr, "[bench] FATAL: cannot pack m3 corpus\n");
            std::exit(1);
        }
        std::error_code mec;
        m3_unp = fs::file_size(m3_src, mec);

        // engineered CDC corpus: 4 text-like files (high mutual affinity)
        // interleaved with 4 noise files in ORIGINAL order; the planner must
        // group the text-like files for the reduction to appear.
        {
            // Engineered corpus: text-like files (even indices) are
            // permutations of a SHARED pool of 32 unique 64 KiB blocks —
            // high mutual chunk-hash overlap (the planner groups them), yet
            // zero self-repetition (each file alone is incompressible, so
            // grouped adjacency inside the solid chain is what the
            // CDC-packed number cashes in). Noise files interleave the
            // original order and flush the solid window.
            constexpr size_t kPoolBlocks = 32;
            constexpr size_t kPoolBlockBytes = 16u * 1024; // 512 KiB per text file
                                                           // (fits the m1 window)
            std::vector<std::vector<core::byte>> pool;
            for (size_t b = 0; b < kPoolBlocks; ++b) {
                std::vector<core::byte> blk(kPoolBlockBytes);
                uint32_t s2 = 7000u + static_cast<uint32_t>(b) * 131u;
                for (auto& byte : blk) byte = lcg_byte(s2);
                pool.push_back(std::move(blk));
            }
            const int rotations[4] = {0, 16, 8, 24}; // T0/T2/T4/T6 start offsets
            for (size_t i = 0; i < kCdcFiles; ++i) {
                const fs::path p = dir / ("cdc_" + std::to_string(i) + ".bin");
                std::ofstream f(p, std::ios::binary);
                if (i % 2 == 0) {
                    const int rot = rotations[i / 2];
                    for (size_t off = 0; off < kCdcFileBytes; off += kPoolBlockBytes) {
                        const size_t blk =
                            ((off / kPoolBlockBytes) + static_cast<size_t>(rot)) % kPoolBlocks;
                        f.write(reinterpret_cast<const char*>(pool[blk].data()),
                                std::streamsize(kPoolBlockBytes));
                    }
                } else {
                    uint32_t n = 100000u + static_cast<uint32_t>(i) * 7919u;
                    for (size_t off = 0; off < kCdcFileBytes; ++off)
                        f.put(static_cast<char>(lcg_byte(n)));
                }
                cdc_files.push_back(p);
            }
        }

        built = true;
    }

    void build_listing_corpus() {
        if (listing_ready) return;
        namespace fs = std::filesystem;
        const fs::path ldir = dir / "listing";
        std::error_code ec;
        fs::create_directories(ldir, ec);
        listing_arc = ldir / "sparse_50g.rar";
        constexpr size_t kEntries = 100000;
        constexpr size_t kHole = 500 * 1024;
#ifdef _WIN32
        bool sparse_ok = true;
        {
            HANDLE h = CreateFileW(listing_arc.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                sparse_ok = false;
            } else {
                constexpr DWORD kFsctlSetSparse = 0x0009008Cu;
                DWORD unused = 0;
                sparse_ok = DeviceIoControl(h, kFsctlSetSparse, nullptr, 0, nullptr, 0, &unused,
                                            nullptr) != FALSE;
                CloseHandle(h);
            }
        }
        if (!sparse_ok) {
            listing_ready = false;
            return;
        }
#endif
        io::FileStream out;
        if (!out.open(listing_arc, io::FileMode::CreateAlways)) return;
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
            fb.unp_size = kHole - 300;
            fb.pack_size = static_cast<core::int64>(fb.unp_size);
            fb.method = 0;
            fb.win_size = 0;
            if (!format::HeaderWriter::write_file_block(out, fb)) return;
            out.seek(static_cast<core::int64>(fb.pack_size), io::SeekOrigin::Current);
        }
        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb);
        listing_ready = true;
    }

private:
    static void raw_store_archive(const std::filesystem::path& arc,
                                  const std::filesystem::path& src, const std::string& entry_name) {
        io::FileStream f;
        if (!f.open(src, io::FileMode::ReadOnly)) {
            std::fprintf(stderr, "[bench] FATAL: cannot read corpus\n");
            std::exit(1);
        }
        const size_t sz = static_cast<size_t>(f.size());
        std::vector<core::byte> data(sz);
        if (sz > 0 && f.read(data.data(), sz) != sz) {
            std::fprintf(stderr, "[bench] FATAL: cannot read corpus\n");
            std::exit(1);
        }
        io::FileStream out;
        if (!out.open(arc, io::FileMode::CreateAlways)) {
            std::fprintf(stderr, "[bench] FATAL: cannot create archive\n");
            std::exit(1);
        }
        format::HeaderWriter::write_signature(out);
        format::MainBlock mb;
        format::HeaderWriter::write_main_block(out, mb);
        format::FileBlock fb;
        fb.file_name = entry_name;
        fb.host_os = 1;
        fb.attributes = 0100644u;
        fb.unp_size = data.size();
        fb.pack_size = static_cast<core::int64>(data.size());
        fb.method = 0;
        fb.win_size = 0;
        if (!format::HeaderWriter::write_file_block(out, fb)) {
            std::fprintf(stderr, "[bench] FATAL: header write failed\n");
            std::exit(1);
        }
        out.write(data.data(), data.size());
        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb);
    }
};

BenchCorpora g_corpora;

// ── one-pass measurement bodies ──────────────────────────────────────────────

double pass_rs16(bool dispatched) {
    recovery::ReedSolomon16 rs;
    if (!rs.init(4, 2)) return 0.0;
    std::vector<core::byte> ecc(kBlock, core::byte(0x3C));
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kUpdates; ++i) {
        if (dispatched)
            rs.update_ecc(i % 4, 0, g_corpora.rs16_data.data(), ecc.data(), kBlock);
        else
            rs.update_ecc_scalar(i % 4, 0, g_corpora.rs16_data.data(), ecc.data(), kBlock);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    g_sink += ecc[0];
    return sec > 0 ? static_cast<double>(kUpdates) * kBlock / sec / (1024.0 * 1024.0) : 0.0;
}

double pass_match(arch::MatchFn fn) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    for (size_t i = 0; i < kMatchIters; ++i) {
        const size_t p = (i * 64) % (kMatchBuf - kMatchCap - kMatchDist);
        sink += fn(g_corpora.match_buf.data() + p, g_corpora.match_buf.data() + p + kMatchDist,
                   kMatchCap);
    }
    const auto t1 = std::chrono::steady_clock::now();
    g_sink += sink;
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return sec > 0 ? static_cast<double>(kMatchIters) * kMatchCap / sec / (1024.0 * 1024.0) : 0.0;
}

double pass_listing(bool mapped) {
    archive::ArchiveReader reader;
    reader.set_use_mapped_scan(mapped);
    int status = 0;
    std::string detail;
    const auto t0 = std::chrono::steady_clock::now();
    if (!reader.open_ex(g_corpora.listing_arc.string(), "", status, detail)) return -1.0;
    const auto t1 = std::chrono::steady_clock::now();
    const size_t n = reader.entries().size();
    reader.close();
    if (n != 100000) return -2.0;
    return std::chrono::duration<double>(t1 - t0).count();
}

double pass_extract(const std::filesystem::path& arc, uint64_t unp_bytes, int pass_idx) {
    namespace fs = std::filesystem;
    const fs::path out = g_corpora.dir / ("x_out_" + std::to_string(pass_idx));
    archive::ArchiveReader reader;
    int status = 0;
    std::string detail;
    if (!reader.open_ex(arc.string(), "", status, detail)) return -1.0;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service) continue;
        ok = reader.extract_entry(e, out / e.header.file_name) && ok;
    }
    const auto t1 = std::chrono::steady_clock::now();
    reader.close();
    if (!ok) return -2.0;
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return sec > 0 ? static_cast<double>(unp_bytes) / sec / (1024.0 * 1024.0) : 0.0;
}

double pass_add_m3(int pass_idx) {
    const std::filesystem::path arc =
        g_corpora.dir / ("a_out_" + std::to_string(pass_idx) + ".rar");
    const auto t0 = std::chrono::steady_clock::now();
    if (!archive::ArchiveMutator::add_file_to_archive(arc, g_corpora.m3_src, "m3_corpus.txt", 3))
        return -1.0;
    const auto t1 = std::chrono::steady_clock::now();
    std::error_code ec;
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(kExtractCorpusM3Bytes);
    std::filesystem::remove(arc, ec);
    return sec > 0 ? bytes / sec / (1024.0 * 1024.0) : 0.0;
}

double pass_cdc_fingerprint() {
    compress::CdcChunker chunker;
    std::vector<core::uint64> hashes;
    std::vector<compress::CdcChunk> chunks;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& p : g_corpora.cdc_files) {
        io::FileStream f;
        if (!f.open(p, io::FileMode::ReadOnly)) return -1.0;
        compress::CdcStreamChunker stream(chunker);
        std::vector<core::byte> buf(1024 * 1024);
        for (;;) {
            const size_t got = f.read(buf.data(), buf.size());
            if (got == 0) break;
            chunks.clear();
            stream.feed(buf.data(), got, chunks);
        }
        chunks.clear();
        stream.finish(chunks);
        for (const auto& c : chunks) hashes.push_back(c.hash);
    }
    const auto t1 = std::chrono::steady_clock::now();
    g_sink += hashes.size();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(kCdcFiles * kCdcFileBytes);
    return sec > 0 ? bytes / sec / (1024.0 * 1024.0) : 0.0;
}

// Packs the engineered corpus ONE WAY via the same batch pipeline the CLI
// uses (CompressPlan -> prepare_add_file -> write_batch_add) — per-file
// add_file_to_archive calls are separate pack passes and never carry a solid
// chain across files. mode 0 = store, 1 = plain solid (original order),
// 2 = CDC-planned order + solid. Packed at method 1 (512 KiB window): the
// 2 MiB noise separators flush the solid window between the text-like files,
// so the reduction is window-bounded by design (v1.26 framing). Returns the
// archive size in bytes.
double pass_cdc_pack(int mode, int pass_idx) {
    namespace fs = std::filesystem;
    const fs::path arc =
        g_corpora.dir / ("cdc_" + std::to_string(mode) + "_" + std::to_string(pass_idx) + ".rar");
    const uint32_t method = mode == 0 ? 0u : 1u;
    const bool solid = mode >= 1;

    std::vector<size_t> order(g_corpora.cdc_files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (mode == 2) {
        compress::CdcChunker chunker;
        std::vector<std::vector<core::uint64>> hashes(g_corpora.cdc_files.size());
        for (size_t i = 0; i < g_corpora.cdc_files.size(); ++i) {
            io::FileStream f;
            if (!f.open(g_corpora.cdc_files[i], io::FileMode::ReadOnly)) return -1.0;
            compress::CdcStreamChunker stream(chunker);
            std::vector<core::byte> buf(1024 * 1024);
            std::vector<compress::CdcChunk> chunks;
            for (;;) {
                const size_t got = f.read(buf.data(), buf.size());
                if (got == 0) break;
                chunks.clear();
                stream.feed(buf.data(), got, chunks);
                for (const auto& c : chunks) hashes[i].push_back(c.hash);
            }
            chunks.clear();
            stream.finish(chunks);
            for (const auto& c : chunks) hashes[i].push_back(c.hash);
        }
        order = plan_cdc_order(hashes).order;
    }

    const uint64_t eff_dict = compress::default_dict_size_for_method(method);
    std::vector<compress::EntryPlan> reqs(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
        reqs[k].method = method;
        reqs[k].raw_size = kCdcFileBytes;
    }
    auto comp_plan = compress::CompressPlan::plan_entries(reqs, solid, method, eff_dict);
    std::unique_ptr<compress::SolidPacker> solid_packer;
    if (solid) {
        solid_packer = std::make_unique<compress::SolidPacker>(
            method, static_cast<size_t>(compress::snap_window_to_fci_grid(eff_dict)));
    }

    std::vector<archive::ArchiveMutator::PreparedAdd> prepared(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
        const size_t src_idx = order[k];
        if (!archive::ArchiveMutator::prepare_add_file(
                g_corpora.cdc_files[src_idx], g_corpora.cdc_files[src_idx].filename().string(),
                static_cast<int>(method), "", prepared[k], openrar::archive::time_flags::MTIME, 0,
                false, false, solid, false, {}, "", "", 1, solid_packer.get(),
                comp_plan.entries[k].is_solid_chain, false)) {
            return -2.0;
        }
    }
    if (!archive::ArchiveMutator::write_batch_add(arc, prepared)) return -3.0;
    std::error_code ec;
    const double size = static_cast<double>(fs::file_size(arc, ec));
    fs::remove(arc, ec);
    return ec ? -4.0 : size;
}

// ── protocol + reporting ─────────────────────────────────────────────────────

struct SuiteDef {
    std::string name;
    std::string kernel;
    std::string kind; // "compute" | "io"
    std::string unit;
    std::function<double(int)> pass; // one timed pass → value (idx = pass no.)
    bool available = true;
    std::string skip_reason;
};

struct SuiteResult {
    std::string name, kernel, kind, unit;
    std::vector<double> passes;
    double median = 0.0;
    double spread_pct = 0.0;
    bool skipped = false;
    std::string skip_reason;
};

std::string host_cpu() {
#if defined(_MSC_VER)
    int regs[4] = {0};
    char brand[49] = {0};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        std::string s(brand);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        if (!s.empty()) return s;
    }
    return "unknown";
#elif defined(__APPLE__)
    char buf[128] = {0};
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
    return "unknown";
#elif defined(__linux__)
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            const size_t c = line.find(':');
            if (c != std::string::npos) return line.substr(c + 2);
        }
    }
    return "unknown";
#else
    return "unknown";
#endif
}

std::string host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

std::string host_compiler() {
#if defined(_MSC_VER)
    return "msvc-" + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "clang-" + std::to_string(__clang_major__);
#elif defined(__GNUC__)
    return "gcc-" + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

SuiteResult run_suite(const SuiteDef& def, bool quick) {
    SuiteResult r;
    r.name = def.name;
    r.kernel = def.kernel;
    r.kind = def.kind;
    r.unit = def.unit;
    if (!def.available) {
        r.skipped = true;
        r.skip_reason = def.skip_reason;
        return r;
    }
    const int passes = quick ? 3 : (def.kind == "io" ? 3 : 7);
    // Warm-up: one untimed pass (for the io listing suites the corpus build
    // happens on this first call and IS the warm-up, per the plan).
    (void)def.pass(-1);
    for (int i = 0; i < passes; ++i) r.passes.push_back(def.pass(i));
    std::vector<double> sorted = r.passes;
    std::sort(sorted.begin(), sorted.end());
    r.median = sorted[sorted.size() / 2];
    const double lo = sorted.front();
    const double hi = sorted.back();
    r.spread_pct = (r.median > 0.0 && hi > lo) ? (hi - lo) / r.median * 100.0 : 0.0;
    return r;
}

void print_human(const SuiteResult& r, bool to_stderr) {
    std::FILE* out = to_stderr ? stderr : stdout;
    if (r.skipped) {
        std::fprintf(out, "[bench] %-34s SKIPPED (%s)\n", r.name.c_str(), r.skip_reason.c_str());
        return;
    }
    std::fprintf(out, "[bench] %-34s = %12.1f %s   (spread %.1f%%)\n", r.name.c_str(), r.median,
                 r.unit.c_str(), r.spread_pct);
}

std::string to_json(const std::vector<SuiteResult>& results, bool quick, int passes_default) {
    std::ostringstream o;
    o << "{\"schema_version\":1\n,\"host\":{\"cpu\":\"" << host_cpu()
      << "\",\"cores\":" << std::thread::hardware_concurrency() << ",\"os\":\"" << host_os()
      << "\",\"compiler\":\"" << host_compiler() << "\",\"clock\":\"steady_clock\"}"
      << "\n,\"protocol\":{\"warmup\":1,\"passes\":" << passes_default
      << ",\"quick\":" << (quick ? "true" : "false") << ",\"spread_metric\":\"(max-min)/median\"}"
      << "\n,\"suites\":[";
    bool first = true;
    for (const auto& r : results) {
        if (!first) o << ",";
        first = false;
        o << "\n  {\"name\":\"" << r.name << "\"";
        if (!r.kernel.empty()) o << ",\"kernel\":\"" << r.kernel << "\"";
        o << ",\"kind\":\"" << r.kind << "\",\"unit\":\"" << r.unit
          << "\",\"skipped\":" << (r.skipped ? "true" : "false");
        if (r.skipped) {
            o << ",\"skip_reason\":\"" << r.skip_reason << "\"}";
            continue;
        }
        o << ",\"median\":" << r.median << ",\"spread_pct\":" << r.spread_pct << ",\"passes\":[";
        for (size_t i = 0; i < r.passes.size(); ++i) {
            if (i) o << ",";
            o << r.passes[i];
        }
        o << "]}";
    }
    o << "\n]}\n";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool quick = false;
    bool strict = false;
    double threshold = 5.0;
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            json = true;
        } else if (a == "--quick") {
            quick = true;
        } else if (a == "--strict") {
            strict = true;
        } else if (a == "--spread-threshold-pct" && i + 1 < argc) {
            threshold = std::atof(argv[++i]);
        } else if (a.rfind("--suite=", 0) == 0) {
            wanted.push_back(a.substr(8));
        } else if (a == "--suite" && i + 1 < argc) {
            wanted.push_back(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: openrar_bench [--json] [--quick] [--strict] "
                                 "[--suite <name>] [--spread-threshold-pct <x>]\n");
            return 2;
        }
    }

    const auto& cpu = core::get_cpu_features();
    std::vector<SuiteDef> suites;

    std::string rs_kernel = "scalar (none)";
#if defined(__aarch64__) || defined(_M_ARM64)
    rs_kernel = "NEON";
#elif defined(OPENRAR_HAS_GFNI_KERNEL)
    if (recovery::ReedSolomon16::gfni_kernel_active()) rs_kernel = "GFNI";
#endif
    suites.push_back({"rs16_fold_scalar", "scalar", "compute", "MiB/s",
                      [](int) { return pass_rs16(false); }, true, ""});
    suites.push_back({"rs16_fold_dispatched", rs_kernel, "compute", "MiB/s",
                      [](int) { return pass_rs16(true); }, true, ""});

    suites.push_back({"match_length_scalar", "scalar", "compute", "MiB/s",
                      [](int) { return pass_match(arch::match_length_scalar); }, true, ""});
#if defined(OPENRAR_HAS_X86_SIMD)
    if (cpu.sse2)
        suites.push_back({"match_length_SSE2", "SSE2", "compute", "MiB/s",
                          [](int) { return pass_match(arch::match_length_sse2); }, true, ""});
    if (cpu.avx2)
        suites.push_back({"match_length_AVX2", "AVX2", "compute", "MiB/s",
                          [](int) { return pass_match(arch::match_length_avx2); }, true, ""});
#endif
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)
    if (cpu.neon)
        suites.push_back({"match_length_NEON", "NEON", "compute", "MiB/s",
                          [](int) { return pass_match(arch::match_length_neon); }, true, ""});
#endif

    // io suites: temp-disk driven. The listing corpus build IS the warm-up.
    suites.push_back(
        {"extract_throughput_store", "", "io", "MiB/s",
         [](int i) { return pass_extract(g_corpora.store_arc, kExtractCorpusBytes, i); }, true,
         ""});
    suites.push_back({"extract_throughput_m3", "", "io", "MiB/s",
                      [](int i) { return pass_extract(g_corpora.m3_arc, g_corpora.m3_unp, i); },
                      true, ""});
    suites.push_back(
        {"add_throughput_m3", "", "io", "MiB/s", [](int i) { return pass_add_m3(i); }, true, ""});
    suites.push_back({"cdc_fingerprint", "", "compute", "MiB/s",
                      [](int) { return pass_cdc_fingerprint(); }, true, ""});
    suites.push_back({"cdc_three_number_store", "", "compute", "B",
                      [](int i) { return pass_cdc_pack(0, i); }, true, ""});
    suites.push_back({"cdc_three_number_plain_solid", "", "compute", "B",
                      [](int i) { return pass_cdc_pack(1, i); }, true, ""});
    suites.push_back({"cdc_three_number_cdc_packed", "", "compute", "B",
                      [](int i) { return pass_cdc_pack(2, i); }, true, ""});

    // listing suites: build the sparse corpus ONLY when they are selected
    // (the build is once-per-run, ~30 MB of header writes).
    g_corpora.build();
    const bool want_listing = wanted.empty() || contains(wanted, "listing_50gb_mapped") ||
                              contains(wanted, "listing_50gb_buffered");
    bool listing_ok = false;
    if (want_listing) {
        g_corpora.build_listing_corpus();
        listing_ok = g_corpora.listing_ready;
    }
    SuiteDef listing_mapped{"listing_50gb_mapped",
                            "mapped",
                            "io",
                            "s",
                            [](int) {
                                return pass_listing(true); },
                            true,
                            ""};
    SuiteDef listing_buffered{"listing_50gb_buffered",
                              "buffered",
                              "io",
                              "s",
                              [](int) {
                                  return pass_listing(false); },
                              true,
                              ""};
    if (!listing_ok) {
        listing_mapped.available = false;
        listing_mapped.skip_reason = "sparse unsupported";
        listing_buffered.available = false;
        listing_buffered.skip_reason = "sparse unsupported";
    }
    suites.push_back(listing_mapped);
    suites.push_back(listing_buffered);

    // filter
    if (!wanted.empty()) {
        std::vector<SuiteDef> kept;
        for (const auto& w : wanted) {
            bool found = false;
            for (const auto& s : suites) {
                if (s.name == w) {
                    kept.push_back(s);
                    found = true;
                }
            }
            if (!found) {
                std::fprintf(stderr, "[bench] unknown suite: %s\n", w.c_str());
                return 2;
            }
        }
        suites = std::move(kept);
    }

    std::vector<SuiteResult> results;
    for (const auto& s : suites) results.push_back(run_suite(s, quick));

    // reporting: default humans on stdout; --json flips stdout to the JSON
    // document only and moves the human lines to stderr (purity pattern).
    for (const auto& r : results) print_human(r, json);
    if (json) {
        const int passes_default = quick ? 3 : 7;
        std::fputs(to_json(results, quick, passes_default).c_str(), stdout);
    }
    // --json stdout purity: the JSON document is the only stdout content.
    std::fprintf(json ? stderr : stdout, "[bench] sink=%llu\n",
                 static_cast<unsigned long long>(g_sink));

    if (strict) {
        for (const auto& r : results) {
            if (!r.skipped && r.kind == "compute" && r.spread_pct > threshold) {
                std::fprintf(stderr, "[bench] STRICT: %s spread %.1f%% > %.1f%%\n", r.name.c_str(),
                             r.spread_pct, threshold);
                return 1;
            }
        }
    }
    return 0;
}
