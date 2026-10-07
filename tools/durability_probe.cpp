// v1.39.0 Gate 0 probe (not a test): durability cost decomposition for the
// extraction pipeline (docs/v1.39.0-pre-analysis.md).
//
// Prices every FlushFileBuffers-class component the v1.24 contract pays per
// entry so the revised crash-consistency contract is designed against
// measurement, not guesswork:
//   - temp write + FlushFileBuffers before rename (per entry, all sizes)
//   - flush-through-renamed-handle (the batch-boundary shape)
//   - per-entry journal lifecycle: sweep rescan + create + header sync +
//     record append + sync + close + unlink
//   - end-to-end pipelines over the REAL mechanism: today's entry-granularity
//     AtomicWriter loop vs a batched-replica loop (journal lifetime + batched
//     record syncs + no per-temp flush), small-file and big-file shapes.
//
// Output is a table; interpretation lives in the pre-analysis document.

#include "../src/io/extraction_journal.hpp"
#include "../src/io/file_stream.hpp"
#include "../src/io/path_util.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace openrar;

namespace fs = std::filesystem;

namespace {

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[noreturn]] void die(const char* what) {
#ifdef _WIN32
    std::fprintf(stderr, "probe: %s failed (GLE=%lu)\n", what, GetLastError());
#else
    std::fprintf(stderr, "probe: %s failed (errno=%d)\n", what, errno);
#endif
    std::exit(1);
}

std::string size_label(size_t size) {
    char buf[32];
    if (size >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%zu MiB", size / (1024 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%zu KiB", size / 1024);
    }
    return buf;
}

void make_file(const fs::path& p, size_t size) {
    io::FileStream f;
    if (!f.open(p, io::FileMode::CreateNew)) {
        std::fprintf(stderr, "probe: cannot create %s\n", io::u8_str(p).c_str());
        std::exit(1);
    }
    static core::byte buf[65536];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<core::byte>(i * 31 + i / 251);
    size_t put = 0;
    while (put < size) {
        const size_t take = std::min(size - put, sizeof(buf));
        if (f.write(buf, take) != take) {
            std::fprintf(stderr, "probe: short write\n");
            std::exit(1);
        }
        put += take;
    }
    f.close();
}

void rm_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// Renames an OPEN stream to `dest` without flushing (the batch-mode commit
// shape): FileRenameInformationEx through the handle anchored to the verified
// parent directory — the same cascade FileStream::commit_rename uses, minus
// the flush. rename() on POSIX. The handle stays open for the post-rename
// flush.
bool rename_open_no_flush(io::FileStream& f, const fs::path& dest) {
#ifdef _WIN32
    // NtSetInformationFile(FileRenameInformationEx) — the containment commit
    // path's call. (Gate 0 en-route finding: SetFileInformationByHandle class
    // 13 fails with ERROR_INVALID_PARAMETER on this host's NTFS, so the
    // kernel32 form cannot be used to price rename-through-handle here.)
    HANDLE parent = CreateFileW(dest.parent_path().wstring().c_str(),
                                FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parent == INVALID_HANDLE_VALUE) return false;
    const std::wstring leaf = dest.filename().wstring();
    struct RenameInfo {
        DWORD Flags;
        HANDLE RootDirectory;
        DWORD FileNameLength;
        WCHAR FileName[1];
    };
    std::vector<BYTE> buf(sizeof(RenameInfo) + leaf.size() * sizeof(WCHAR));
    auto* ri = reinterpret_cast<RenameInfo*>(buf.data());
    ri->Flags = 0x00000002 /* POSIX semantics */ | 0x00000001 /* replace */;
    ri->RootDirectory = parent;
    ri->FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(WCHAR));
    std::memcpy(ri->FileName, leaf.c_str(), (leaf.size() + 1) * sizeof(WCHAR));
    using NtSetInfoFn = LONG(NTAPI*)(HANDLE, PVOID, PVOID, ULONG, ULONG);
    static NtSetInfoFn set_info = []() -> NtSetInfoFn {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtSetInfoFn>(
                           static_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")))
                     : nullptr;
    }();
    if (set_info == nullptr) {
        CloseHandle(parent);
        return false;
    }
    struct {
        LONG Status;
        ULONG_PTR Information;
    } iosb{0, 0};
    const LONG st = set_info(static_cast<HANDLE>(f.os_handle()), &iosb, ri,
                             static_cast<ULONG>(buf.size()), 65 /* FileRenameInformationEx */);
    CloseHandle(parent);
    return st == 0;
#else
    return ::rename(io::u8_str(f.path()).c_str(), io::u8_str(dest).c_str()) == 0;
#endif
}

// One create+write+rename cycle of `size` bytes. flush_mode: 0 = none (batch
// ceiling), 1 = FlushFileBuffers before rename (today's contract), 2 = flush
// AFTER the rename through the still-open handle (the batch-boundary shape).
double pipeline_once(const fs::path& dir, size_t size, int flush_mode) {
    const fs::path dest = dir / "pl.bin";
    std::error_code ec;
    fs::remove(dest, ec);
    static core::byte buf[8192];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<core::byte>(i * 7 + i / 253);

    const double t0 = now_s();
    io::FileStream f;
    if (!f.open(dest, io::FileMode::CreateNew)) die("CreateNew temp");
    size_t put = 0;
    while (put < size) {
        const size_t take = std::min(size - put, sizeof(buf));
        if (f.write(buf, take) != take) die("payload write");
        put += take;
    }
    if (flush_mode == 1 && !f.flush()) std::exit(1);
    if (!rename_open_no_flush(f, dir / "pl2.bin")) die("no-flush rename");
    if (flush_mode == 2 && !f.flush()) std::exit(1);
    f.close();
    fs::remove(dir / "pl2.bin", ec);
    return now_s() - t0;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void bench(const char* name, int iters, const std::function<double()>& fn) {
    // 1 untimed warm-up (page-cache, metadata state), then `iters` timed
    // samples; median and min reported.
    fn();
    std::vector<double> samples;
    for (int i = 0; i < iters; ++i) samples.push_back(fn());
    std::printf("  %-52s median %8.3f ms   min %8.3f ms   (n=%d)\n", name, median(samples) * 1e3,
                *std::min_element(samples.begin(), samples.end()) * 1e3, iters);
}

// Real-mechanism entry-granularity pipeline: one ExtractionSession, one
// AtomicWriter per file, open/write/commit — exactly what the reader's write
// paths do per entry today.
double entry_pipeline(const fs::path& dir, const std::string& stem_prefix, int count, size_t size) {
    io::ExtractionSession session;
    static core::byte buf[8192];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<core::byte>(i * 7 + i / 253);
    const double t0 = now_s();
    for (int i = 0; i < count; ++i) {
        io::AtomicWriter writer;
        const fs::path dest = dir / (stem_prefix + std::to_string(i) + ".bin");
        if (!writer.open(session, dest)) std::exit(1);
        size_t put = 0;
        while (put < size) {
            const size_t take = std::min(size - put, sizeof(buf));
            if (writer.stream().write(buf, take) != take) std::exit(1);
            put += take;
        }
        if (!writer.commit()) std::exit(1);
    }
    return now_s() - t0;
}

// Appends `data` to an open journal stream (seek to end, write).
void journal_append(io::FileStream& j, const std::string& data) {
    j.seek(0, io::SeekOrigin::End);
    if (j.write(data.data(), data.size()) != data.size()) std::exit(1);
}

// Batched-replica pipeline (the ceiling shape): one journal for the whole run
// (no per-entry lifecycle churn, no per-entry sweep rescan), records appended
// WITHOUT per-record sync and synced once per window of 32, no per-temp flush,
// rename per entry through the open handle.
double batch_replica_pipeline(const fs::path& dir, const std::string& stem_prefix, int count,
                              size_t size) {
    const double t0 = now_s();
    const fs::path jpath = dir / ".openrar_journal_probe_replica.tmp";
    io::FileStream journal;
    if (!journal.open(jpath, io::FileMode::CreateNew)) std::exit(1);
    journal_append(journal, "OPENRAR-JOURNAL 1\n");
    journal.flush();
    static core::byte buf[8192];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<core::byte>(i * 7 + i / 253);
    int since_sync = 0;
    for (int i = 0; i < count; ++i) {
        const fs::path dest = dir / (stem_prefix + std::to_string(i) + ".bin");
        const fs::path temp = dir / (stem_prefix + std::to_string(i) + ".replica.tmp");
        // Record (unsynced): same wire shape the sweep validates (bogus CRC —
        // the sweep would stop at it; this replica prices cost, not semantics).
        const std::string pb = io::u8_str(temp);
        journal_append(journal, "T " + std::to_string(pb.size()) + " 00000000\n" + pb);
        if (++since_sync >= 32) {
            journal.flush();
            since_sync = 0;
        }
        io::FileStream out;
        if (!out.open(temp, io::FileMode::CreateNew)) std::exit(1);
        size_t put = 0;
        while (put < size) {
            const size_t take = std::min(size - put, sizeof(buf));
            if (out.write(buf, take) != take) std::exit(1);
            put += take;
        }
        if (!rename_open_no_flush(out, dest)) die("no-flush rename");
        out.close();
    }
    journal.flush();
    journal.close();
    std::error_code ec;
    fs::remove(jpath, ec);
    return now_s() - t0;
}

void probe(const char* label, int count, size_t size) {
    std::printf("[pipeline] %s: %d x %s\n", label, count, size_label(size).c_str());
    const fs::path dir = fs::temp_directory_path() / "openrar_dur_probe_pipe";
    rm_tree(dir);
    fs::create_directories(dir);
    entry_pipeline(dir, "warm", 1, size); // warm-up (page cache, metadata state)
    rm_tree(dir);
    fs::create_directories(dir);
    const double entry_s = entry_pipeline(dir, "e", count, size);
    rm_tree(dir);
    fs::create_directories(dir);
    const double batch_s = batch_replica_pipeline(dir, "b", count, size);
    rm_tree(dir);
    const double mb = static_cast<double>(count) * size / (1024.0 * 1024.0);
    std::printf("  entry-granularity (today):   %7.3f s   %8.1f MiB/s\n", entry_s, mb / entry_s);
    std::printf("  batched replica (ceiling):  %7.3f s   %8.1f MiB/s   (%.2fx)\n", batch_s,
                mb / batch_s, entry_s / batch_s);
    rm_tree(dir);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== durability probe (v1.39.0 Gate 0) ==\n");
    const fs::path root = fs::temp_directory_path() / "openrar_dur_probe";
    rm_tree(root);
    fs::create_directories(root);

    // ── 1. Flush cost vs payload size ───────────────────────────────────────
    struct SizeRow {
        size_t size;
        int iters;
    };
    const SizeRow rows[] = {
        {4 * 1024, 400}, {64 * 1024, 400}, {256 * 1024, 200}, {1 << 20, 100},
        {8 << 20, 30},   {64 << 20, 8},    {256 << 20, 3},
    };
    std::printf("[flush-vs-size] create+write+rename; no flush vs pre-rename vs "
                "post-rename flush\n");
    for (const SizeRow& r : rows) {
        const fs::path dir = root / "flushsize";
        rm_tree(dir);
        fs::create_directories(dir);
        std::vector<double> nof, pre, post;
        pipeline_once(dir, r.size, 0); // warm-up
        for (int i = 0; i < r.iters; ++i) nof.push_back(pipeline_once(dir, r.size, 0));
        for (int i = 0; i < r.iters; ++i) pre.push_back(pipeline_once(dir, r.size, 1));
        for (int i = 0; i < r.iters; ++i) post.push_back(pipeline_once(dir, r.size, 2));
        std::printf("  %-8s noflush %8.3f ms | preflush %8.3f ms | postrename-flush %8.3f ms\n",
                    size_label(r.size).c_str(), median(nof) * 1e3, median(pre) * 1e3,
                    median(post) * 1e3);
        rm_tree(dir);
    }

    // ── 2. Journal component costs ──────────────────────────────────────────
    std::printf("[journal] per-entry lifecycle components\n");
    {
        const fs::path dir = root / "journal";
        fs::create_directories(dir);
        // (a) sweep rescan on a directory with K files — the per-entry rescan
        //     entry-granularity pays whenever it holds no journal.
        for (const int k : {1, 100, 2000}) {
            const fs::path d = dir / ("k" + std::to_string(k));
            fs::create_directories(d);
            for (int i = 0; i < k; ++i) make_file(d / ("f" + std::to_string(i) + ".bin"), 16);
            std::printf("  sweep rescan, dir has %4d files:", k);
            bench("", 20, [d] {
                const double t = now_s();
                io::ExtractionSession::sweep_directory(d);
                return now_s() - t;
            });
        }
        // (b) the full per-entry journal lifecycle the entry mode pays:
        //     sweep + journal create + header sync + record append + sync +
        //     close + unlink (open + abandon drives exactly that cycle).
        io::ExtractionSession s;
        bench("AtomicWriter open+abandon cycle (full lifecycle)", 200, [dir, &s] {
            const double t = now_s();
            io::AtomicWriter w2;
            if (!w2.open(s, dir / "rec2.txt")) std::exit(1);
            w2.abandon();
            return now_s() - t;
        });
    }

    // ── 3. End-to-end pipelines through the real mechanism ──────────────────
    probe("many small files", 2000, 16 * 1024);
    probe("many small files", 500, 128 * 1024);
    probe("single big file", 1, 64 << 20);
    probe("few big files", 4, 32 << 20);

    rm_tree(root);
    std::printf("== probe done ==\n");
    return 0;
}
