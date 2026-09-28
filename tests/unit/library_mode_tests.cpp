// Library-mode containment gates (v1.30.0 M2; SECURITY_ARCHITECTURE §5.1,
// docs/abi-freeze.md §8). Pins the two-layer limit split at the C-ABI
// surface:
//
//   library_mode_limits_non_disableable
//     - the engine floors are compile-time constants with NO embedder-facing
//       setter (static_assert on MAX_STREAM_OUTPUT; the KDF lg2_count
//       ceiling refuses a hostile entry even with every caller limit set to
//       UINT64_MAX);
//     - the caller-budget layer (ExtractionLimits / set_limits) IS tunable —
//       tiny limits fire RAR_ERR_LIMIT_EXCEEDED, unlimited passes — which is
//       exactly why the floors must not be confused with it.
//
//   dll_destinations_are_caller_owned
//     - archive entry NAMES are inert at the C-ABI extraction surface: an
//       entry named "../lm_escape.txt" extracted via
//       openrar_archive_handle_extract_to_path writes ONLY the caller-named
//       destination — the DLL has no archive-name-driven write path.
//
//   library_containment_no_disable_switch
//     - the §4.1 path-containment walk has no disable switch: a midpath
//       junction adversary is refused identically with and without
//       OPENRAR_NO_MMAP=1 (the mmap kill-switch downgrades the read engine,
//       never containment), and nothing is written through the link.

#include <openrar/openrar_dll.h>

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/core/types.hpp"
#include "../../src/format/header_reader.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/headers.hpp"
#include "../../src/io/containment.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/win32_meta.hpp"

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
#include <cstdlib>
static void set_env(const char* name, const char* value) {
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
}
static void unset_env(const char* name) {
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
}
#else
#include <cstdlib>
#include <unistd.h>
static void set_env(const char* name, const char* value) {
    setenv(name, value, 1);
}
static void unset_env(const char* name) {
    unsetenv(name);
}
#endif

using namespace openrar;
using namespace openrar::test;
namespace fs = std::filesystem;

// The floors are constants: no API, flag, or environment reaches them.
// Pin the values so an unnoticed change is an ABI-doc event (abi-freeze.md
// §8); on 64-bit native the stream cap is 64 GiB.
static_assert(openrar::compress::Decompressor50::MAX_STREAM_OUTPUT == 64ULL * 1024 * 1024 * 1024,
              "MAX_STREAM_OUTPUT (64-bit native floor) drifted — docs/abi-freeze.md §8");

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

// Minimal single-entry STORED archive: signature + main + file block + data
// + end, written through HeaderWriter so the header CRC is valid.
void craft_stored_archive(const fs::path& arc, const std::string& entry_name,
                          const std::vector<core::byte>& payload) {
    io::FileStream out;
    assert(out.open(arc, io::FileMode::CreateAlways));
    assert(format::HeaderWriter::write_signature(out));
    format::MainBlock mb;
    assert(format::HeaderWriter::write_main_block(out, mb));

    format::FileBlock fb;
    fb.file_name = entry_name;
    fb.unp_size = payload.size();
    fb.pack_size = payload.size();
    fb.method = 0;                        // store
    fb.host_os = 1;                       // unix-ish defaults; no metadata extras
    fb.mtime_win = 133000000000000000ull; // FILETIME (100 ns since 1601)
    assert(format::HeaderWriter::write_file_block(out, fb, 0));
    if (!payload.empty()) assert(out.write(payload.data(), payload.size()) == payload.size());
    format::EndArcBlock eb;
    assert(format::HeaderWriter::write_end_block(out, eb));
}

} // namespace

static void test_set_limits_caller_layer_is_tunable() {
    std::cout << "[+] set_limits_caller_layer_is_tunable\n";
    const fs::path dir = make_dir("limits");
    const fs::path arc = dir / "plain.rar";
    std::vector<core::byte> payload(32);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = core::byte(i * 7u + 1u);
    craft_stored_archive(arc, "ok.txt", payload);

    uint32_t h =
        openrar_archive_open_file(arc.string().c_str(), nullptr, nullptr, nullptr, nullptr);
    CHECK(h != 0);
    if (h == 0) {
        char err[256];
        openrar_archive_get_error(err, sizeof(err));
        std::fprintf(stderr, "  open failed: %s\n", err);
    } else {
        const fs::path dest = dir / "out.bin";

        // Caller budget fires: member limit 1 < 32-byte entry.
        int rc = openrar_archive_handle_set_limits(h, 1, 1, UINT64_MAX, UINT64_MAX);
        CHECK(rc == RAR_OK);
        rc = openrar_archive_handle_extract_to_path(h, 0, dest.string().c_str(), nullptr, nullptr,
                                                    nullptr);
        CHECK(rc == RAR_ERR_LIMIT_EXCEEDED);
        CHECK(!fs::exists(dest));

        // Caller budget released: unlimited passes and the bytes are exact.
        rc = openrar_archive_handle_set_limits(h, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX);
        CHECK(rc == RAR_OK);
        rc = openrar_archive_handle_extract_to_path(h, 0, dest.string().c_str(), nullptr, nullptr,
                                                    nullptr);
        CHECK(rc == RAR_OK);
        CHECK(fs::file_size(dest) == payload.size());
        std::ifstream in(dest, std::ios::binary);
        std::vector<char> got(payload.size());
        in.read(got.data(), got.size());
        CHECK(std::memcmp(got.data(), payload.data(), payload.size()) == 0);

        openrar_archive_close(h);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_kdf_floor_unaffected_by_unlimited_limits() {
    std::cout << "[+] kdf_floor_unaffected_by_unlimited_limits\n";
    const fs::path dir = make_dir("kdffloor");
    const fs::path arc = dir / "hostile_kdf.rar";
    {
        // Hostile entry KDF (lg2_count = 25, one past the pinned ceiling) in
        // a header with a VALID CRC — the ceiling itself must refuse it,
        // independent of any caller budget (the archive_tests.cpp
        // test_kdf_cap_pinned pattern, lifted to the C-ABI surface).
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(format::HeaderWriter::write_signature(out));
        format::MainBlock mb;
        assert(format::HeaderWriter::write_main_block(out, mb));
        format::FileBlock fb;
        fb.file_name = "hostile.bin";
        fb.unp_size = 16;
        fb.pack_size = 16;
        fb.method = 0;
        fb.is_encrypted = true;
        fb.crypt_version = 0;
        fb.lg2_count = 25;
        fb.salt.fill(core::byte(0x5A));
        fb.init_v.fill(core::byte(0xA5));
        assert(format::HeaderWriter::write_file_block(out, fb, 0));
        const core::byte payload[16] = {};
        assert(out.write(payload, sizeof(payload)) == sizeof(payload));
        format::EndArcBlock eb;
        assert(format::HeaderWriter::write_end_block(out, eb));
    }

    int rc_default = -100, rc_unlimited = -100;
    const fs::path dest = dir / "out.bin";
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t h =
            openrar_archive_open_file(arc.string().c_str(), "correct", nullptr, nullptr, nullptr);
        CHECK(h != 0);
        if (h != 0) {
            if (pass == 1) {
                int rc = openrar_archive_handle_set_limits(h, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                                                           UINT64_MAX);
                CHECK(rc == RAR_OK);
            }
            int rc = openrar_archive_handle_extract_to_path(h, 0, dest.string().c_str(), nullptr,
                                                            nullptr, nullptr);
            CHECK(rc != RAR_OK);
            CHECK(!fs::exists(dest));
            if (pass == 0)
                rc_default = rc;
            else
                rc_unlimited = rc;
            openrar_archive_close(h);
        }
    }
    // Identical refusal with and without caller budgets: no knob reaches the
    // floor.
    CHECK(rc_default == rc_unlimited);
    std::printf("  hostile-KDF rc (default vs unlimited): %d == %d\n", rc_default, rc_unlimited);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_dll_destinations_are_caller_owned() {
    std::cout << "[+] dll_destinations_are_caller_owned\n";
    const fs::path dir = make_dir("callerowned");
    const fs::path arc = dir / "hostile_name.rar";
    const std::vector<core::byte> payload(16, core::byte('E'));
    craft_stored_archive(arc, "../lm_escape.txt", payload);

    const fs::path dest_dir = dir / "out";
    fs::create_directories(dest_dir);
    const fs::path dest = dest_dir / "entry.bin";

    uint32_t h =
        openrar_archive_open_file(arc.string().c_str(), nullptr, nullptr, nullptr, nullptr);
    CHECK(h != 0);
    if (h != 0) {
        const int rc = openrar_archive_handle_extract_to_path(h, 0, dest.string().c_str(), nullptr,
                                                              nullptr, nullptr);
        // The caller-named destination gets the bytes; the hostile entry name
        // never reaches the filesystem anywhere.
        CHECK(rc == RAR_OK);
        CHECK(fs::exists(dest));
        CHECK(fs::file_size(dest) == payload.size());
        const fs::path escaped = dir / "lm_escape.txt";
        CHECK(!fs::exists(escaped));
        // The whole work tree carries exactly: the archive + out/entry.bin.
        std::error_code dec;
        for (const auto& e : fs::recursive_directory_iterator(dir, dec)) {
            const std::string name = io::u8_str(e.path().filename());
            CHECK(name == "hostile_name.rar" || name == "out" || name == "entry.bin");
        }
        openrar_archive_close(h);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void plant_link(const fs::path& link_path, const fs::path& target, bool dir_link) {
    std::error_code ec;
    fs::remove(link_path, ec);
#ifdef _WIN32
    io::RedirEntry re;
    re.type = dir_link ? io::RedirType::Junction : io::RedirType::WinSymlink;
    re.target = target.string();
    re.is_directory = dir_link;
    assert(io::create_reparse_link(link_path, re));
#else
    (void)dir_link;
    assert(::symlink(target.c_str(), link_path.c_str()) == 0);
#endif
}

static bool is_symlink_like(const fs::path& p) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (!ec && fs::is_symlink(st)) return true;
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}

static bool extraction_refuses_midpath_link(bool no_mmap_env, const fs::path& arc,
                                            const fs::path& dest) {
    if (no_mmap_env)
        set_env("OPENRAR_NO_MMAP", "1");
    else
        unset_env("OPENRAR_NO_MMAP");
    archive::ArchiveReader reader;
    reader.set_extraction_root(arc.parent_path());
    const bool opened = reader.open(arc);
    bool refused;
    if (!opened) {
        refused = true;
    } else {
        refused = !reader.extract_entry(reader.entries()[0], dest);
    }
    if (no_mmap_env) unset_env("OPENRAR_NO_MMAP");
    return refused;
}

static void test_containment_no_disable_switch() {
    std::cout << "[+] containment_no_disable_switch\n";
    const fs::path root = make_dir("contain");
    const fs::path outside = make_dir("contain_outside");

    // The archive fixture: one stored entry, added through the mutator so
    // the reader's own writer produces it.
    const fs::path arc = root / "fixture.rar";
    const fs::path blob = root / "blob.bin";
    {
        std::ofstream f(blob, std::ios::binary | std::ios::trunc);
        f << "should not land";
    }
    assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, "sub/inner.txt", 0));
    fs::remove(blob);

    // Adversary: a junction/symlink planted at an INTERMEDIATE component,
    // pointing at a real directory outside the extraction root.
    fs::create_directories(outside / "real");
    fs::create_directories(root / "sub");
    plant_link(root / "sub", outside / "real", true);

    const bool refused_mmap =
        extraction_refuses_midpath_link(false, arc, root / "sub" / "inner.txt");
    // Re-plant state (the first walk may have left the link; assert it did).
    CHECK(is_symlink_like(root / "sub"));
    const bool refused_no_mmap =
        extraction_refuses_midpath_link(true, arc, root / "sub" / "inner.txt");

    // Identical refusal both ways: the mmap kill-switch downgrades the read
    // engine, never the §4.1 containment walk.
    CHECK(refused_mmap);
    CHECK(refused_no_mmap == refused_mmap);
    CHECK(is_symlink_like(root / "sub")); // the planted link survives both
    bool outside_clean = true;
    std::error_code dec;
    for (const auto& e : fs::directory_iterator(outside / "real", dec)) {
        (void)e;
        outside_clean = false; // nothing may be written through the link
    }
    CHECK(outside_clean);
    CHECK(!fs::exists(root / "sub" / "inner.txt", dec));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    unset_env("OPENRAR_NO_MMAP");
}

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    test_set_limits_caller_layer_is_tunable();
    test_kdf_floor_unaffected_by_unlimited_limits();
    test_dll_destinations_are_caller_owned();
    test_containment_no_disable_switch();
    if (fails == 0) std::printf("[lm] library-mode containment gates: OK\n");
    return fails == 0 ? 0 : 1;
}
