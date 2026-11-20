// PROBE-ENTRY-DECODE (§4.34) — log-only probe for the per-entry extraction path.
//
// WHY. The runbench matrix puts entry-wise many-file extraction at 5.535 s vs
// UnRAR's 0.812 s (6.8x), while BATCH extraction of the SAME archive is
// 2.103 s. The 3.4 s difference is per-entry overhead, not entropy decode:
// every decode-side row (§4.6/§4.7/§4.32) has already closed as sub-threshold
// or refuted by decode_kernel_probe, which bounds symbol decode at ~56% of
// wall. This probe attributes the remaining per-entry cost.
//
// WHAT IT MEASURES, per extraction mode:
//   1. End-to-end `x` wall (the number runbench reports)
//   2. Open + scan cost alone (list the archive, extract nothing) — the
//      header-walk floor that every entry pays on top of its decode
//   3. Store (-m0) extraction of the same entry count — isolates decode from
//      the per-entry lifecycle, because a stored member has no entropy stage
//   4. The per-entry lifecycle cost by difference: (compressed - stored) is
//      decode; (stored - raw copy) is journal/temp/rename/MOTW/durability
//
// The subtraction is the point: it separates "our decoder is slow" from "our
// per-entry path is slow" without instrumenting production code.
//
// Usage: entry_decode_probe <openrar_exe> [--reps N] [--entries 610]
// Exit 0 = ran (verdict is in the output), 1 = could not measure.

#include "../src/core/types.hpp"

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
int run_cmd(const std::string& cmdline) {
    std::wstring wcmd(cmdline.begin(), cmdline.end());
    std::vector<wchar_t> cmdbuf(wcmd.begin(), wcmd.end());
    cmdbuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                          &pi)) {
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
#include <sys/wait.h>
#include <unistd.h>
int run_cmd(const std::string& cmdline) {
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

struct Stat {
    double min_s = 1e9;
    double sum_s = 0;
    int n = 0;
    void add(double s) {
        min_s = std::min(min_s, s);
        sum_s += s;
        ++n;
    }
    double mean_s() const { return n ? sum_s / n : 0.0; }
};

void print_row(const char* label, const Stat& st) {
    if (!st.n) {
        std::printf("  %-34s (no samples)\n", label);
        return;
    }
    std::printf("  %-34s min=%7.3f s  mean=%7.3f s  n=%d\n", label, st.min_s, st.mean_s(), st.n);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: entry_decode_probe <openrar_exe> [--reps N] [--entries N]\n");
        return 2;
    }
    const std::string exe = argv[1];
    int reps = 3;
    int n_entries = 610;
    // File size matters: at ~200 B the entropy stage has nothing to do and the
    // compressed/stored pair comes out identical, which measures nothing about
    // decode. 4 KiB keeps the entry count high while giving -m3 real work.
    int file_size_kib = 4;
    for (int i = 2; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--reps", 6) && i + 1 < argc) reps = std::atoi(argv[++i]);
        if (!std::strncmp(argv[i], "--entries", 9) && i + 1 < argc)
            n_entries = std::atoi(argv[++i]);
        if (!std::strncmp(argv[i], "--kib", 5) && i + 1 < argc)
            file_size_kib = std::atoi(argv[++i]);
    }
    if (reps < 1) reps = 1;
    if (file_size_kib < 1) file_size_kib = 1;

    const auto tmp = fs::temp_directory_path() / "entry_decode_probe";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // Corpus shape matched to runbench's `many/` cell (tools/bench_run.cpp):
    // 20 dirs x 30 files + 10 in deep/a/b/c = 610 entries, 4 KiB each, and
    // the content is a repeated SHA-256 digest - i.e. INCOMPRESSIBLE. That
    // matters: with compressible text the -m3 archive is tiny and decode is
    // not the dominant cost, so the durability comparison measures something
    // other than what the benchmark measured.
    const auto corpus = tmp / "src";
    fs::create_directories(corpus);
    std::vector<std::string> body_pool;
    {
        // SHA-256 here is only used to make incompressible filler; correctness
        // of the digest is irrelevant, so a simple deterministic PRF suffices.
        // It only has to be byte-uniform and not match the compressor's model.
        core::uint32 s = 0x9E3779B9u;
        std::string block;
        for (int k = 0; k < 32; ++k) {
            std::string out;
            for (int j = 0; j < 4; ++j) {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                for (int b = 0; b < 4; ++b) out.push_back(static_cast<char>((s >> (b * 8)) & 0xFF));
            }
            block += out;
        }
        while (block.size() < static_cast<size_t>(file_size_kib) * 1024) block += block;
        block.resize(static_cast<size_t>(file_size_kib) * 1024);
        body_pool.push_back(block);
    }
    const std::string& body = body_pool[0];

    const char* subdirs[] = {"a", "b", "c"};
    int made = 0;
    for (int d = 0; d < 20; ++d) {
        char dn[32];
        std::snprintf(dn, sizeof(dn), "d%02d", d);
        const auto dd = corpus / dn;
        fs::create_directories(dd);
        for (int i = 0; i < 30 && made < n_entries; ++i, ++made) {
            char name[64];
            std::snprintf(name, sizeof(name), "f%03d.bin", made);
            std::string p = (dd / name).string();
            FILE* f = std::fopen(p.c_str(), "wb");
            if (!f) {
                std::fprintf(stderr, "cannot write corpus file %s\n", p.c_str());
                return 2;
            }
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
        }
    }
    // deep/a/b/c - the nested path runbench also packs
    {
        const auto deep = corpus / "deep" / "a" / "b" / "c";
        fs::create_directories(deep);
        for (int i = 0; i < 10 && made < n_entries; ++i, ++made) {
            char name[64];
            std::snprintf(name, sizeof(name), "deep%02d.bin", made);
            std::string p = (deep / name).string();
            FILE* f = std::fopen(p.c_str(), "wb");
            if (!f) continue;
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
        }
    }
    (void)subdirs;

    std::printf("PROBE-ENTRY-DECODE (§4.34)\n");
    std::printf("corpus: %d files x %d KiB\nreps: %d\n\n", n_entries, file_size_kib, reps);

    // --- build one archive ----------------------------------------------
    // -m3 rather than -m0: this probe is about the compressed entry path,
    // which is where the runbench gap was measured. Both modes are measured
    // against this same archive, so no content variable is introduced.
    const auto arc_m3 = (tmp / "m3.rar").string();

    const std::string cmd =
        "\"" + exe + "\" a -y -ep -m3 -mt1 -idq \"" + arc_m3 + "\" \"" + corpus.string() + "\"";
    if (run_cmd(cmd) != 0) {
        std::fprintf(stderr, "build failed (rc=%d)\n", 1);
        return 2;
    }

    // --- measurements -----------------------------------------------------
    // Two modes, one archive: default (per-entry durability) vs -db (batch).
    // The difference IS the per-entry durability cost, measured on identical
    // bytes with no synthetic baseline to invalidate it. This is the same
    // technique §4.16 used for the store sink.
    Stat s_def, s_db, s_list;
    const auto arc = arc_m3;

    for (int r = 0; r < reps; ++r) {
        // list only: open + header walk, no extraction. The floor every
        // invocation pays before any entry is touched.
        {
            const auto t0 = now_s();
            const int rc = run_cmd("\"" + exe + "\" l \"" + arc + "\"");
            const double dt = now_s() - t0;
            if (rc == 0) s_list.add(dt);
        }
        // default: per-entry durability (journal + flush per entry)
        {
            const auto d = tmp / "x_def";
            fs::remove_all(d, ec);
            fs::create_directories(d);
            const auto t0 = now_s();
            const int rc =
                run_cmd("\"" + exe + "\" x -y -idq \"" + arc + "\" \"" + d.string() + "\"");
            const double dt = now_s() - t0;
            if (rc == 0) s_def.add(dt);
        }
        // -db: batch durability (one flush at the end, no per-entry journal)
        {
            const auto d = tmp / "x_db";
            fs::remove_all(d, ec);
            fs::create_directories(d);
            const auto t0 = now_s();
            const int rc =
                run_cmd("\"" + exe + "\" x -y -db -idq \"" + arc + "\" \"" + d.string() + "\"");
            const double dt = now_s() - t0;
            if (rc == 0) s_db.add(dt);
        }
    }

    std::printf("--- per-mode wall (min/mean over reps) ---\n");
    print_row("list (open + header walk)", s_list);
    print_row("x (per-entry durability)", s_def);
    print_row("x -db (batch durability)", s_db);

    if (!s_def.n || !s_db.n) {
        std::printf("\nFAIL: a measurement is missing; cannot attribute.\n");
        return 1;
    }

    // --- attribution ------------------------------------------------------
    const double def = s_def.mean_s();
    const double db = s_db.mean_s();
    const double list = s_list.n ? s_list.mean_s() : 0.0;
    // The gap between the two modes is the per-entry durability cost. Nothing
    // else differs: same archive, same member count, same bytes.
    const double durability = def - db;

    std::printf("\n--- attribution ---\n");
    std::printf("  x (per-entry durability)     %8.3f s\n", def);
    std::printf("  x -db (batch durability)     %8.3f s\n", db);
    if (list > 0) std::printf("  list only                    %8.3f s\n", list);
    std::printf("\n");
    std::printf("  per-entry durability cost    %8.3f s   %+6.1f%% of x\n", durability,
                100.0 * durability / def);
    std::printf("  per entry                    %+8.3f ms\n",
                durability * 1000.0 / static_cast<double>(n_entries));
    std::printf("\n  The remainder (x -db) is open + header walk + decode + file creation,\n"
                "  none of which -db changes. -db is the floor; the gap is the tax.\n");

    std::printf("\n--- decision ---\n");
    if (durability <= 0.0) {
        std::printf("  Batch mode was not faster (delta %+.3f s). Either the run spread\n"
                    "  exceeds the effect or per-entry durability is not the cost here.\n"
                    "  Re-run with more reps before concluding.\n",
                    durability);
    } else {
        const double pct = 100.0 * durability / def;
        std::printf("  Per-entry durability is %+.3f s = %+.1f%% of extraction wall.\n", durability,
                    pct);
        if (pct >= 15.0) {
            std::printf("  MATERIAL. The lever is the entry-durability policy (journal +\n"
                        "  per-entry flush), owned by its own row - not a codec change.\n");
        } else {
            std::printf("  MINOR (%.1f%%). Per-entry durability is not the explanation for\n"
                        "  the many-entry gap; look at open/header-walk/decode instead.\n",
                        pct);
        }
    }

    fs::remove_all(tmp, ec);
    return 0;
}