// v1.40 quick-win microprobe (not a test): prices the individual per-entry
// extraction operations targeted by the extraction quick-win scorecard, so
// the scorecard rests on measurement rather than syscall-count guesses.
//
// Measured operations:
//   1. lexical parent-chain walk (exact replica of archive_reader's
//      has_symlink_parent) at several extraction-root depths
//   2. DuplicateHandle + CloseHandle (the containment cache-hit cost)
//   3. ContainmentRoot::leaf_is_reparse with the leaf present and absent
//   4. ContainmentRoot::resolve_dir: cold component walk vs cache hit
//
// Output is a table; interpretation lives in the scorecard.

#include "../src/io/containment.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace openrar;
namespace fs = std::filesystem;

namespace {

double now_us() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Row {
    const char* label;
    long iters;
    double total_ms;
};

// Replica of src/archive/archive_reader.cpp is_reparse_or_symlink /
// has_symlink_parent (the real one lives in an anonymous namespace).
bool probe_is_reparse_or_symlink(const fs::path& p) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(p.wstring().c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    return !ec && fs::is_symlink(st);
}

bool probe_has_symlink_parent(const fs::path& dest) {
    fs::path parent = dest.parent_path();
    for (auto p = parent; !p.empty(); p = p.parent_path()) {
        if (p == p.root_path() || p.parent_path() == p) break;
        if (p.is_absolute() && p.parent_path() == p.root_path()) break;
        if (probe_is_reparse_or_symlink(p)) return true;
    }
    return false;
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "openrar_microprobe";
    std::error_code ec;
    fs::remove_all(base, ec);

    // Deep existing chain: base/a0/a1/.../a13/leaf.txt — extraction roots in
    // the harness live under tools/perf/data/... (similar depth).
    fs::path deep = base;
    for (int i = 0; i < 14; ++i) deep /= ("a" + std::to_string(i));
    fs::create_directories(deep, ec);
    const fs::path leaf = deep / "leaf.txt";
    {
        std::FILE* f = std::fopen(leaf.string().c_str(), "wb");
        if (f) {
            std::fputs("x", f);
            std::fclose(f);
        }
    }

    std::vector<Row> rows;

    // 1. parent-chain walk at depth 14 (dest = deep/leaf.txt)
    {
        const long iters = 2000;
        double t0 = now_us();
        long hits = 0;
        for (long i = 0; i < iters; ++i) hits += probe_has_symlink_parent(leaf) ? 1 : 0;
        rows.push_back(
            {"has_symlink_parent depth14 (hit-if-link)", iters, (now_us() - t0) / 1000.0});
        if (hits != 0) {
            std::printf("unexpected symlink in chain\n");
            return 1;
        }
    }

#ifdef _WIN32
    // 2. DuplicateHandle + CloseHandle (cache-hit cost)
    {
        HANDLE h = CreateFileW(base.wstring().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("probe: createfile failed\n");
            return 1;
        }
        const long iters = 20000;
        double t0 = now_us();
        for (long i = 0; i < iters; ++i) {
            void* dup = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS)) {
                std::printf("probe: DuplicateHandle failed\n");
                return 1;
            }
            CloseHandle(static_cast<HANDLE>(dup));
        }
        rows.push_back({"DuplicateHandle+Close (cache hit)", iters, (now_us() - t0) / 1000.0});
        CloseHandle(h);
    }
#endif

    // 3/4. ContainmentRoot operations
    io::ContainmentRoot root;
    if (!root.attach(base)) {
        std::printf("probe: attach failed\n");
        return 1;
    }

    // 3a. leaf_is_reparse, leaf present (regular file)
    {
        io::ContainmentRoot::VerifiedDir dir;
        if (!root.resolve_dir("a0/a1/a2/a3/a4/a5/a6/a7/a8/a9/a10/a11/a12/a13", true, dir)) {
            std::printf("probe: resolve_dir failed\n");
            return 1;
        }
        const long iters = 20000;
        double t0 = now_us();
        bool present_out = false;
        for (long i = 0; i < iters; ++i) {
            bool out = false;
            root.leaf_is_reparse(dir, "leaf.txt", out);
            present_out = out;
        }
        rows.push_back({"leaf_is_reparse leaf-present", iters, (now_us() - t0) / 1000.0});
        if (present_out) {
            std::printf("unexpected reparse leaf\n");
            return 1;
        }
    }

    // 3b. leaf_is_reparse, leaf absent (fresh destination — the common
    // first-extraction case)
    {
        io::ContainmentRoot::VerifiedDir dir;
        root.resolve_dir("a0/a1/a2/a3/a4/a5/a6/a7/a8/a9/a10/a11/a12/a13", true, dir);
        const long iters = 20000;
        double t0 = now_us();
        for (long i = 0; i < iters; ++i) {
            bool out = false;
            root.leaf_is_reparse(dir, "definitely_absent.txt", out);
        }
        rows.push_back({"leaf_is_reparse leaf-absent", iters, (now_us() - t0) / 1000.0});
    }

    // 4a. resolve_dir cold: 512 distinct pre-created directories, one miss each
    {
        std::vector<std::string> rels;
        const int n = 512;
        for (int i = 0; i < n; ++i) {
            std::string rel = "c" + std::to_string(i / 32) + "/d" + std::to_string(i % 32);
            fs::create_directories(base / rel, ec);
            rels.push_back(rel);
        }
        double t0 = now_us();
        for (const std::string& r : rels) {
            io::ContainmentRoot::VerifiedDir dir;
            if (!root.resolve_dir(r, false, dir)) {
                std::printf("probe: cold resolve %s\n", r.c_str());
                return 1;
            }
        }
        rows.push_back({"resolve_dir cold (2-level walk)", n, (now_us() - t0) / 1000.0});
    }

    // 4b. resolve_dir warm: the same directory over and over (cache hit)
    {
        const long iters = 20000;
        double t0 = now_us();
        for (long i = 0; i < iters; ++i) {
            io::ContainmentRoot::VerifiedDir dir;
            if (!root.resolve_dir("c0/d0", false, dir)) {
                std::printf("probe: warm resolve\n");
                return 1;
            }
        }
        rows.push_back({"resolve_dir warm (cache hit)", iters, (now_us() - t0) / 1000.0});
    }

    std::printf("\n%-46s %9s %12s %14s\n", "operation", "iters", "total_ms", "us_per_op");
    std::printf("%s\n", std::string(84, '-').c_str());
    for (const Row& r : rows) {
        std::printf("%-46s %9ld %12.3f %14.3f\n", r.label, r.iters, r.total_ms,
                    r.total_ms * 1000.0 / r.iters);
    }

    fs::remove_all(base, ec);
    return 0;
}
