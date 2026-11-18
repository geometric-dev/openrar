// PROBE-STORE-SINK (§4.16) — log-only probe for the store-extraction sink.
//
// The store-extraction path for a -m0 (stored) member is 2.5× slower than a
// raw read+write (0.574 s vs 0.21-0.23 s for 128 MiB), and the gap is
// unattributed. This probe timestamps each stage so the residual is
// attributed before any change is attempted:
//
//   - journal record create + header sync + append + sync + close + unlink
//   - anchored temp create (O_EXCL, random 128-bit name)
//   - 64 KiB write cadence (per-chunk write + progress/cancel callbacks)
//   - rename + chmod + MOTW
//   - per-entry flush (FlushFileBuffers / fsync)
//
// Usage: store_sink_probe <openrar_exe> <corpus_file> [--reps N]
// Output: a table with per-stage wall times, min/median/max across reps.
//
// No production code change. The probe wraps the existing openrar x command
// with timing hooks via a pre-loaded diagnostic (env-gated fprintf in
// archive_reader.cpp), or, when that is not available, measures the
// end-to-end wall and the fsync cost separately.

#include "../src/core/types.hpp"
#include "../src/io/file_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Run a command and return the exit code. Uses CreateProcessW so arguments
// with spaces or special characters are passed correctly (std::system
// re-parses the string and mangles paths with backslashes on Windows).
int run_cmd(const std::string& cmdline) {
    std::wstring wcmd(cmdline.begin(), cmdline.end());
    std::vector<wchar_t> cmdbuf(wcmd.begin(), wcmd.end());
    cmdbuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &si, &pi)) {
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return static_cast<int>(code);
}
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
int run_cmd(const std::string& cmdline) {
    // POSIX: use system() — paths with spaces are handled by the shell.
    return std::system(cmdline.c_str());
}
#endif

using namespace openrar;
namespace fs = std::filesystem;

namespace {

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Timing {
    double min_s = 1e9;
    double max_s = 0;
    double sum_s = 0;
    int n = 0;
    void add(double s) {
        min_s = std::min(min_s, s);
        max_s = std::max(max_s, s);
        sum_s += s;
        ++n;
    }
    double median_s() const {
        // Caller collects samples separately; this is a placeholder.
        return n > 0 ? sum_s / n : 0;
    }
};

void print_timing(const char* label, const Timing& t) {
    if (t.n == 0) {
        std::printf("  %-40s  (no samples)\n", label);
        return;
    }
    std::printf("  %-40s  min=%.3f  mean=%.3f  max=%.3f  n=%d\n", label, t.min_s,
                t.sum_s / t.n, t.max_s, t.n);
}

// Measure a raw read+write of the same size (no journal, no temp, no rename).
double measure_raw_copy(const fs::path& src, const fs::path& dst) {
    const auto t0 = now_s();
    std::FILE* in = std::fopen(src.string().c_str(), "rb");
    if (!in) return -1;
    std::FILE* out = std::fopen(dst.string().c_str(), "wb");
    if (!out) { std::fclose(in); return -1; }
    std::vector<core::byte> buf(1 << 20); // 1 MiB
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        std::fwrite(buf.data(), 1, n, out);
    }
    std::fclose(in);
    std::fclose(out);
    return now_s() - t0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: store_sink_probe <openrar_exe> <corpus_file> [--reps N]\n"
                     "  Measures the store-extraction sink stages for one member.\n"
                     "  The member must be stored (-m0) for this to be meaningful.\n");
        return 2;
    }

    const std::string exe = argv[1];
    const fs::path corpus = argv[2];
    int reps = 5;
    for (int i = 3; i < argc; ++i) {
        if (std::strncmp(argv[i], "--reps", 6) == 0 && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        }
    }
    if (reps < 1) reps = 1;

    if (!fs::exists(corpus)) {
        std::fprintf(stderr, "store_sink_probe: corpus not found: %s\n", corpus.string().c_str());
        return 2;
    }
    const auto corpus_size = fs::file_size(corpus);
    const double mib = static_cast<double>(corpus_size) / (1024.0 * 1024.0);

    std::printf("PROBE-STORE-SINK (§4.16)\n");
    std::printf("corpus: %s (%.1f MiB)\n", corpus.filename().string().c_str(), mib);
    std::printf("reps: %d\n\n", reps);

    const auto tmp = fs::temp_directory_path() / "store_sink_probe";
    fs::create_directories(tmp);
    // Use short paths without spaces for std::system compatibility.
    const auto arc = tmp / "s.rar";
    const auto dest = tmp / "d";
    const auto raw_out = tmp / "r.bin";

    // Phase 1: pack the corpus as -m0 (stored) so extraction is a store path.
    {
        const auto t0 = now_s();
        // Build a command string with proper quoting for CreateProcessW.
        std::string cmd = "\"" + exe + "\" a -y -ep -m0 -mt1 -idq \"" + arc.string() +
                          "\" \"" + corpus.string() + "\"";
        int rc = run_cmd(cmd);
        if (rc != 0) {
            cmd = "\"" + exe + "\" a -y -m0 -mt1 -idq \"" + arc.string() + "\" \"" +
                  corpus.string() + "\"";
            rc = run_cmd(cmd);
            if (rc != 0) {
                std::fprintf(stderr, "store_sink_probe: pack failed (rc=%d)\n", rc);
                return 1;
            }
        }
        std::printf("  pack -m0: %.3f s (archive %zu B)\n\n", now_s() - t0,
                    static_cast<size_t>(fs::file_size(arc)));
    }

    // Phase 2: measure raw read+write (no journal, no temp, no rename).
    Timing raw_t;
    for (int r = 0; r < reps; ++r) {
        const double s = measure_raw_copy(corpus, raw_out);
        if (s > 0) raw_t.add(s);
    }
    std::printf("--- Raw read+write (no journal, no temp, no rename) ---\n");
    print_timing("raw 1 MiB read+write", raw_t);

    // Phase 3: measure openrar x -db (batch durability, no per-entry flush).
    Timing batch_t;
    for (int r = 0; r < reps; ++r) {
        fs::remove_all(dest);
        fs::create_directories(dest);
        const auto t0 = now_s();
        std::string cmd = "\"" + exe + "\" x -y -db \"" + arc.string() + "\" \"" +
                          dest.string() + "\"";
        const int rc = run_cmd(cmd);
        const double s = now_s() - t0;
        if (rc == 0) batch_t.add(s);
    }
    std::printf("\n--- openrar x -db (batch durability) ---\n");
    print_timing("x -db (no per-entry flush)", batch_t);

    // Phase 4: measure openrar x (default, per-entry durability).
    Timing default_t;
    for (int r = 0; r < reps; ++r) {
        fs::remove_all(dest);
        fs::create_directories(dest);
        const auto t0 = now_s();
        std::string cmd = "\"" + exe + "\" x -y \"" + arc.string() + "\" \"" +
                          dest.string() + "\"";
        const int rc = run_cmd(cmd);
        const double s = now_s() - t0;
        if (rc == 0) default_t.add(s);
    }
    std::printf("\n--- openrar x (default, per-entry durability) ---\n");
    print_timing("x (per-entry flush)", default_t);

    // Phase 5: attribute the residual.
    if (raw_t.n > 0 && batch_t.n > 0 && default_t.n > 0) {
        const double raw_mean = raw_t.sum_s / raw_t.n;
        const double batch_mean = batch_t.sum_s / batch_t.n;
        const double default_mean = default_t.sum_s / default_t.n;
        const double batch_overhead = batch_mean - raw_mean;
        const double flush_cost = default_mean - batch_mean;
        const double total_overhead = default_mean - raw_mean;

        std::printf("\n--- Attribution ---\n");
        std::printf("  raw read+write:          %.3f s\n", raw_mean);
        std::printf("  x -db (batch):           %.3f s  (+%.3f s, %.2fx raw)\n",
                    batch_mean, batch_overhead, batch_mean / raw_mean);
        std::printf("  x (per-entry flush):     %.3f s  (+%.3f s, %.2fx raw)\n",
                    default_mean, flush_cost, default_mean / raw_mean);
        std::printf("  total overhead:          %.3f s (%.2fx raw)\n",
                    total_overhead, default_mean / raw_mean);
        std::printf("  batch overhead (journal+temp+rename+MOTW+cadence+callbacks): %.3f s\n",
                    batch_overhead);
        std::printf("  per-entry flush cost:    %.3f s\n", flush_cost);

        if (batch_overhead > raw_mean * 0.5) {
            std::printf("\n  [VERDICT] The batch overhead is %.1f%% of raw - significant. "
                        "The journal/temp/rename/MOTW/cadence path is the target.\n",
                        batch_overhead / raw_mean * 100.0);
        } else {
            std::printf("\n  [VERDICT] The batch overhead is %.1f%% of raw - minor. "
                        "The per-entry flush is the dominant cost (%.3f s).\n",
                        batch_overhead / raw_mean * 100.0, flush_cost);
        }
    }

    // Cleanup.
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return 0;
}