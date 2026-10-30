#include "../../src/cli/benchmark.hpp"
#include "../../src/core/cpu.hpp"

#include <iostream>
#include <sstream>
#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar;
using namespace openrar::cli;

void test_cpu_brand_and_acceleration() {
    std::string brand = core::get_cpu_brand_string();
    REQUIRE(!brand.empty());

    core::AccelerationReport accel = core::describe_acceleration();
    std::cout << "[PASS] CPU brand detected: " << brand << " (" << accel.tags.size()
              << " hardware acceleration tags)\n";
}

void test_benchmark_single_thread() {
    BenchmarkOptions opt;
    opt.threads = 1;
    opt.seconds = 1;
    opt.dict_size = 1024 * 1024; // 1 MiB for fast unit test
    opt.method = 3;
    opt.plain = true;

    BenchmarkResult res = run_benchmark(opt);
    REQUIRE(res.threads_used == 1);
    REQUIRE(res.st_compress_kbps > 0.0);
    REQUIRE(res.mt_compress_kbps > 0.0);
    REQUIRE(res.mt_decompress_kbps > 0.0);
    REQUIRE(res.verified); // Directive 6: CRC32 decompression match verified
    REQUIRE(res.rarmark_score > 0);

    std::cout << "[PASS] ST Benchmark: " << res.st_compress_kbps << " KB/s compress, "
              << res.mt_decompress_kbps << " KB/s decompress, RARMark=" << res.rarmark_score
              << "\n";
}

void test_benchmark_multi_thread() {
    BenchmarkOptions opt;
    opt.threads = 2;
    opt.seconds = 1;
    opt.dict_size = 1024 * 1024;
    opt.method = 3;
    opt.plain = true;

    BenchmarkResult res = run_benchmark(opt);
    REQUIRE(res.threads_used == 2);
    REQUIRE(res.mt_compress_kbps > 0.0);
    REQUIRE(res.speedup_ratio > 0.0);
    REQUIRE(res.verified);

    std::cout << "[PASS] MT Benchmark (2 threads): " << res.mt_compress_kbps
              << " KB/s, scaling=" << res.speedup_ratio << "x\n";
}

void test_benchmark_clamping_and_bounds() {
    BenchmarkOptions opt;
    opt.threads = 999; // Should clamp to 256
    opt.seconds = 0;   // Should clamp to 1
    opt.dict_size = 1024 * 1024;
    opt.method = 3;
    opt.plain = true;

    BenchmarkResult res = run_benchmark(opt);
    REQUIRE(res.threads_used <= 256);
    REQUIRE(res.verified);

    std::cout << "[PASS] Bounds clamped: threads=" << res.threads_used << "\n";
}

void test_benchmark_cli_json_and_plain() {
    BenchmarkOptions opt;
    opt.threads = 1;
    opt.seconds = 1;
    opt.dict_size = 1024 * 1024;
    opt.method = 3;
    opt.plain = true;
    opt.json = true;

    // Redirect cout to buffer to capture JSON
    std::streambuf* orig_cout = std::cout.rdbuf();
    std::stringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    int rc = run_benchmark_cli(opt);

    std::cout.rdbuf(orig_cout);

    REQUIRE(rc == 0);
    std::string json_str = captured.str();
    REQUIRE(json_str.find("\"benchmark\": \"openrar_rarmark_v2\"") != std::string::npos);
    REQUIRE(json_str.find("\"decompression_verified\": true") != std::string::npos);

    std::cout << "[PASS] CLI JSON output format validated.\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_cpu_brand_and_acceleration", test_cpu_brand_and_acceleration, false},
    {"test_benchmark_single_thread", test_benchmark_single_thread, false},
    {"test_benchmark_multi_thread", test_benchmark_multi_thread, false},
    {"test_benchmark_clamping_and_bounds", test_benchmark_clamping_and_bounds, false},
    {"test_benchmark_cli_json_and_plain", test_benchmark_cli_json_and_plain, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All Built-in Benchmark Unit Tests PASSED!\n";
    return rc;
}
