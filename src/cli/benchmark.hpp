#ifndef OPENRAR_CLI_BENCHMARK_HPP
#define OPENRAR_CLI_BENCHMARK_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace openrar::cli {

struct BenchmarkOptions {
    unsigned threads{0};               // 0 = auto-detect (hardware_concurrency)
    size_t dict_size{4 * 1024 * 1024}; // default 4 MiB
    int method{3};                     // compression method (0..5, default 3)
    unsigned seconds{5};               // duration per timed phase (default 5s, min 1, max 60)
    bool plain{false};                 // disable ANSI animations
    bool json{false};                  // emit JSON output
};

struct BenchmarkResult {
    std::string cpu_brand;
    std::string arch;
    std::vector<std::string> simd_features;
    unsigned logical_cores{1};
    unsigned threads_used{1};
    size_t dict_size{0};
    int method{3};

    double st_compress_kbps{0.0};
    double mt_compress_kbps{0.0};
    double mt_decompress_kbps{0.0};
    double speedup_ratio{1.0};
    unsigned rarmark_score{0};
    bool verified{false}; // true if decompression CRC32 matched corpus CRC32
};

// Executes the in-memory benchmark according to BenchmarkOptions.
// Bounded memory footprint (single shared read-only corpus <= 32 MiB).
BenchmarkResult run_benchmark(const BenchmarkOptions& opt);

// Formats and outputs the benchmark result to stdout (or JSON).
int run_benchmark_cli(const BenchmarkOptions& opt);

} // namespace openrar::cli

#endif // OPENRAR_CLI_BENCHMARK_HPP
