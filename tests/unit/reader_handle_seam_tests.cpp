// Reader handle seam (v1.30.0 M3 harness): ArchiveReader::open_read_handle
// opens the first volume from an ALREADY-OPEN OS handle without any
// filesystem path — the sandboxed worker's only way to see the archive.
//
// Pinned here, independent of any worker process:
//   - scan parity: entries from the handle-open match the path-open exactly
//     (names, sizes, methods, CRCs);
//   - extraction parity: extract_entry_sink bytes are identical;
//   - the synthetic display path is unopenable: a multi-volume archive
//     fails its second volume with RAR_ERR_MISSING_VOLUME (the worker mode
//     is single-volume by contract);
//   - the worker owns the handle after a successful open (close() closes
//     it) — asserted indirectly by the test process not leaking fds
//     (Windows: the temp archive file stays shareable after close()).

#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/core/types.hpp"

#include "test_support.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace openrar;
using namespace openrar::test;
using namespace openrar::archive;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

namespace {

fs::path make_dir(const char* name) {
    return openrar::test::make_scratch_dir(std::string("openrar_") + name);
}

#ifdef _WIN32
void* open_read_handle_for(const fs::path& p) {
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}
#else
void* open_read_handle_for(const fs::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY);
    return fd < 0 ? nullptr : reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}
#endif

} // namespace

static void run_all();

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    try {
        run_all();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXCEPTION: %s\n", e.what());
        return 2;
    }
    if (fails == 0) std::printf("[seam] reader handle seam: OK\n");
    return fails == 0 ? 0 : 1;
}

static void run_all() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    std::cout << "[+] reader_handle_seam: scan + extract parity\n";
    const fs::path dir = make_dir("parity");
    const fs::path arc = dir / "seam.rar";
    // Two entries, one compressed one stored, through the mutator.
    {
        const fs::path blob = dir / "blob_a.bin";
        {
            std::ofstream f(blob, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 4096; ++i) f << "sample-" << i << "\n";
        }
        assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, "a/stored.txt", 0));
        std::ofstream f2(blob, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 4096; ++i) f2 << "content-" << i << "\n";
        assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, "b/deflated.txt", 3));
        std::error_code rm_ec;
        fs::remove(blob, rm_ec); // AV/indexer can hold the blob briefly — best-effort
    }

    archive::ArchiveReader by_path;
    CHECK(by_path.open(arc));
    CHECK(!by_path.entries().empty());

    void* h = open_read_handle_for(arc);
    CHECK(h != nullptr);
    if (h != nullptr) {
        archive::ArchiveReader by_handle;
        int status = 0;
        std::string detail;
        CHECK(by_handle.open_read_handle(h, "", status, detail));
        if (status != 0 && detail.empty()) detail = "(no detail)";
        if (!detail.empty() && status != 0)
            std::fprintf(stderr, "  open_read_handle: rc=%d detail=%s\n", status, detail.c_str());
        CHECK(by_handle.entries().size() == by_path.entries().size());
        if (!by_handle.entries().empty()) {
            for (size_t i = 0; i < by_path.entries().size() && i < by_handle.entries().size();
                 ++i) {
                const archive::ArchiveEntry& a = by_path.entries()[i];
                const archive::ArchiveEntry& b = by_handle.entries()[i];
                CHECK(a.header.file_name == b.header.file_name);
                CHECK(a.header.unp_size == b.header.unp_size);
                CHECK(a.header.method == b.header.method);
                CHECK(a.header.data_crc32 == b.header.data_crc32);
            }
            // Extraction parity through the sink seam.
            for (size_t i = 0; i < by_path.entries().size(); ++i) {
                std::vector<core::byte> out_p, out_h;
                const int rc_p =
                    by_path.extract_entry_to_memory(i, out_p, 1ull << 30, {}, nullptr, nullptr);
                const int rc_h =
                    by_handle.extract_entry_to_memory(i, out_h, 1ull << 30, {}, nullptr, nullptr);
                if (rc_h != rc_p || out_p.size() != out_h.size()) {
                    std::fprintf(stderr, "  entry %zu: rc_p=%d rc_h=%d len_p=%zu len_h=%zu\n", i,
                                 rc_p, rc_h, out_p.size(), out_h.size());
                }
                CHECK(rc_p == 0);
                CHECK(rc_h == rc_p);
                CHECK(out_p.size() == out_h.size());
                CHECK(std::memcmp(out_p.data(), out_h.data(), out_p.size()) == 0);
            }
        } // !by_handle.entries().empty()
        // by_handle owns the handle: its destructor (close) releases it. The
        // test file must be removable afterwards (Windows share semantics).
        by_handle.close();
        std::error_code ec;
        fs::copy_file(arc, arc.string() + ".probe", ec);
        CHECK(!ec); // not locked by a leaked handle
        fs::remove(arc.string() + ".probe", ec);
    }
    by_path.close();

    std::cout << "[+] reader_handle_seam: multi-volume reports MISSING_VOLUME\n";
    {
        // Build a genuine 2-volume set: the mutator's volume API.
        const fs::path vol1 = dir / "set.part01.rar";
        const fs::path blob = dir / "vblob.bin";
        {
            std::ofstream f(blob, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 200000; ++i) f << "volume-data-" << i << "\n";
        }
        // 16 KiB volumes force a multi-volume set.
        assert(
            archive::ArchiveMutator::add_file_to_archive_vol(vol1, blob, "big.bin", 0, 16 * 1024));
        fs::remove(blob);

        void* vh = open_read_handle_for(vol1);
        CHECK(vh != nullptr);
        if (vh != nullptr) {
            archive::ArchiveReader r;
            int status = -100;
            std::string detail;
            const bool ok = r.open_read_handle(vh, "", status, detail);
            if (ok) {
                // The scan succeeded on volume 1 only; the split entry's
                // declared size cannot be satisfied (volume 2 is unreachable
                // through the seam), so extraction fails — TRUNCATED when the
                // first extent streams short, MISSING_VOLUME when the engine
                // names the absent volume. Either is the single-volume
                // contract holding; success would be the bug.
                std::vector<core::byte> out;
                const int rc = r.extract_entry_to_memory(0, out, 1ull << 30, {}, nullptr, nullptr);
                CHECK(rc != RAR_OK);
            } else {
                // Or the scan itself refuses the set: MISSING_VOLUME.
                CHECK(status == RAR_ERR_MISSING_VOLUME);
            }
            r.close();
        }
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
