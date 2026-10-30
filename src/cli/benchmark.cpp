#include "benchmark.hpp"
#include "json_output.hpp"
#include "progress.hpp"

#include "../compress/compressor50.hpp"
#include "../compress/decompressor50.hpp"
#include "../core/cpu.hpp"
#include "../crypto/crc32.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace openrar::cli {

namespace {

// Deterministic synthetic corpus generator (~4.2 bits/byte entropy).
// Combines dictionary-matchable source text with pseudo-random byte runs.
std::vector<core::byte> generate_benchmark_corpus(size_t size) {
    std::vector<core::byte> corpus;
    corpus.reserve(size);

    static const char* k_words[] = {"function",    "return",       "compress",    "decompress",
                                    "archive",     "dictionary",   "checksum",    "recovery",
                                    "cauchy",      "reed_solomon", "hardware",    "concurrency",
                                    "thread_pool", "streaming",    "performance", "throughput",
                                    "simd_vector", "avx512",       "matrix",      "pipeline"};
    constexpr size_t k_word_count = sizeof(k_words) / sizeof(k_words[0]);

    uint32_t state = 0x1337C0DE;
    auto lcg_next = [&]() -> uint32_t {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    while (corpus.size() < size) {
        uint32_t r = lcg_next();
        uint32_t mode = r % 100;
        if (mode < 60) {
            // Emits structured repetitive English / source tokens with whitespace
            const char* w = k_words[(r >> 8) % k_word_count];
            for (const char* p = w; *p && corpus.size() < size; ++p) {
                corpus.push_back(static_cast<core::byte>(*p));
            }
            if (corpus.size() < size) corpus.push_back(static_cast<core::byte>(' '));
        } else if (mode < 85) {
            // Repeating numeric sequence / table
            uint32_t count = 4 + (r % 16);
            for (uint32_t i = 0; i < count && corpus.size() < size; ++i) {
                corpus.push_back(static_cast<core::byte>('0' + (i % 10)));
            }
            if (corpus.size() < size) corpus.push_back(static_cast<core::byte>('\n'));
        } else {
            // Pseudo-random byte run
            uint32_t count = 2 + (r % 8);
            for (uint32_t i = 0; i < count && corpus.size() < size; ++i) {
                corpus.push_back(static_cast<core::byte>(lcg_next() & 0xFF));
            }
        }
    }
    return corpus;
}

uint32_t compute_crc32(const core::byte* data, size_t size) {
    crypto::Crc32 crc;
    crc.update(data, size);
    return crc.get();
}

} // namespace

BenchmarkResult run_benchmark(const BenchmarkOptions& opt) {
    BenchmarkResult res;
    res.cpu_brand = core::get_cpu_brand_string();
#if defined(__x86_64__) || defined(_M_X64)
    res.arch = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    res.arch = "x86_32";
#elif defined(__aarch64__) || defined(_M_ARM64)
    res.arch = "ARM64";
#elif defined(__arm__) || defined(_M_ARM)
    res.arch = "ARM32";
#else
    res.arch = "Generic";
#endif

    const core::AccelerationReport accel = core::describe_acceleration();
    for (const char* tag : accel.tags) {
        res.simd_features.push_back(tag);
    }

    res.logical_cores = std::thread::hardware_concurrency();
    if (res.logical_cores == 0) res.logical_cores = 1;

    unsigned threads = opt.threads == 0 ? res.logical_cores : opt.threads;
    if (threads > 256) threads = 256;
    res.threads_used = threads;
    res.dict_size = opt.dict_size;
    res.method = opt.method;

    // Directives 1 & 6: Clamped single shared read-only corpus (16 MiB)
    constexpr size_t k_corpus_size = 16 * 1024 * 1024;
    const std::vector<core::byte> corpus = generate_benchmark_corpus(k_corpus_size);

    // Warm-up pass (Directive 6: 100ms un-timed to steady CPU boost clocks)
    {
        std::vector<core::byte> warm_packed;
        compress::Compressor50::compress_buffer(corpus.data(), 512 * 1024, warm_packed, opt.method,
                                                opt.dict_size);
    }

    const unsigned phase_sec = opt.seconds < 1 ? 1 : (opt.seconds > 60 ? 60 : opt.seconds);
    const double phase_limit_sec = static_cast<double>(phase_sec);

    // ── Phase 1: Single-Thread Compression Benchmark ──────────────────────────
    constexpr size_t k_st_slice_size = 4 * 1024 * 1024; // 4 MiB slice
    const core::byte* st_src = corpus.data();
    uint64_t st_bytes_in = 0;
    auto t_st_start = std::chrono::steady_clock::now();
    for (;;) {
        std::vector<core::byte> packed;
        compress::Compressor50::compress_buffer(st_src, k_st_slice_size, packed, opt.method,
                                                opt.dict_size);
        st_bytes_in += k_st_slice_size;
        auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_st_start).count();
        if (elapsed >= std::max(1.0, phase_limit_sec * 0.4)) {
            res.st_compress_kbps =
                (static_cast<double>(st_bytes_in) / 1024.0) / std::max(elapsed, 0.001);
            break;
        }
    }

    // ── Phase 2: Multi-Thread Compression Benchmark ───────────────────────────
    std::atomic<bool> mt_stop{false};
    std::vector<uint64_t> thread_bytes(threads, 0);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    const size_t chunk_per_thread = std::min<size_t>(k_corpus_size / threads, 4 * 1024 * 1024);
    const size_t actual_chunk = std::max<size_t>(chunk_per_thread, 256 * 1024);

    auto t_mt_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < threads; ++i) {
        const size_t offset = (i * actual_chunk) % (k_corpus_size - actual_chunk);
        workers.emplace_back([&, i, offset]() {
            std::vector<core::byte> packed;
            while (!mt_stop.load(std::memory_order_relaxed)) {
                packed.clear();
                compress::Compressor50::compress_buffer(corpus.data() + offset, actual_chunk,
                                                        packed, opt.method, opt.dict_size);
                thread_bytes[i] += actual_chunk;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(phase_sec * 1000));
    mt_stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    double mt_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_mt_start).count();

    uint64_t total_mt_bytes = 0;
    for (uint64_t b : thread_bytes) total_mt_bytes += b;
    res.mt_compress_kbps =
        (static_cast<double>(total_mt_bytes) / 1024.0) / std::max(mt_elapsed, 0.001);
    res.speedup_ratio =
        res.st_compress_kbps > 0.0 ? (res.mt_compress_kbps / res.st_compress_kbps) : 1.0;

    // ── Phase 3: Parallel Decompression Benchmark & CRC32 Verification ────────
    // Pre-compress reference slice
    std::vector<core::byte> ref_packed;
    const size_t decomp_slice_size = std::min<size_t>(k_corpus_size, 2 * 1024 * 1024);
    compress::Compressor50::compress_buffer(corpus.data(), decomp_slice_size, ref_packed,
                                            opt.method, opt.dict_size);
    const uint32_t expected_crc = compute_crc32(corpus.data(), decomp_slice_size);

    std::atomic<bool> decomp_stop{false};
    std::atomic<bool> all_crc_valid{true};
    std::vector<uint64_t> decomp_bytes(threads, 0);
    workers.clear();

    auto t_decomp_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < threads; ++i) {
        workers.emplace_back([&, i]() {
            compress::Decompressor50 dec(opt.dict_size);
            std::vector<core::byte> out_buf;
            out_buf.reserve(decomp_slice_size);
            while (!decomp_stop.load(std::memory_order_relaxed)) {
                out_buf.clear();
                bool ok = dec.decompress(ref_packed.data(), ref_packed.size(), decomp_slice_size,
                                         false, [&](const core::byte* p, size_t n) -> bool {
                                             out_buf.insert(out_buf.end(), p, p + n);
                                             return true;
                                         });
                if (!ok || out_buf.size() != decomp_slice_size ||
                    compute_crc32(out_buf.data(), out_buf.size()) != expected_crc) {
                    all_crc_valid.store(false, std::memory_order_relaxed);
                    break;
                }
                decomp_bytes[i] += decomp_slice_size;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1000u, phase_sec * 500)));
    decomp_stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    double decomp_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_decomp_start).count();

    uint64_t total_decomp_bytes = 0;
    for (uint64_t b : decomp_bytes) total_decomp_bytes += b;
    res.mt_decompress_kbps =
        (static_cast<double>(total_decomp_bytes) / 1024.0) / std::max(decomp_elapsed, 0.001);
    res.verified = all_crc_valid.load();

    // Standardized RARMark v2.0 Score
    res.rarmark_score =
        static_cast<unsigned>((res.mt_compress_kbps * 0.6 + res.mt_decompress_kbps * 0.4) / 10.0);

    return res;
}

int run_benchmark_cli(const BenchmarkOptions& opt) {
    if (!opt.json && !opt.plain) {
        std::cout << "\n  \033[1;36mOpenRAR 2.0.0 Built-in Benchmark\033[0m\n"
                  << "  \033[90m===================================================\033[0m\n"
                  << "  Benchmarking in-memory RAR5 compression & parallel decode...\n"
                  << std::flush;
    }

    BenchmarkResult res = run_benchmark(opt);

    if (opt.json) {
        std::cout << "{\n"
                  << "  \"benchmark\": \"openrar_rarmark_v2\",\n"
                  << "  \"version\": \"2.0.0\",\n"
                  << "  \"cpu\": {\n"
                  << "    \"brand\": \"" << escape_json_string(res.cpu_brand) << "\",\n"
                  << "    \"arch\": \"" << escape_json_string(res.arch) << "\",\n"
                  << "    \"logical_cores\": " << res.logical_cores << ",\n"
                  << "    \"simd_features\": [";
        for (size_t i = 0; i < res.simd_features.size(); ++i) {
            std::cout << (i > 0 ? ", \"" : "\"") << res.simd_features[i] << "\"";
        }
        std::cout << "]\n"
                  << "  },\n"
                  << "  \"config\": {\n"
                  << "    \"threads\": " << res.threads_used << ",\n"
                  << "    \"dict_size_bytes\": " << res.dict_size << ",\n"
                  << "    \"method\": " << res.method << "\n"
                  << "  },\n"
                  << "  \"results\": {\n"
                  << "    \"st_compress_kbps\": " << std::fixed << std::setprecision(1)
                  << res.st_compress_kbps << ",\n"
                  << "    \"mt_compress_kbps\": " << res.mt_compress_kbps << ",\n"
                  << "    \"mt_decompress_kbps\": " << res.mt_decompress_kbps << ",\n"
                  << "    \"speedup_ratio\": " << std::setprecision(2) << res.speedup_ratio << ",\n"
                  << "    \"rarmark_score\": " << res.rarmark_score << ",\n"
                  << "    \"decompression_verified\": " << (res.verified ? "true" : "false") << "\n"
                  << "  }\n"
                  << "}\n";
        return 0;
    }

    const bool vt = is_vt_supported();
    auto c = [vt](const char* code) -> const char* {
        return vt ? code : "";
    };

    std::cout << "\n  " << c("\033[1;37m") << "System Topology:" << c("\033[0m\n")
              << "    CPU Model:     " << res.cpu_brand << " (" << res.arch << ")\n"
              << "    Cores / MT:    " << res.logical_cores << " logical threads ("
              << res.threads_used << " engaged)\n"
              << "    Hardware SIMD: ";
    if (res.simd_features.empty()) {
        std::cout << "Scalar Baseline\n";
    } else {
        for (size_t i = 0; i < res.simd_features.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << res.simd_features[i];
        }
        std::cout << "\n";
    }

    std::cout << "\n  " << c("\033[1;37m") << "Throughput Results:" << c("\033[0m\n")
              << "    Single-Thread Compression:  " << std::setw(10) << std::fixed
              << std::setprecision(0) << res.st_compress_kbps << " KB/s\n"
              << "    Multi-Thread Compression:   " << std::setw(10) << res.mt_compress_kbps
              << " KB/s  (" << c("\033[1;32m") << std::setprecision(2) << res.speedup_ratio
              << "x scaling" << c("\033[0m") << ")\n"
              << "    Parallel Decompression:     " << std::setw(10) << std::setprecision(0)
              << res.mt_decompress_kbps << " KB/s  (" << std::setprecision(2)
              << (res.mt_decompress_kbps / (1024.0 * 1024.0)) << " GB/s)\n"
              << "    Decompression Integrity:    "
              << (res.verified
                      ? (std::string(c("\033[1;32m")) + "[PASS] Verified CRC32 match" +
                         c("\033[0m"))
                      : (std::string(c("\033[1;31m")) + "[FAIL] CRC32 Mismatch" + c("\033[0m")))
              << "\n"
              << "\n  " << c("\033[1;33m") << "RARMark v2.0 Score:          " << c("\033[1;32m")
              << res.rarmark_score << " pts" << c("\033[0m\n") << "  " << c("\033[90m")
              << "===================================================" << c("\033[0m\n\n");

    return res.verified ? 0 : 1;
}

} // namespace openrar::cli
