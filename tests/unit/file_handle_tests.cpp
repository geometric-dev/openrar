// File-mode handle tests (v1.3.0) — openrar_archive_open_file /
// handle_extract_to_path / handle_test and the polymorphic behavior splits.
// Pins the contracts in docs/invariants.md: solid catch-up identity, RAM
// ceilings, volume lifetime, callback/cancel semantics, durability.
//
// Fixtures: plain archives via the C ABI create exports; solid, encrypted and
// multi-volume sets via the engine's ArchiveMutator (linked from openrar_core).

#include "openrar/openrar_dll.h"

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/io/file_stream.hpp"

#include <cassert>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#endif

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

using namespace openrar::archive;

// ── Helpers ──────────────────────────────────────────────────────────────────

static fs::path make_scratch_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / "openrar_file_handle_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static void write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    assert(f);
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
}

static std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    assert(f);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// Deterministic byte pattern: half run-heavy (compresses), half LCG noise.
static std::vector<uint8_t> make_pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            v[i] = static_cast<uint8_t>('A' + ((i / 64) % 16));
        } else {
            x = x * 1103515245u + 12345u;
            v[i] = static_cast<uint8_t>(x >> 16);
        }
    }
    return v;
}

// Plain archive from name→data map, written through the C ABI.
static fs::path
create_plain_archive(const fs::path& dir, const char* name,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<std::vector<uint8_t>> name_bytes;
    std::vector<const uint8_t*> name_ptrs;
    std::vector<const uint8_t*> data_ptrs;
    std::vector<size_t> sizes;
    name_bytes.reserve(files.size());
    for (const auto& [n, d] : files) {
        name_bytes.emplace_back(n.begin(), n.end());
        name_bytes.back().push_back(0);
        name_ptrs.push_back(name_bytes.back().data());
        data_ptrs.push_back(d.data());
        sizes.push_back(d.size());
    }
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create(name_ptrs.data(), data_ptrs.data(), sizes.data(),
                                    static_cast<uint32_t>(files.size()), 3, 2, &out, &out_len);
    assert(rc == RAR_OK);
    fs::path arc = dir / name;
    write_bytes(arc, std::vector<uint8_t>(out, out + out_len));
    openrar_free(out);
    return arc;
}

struct ProgressLog {
    std::vector<std::pair<uint64_t, uint64_t>> calls;
    static void cb(void* user, uint64_t done, uint64_t total) {
        static_cast<ProgressLog*>(user)->calls.emplace_back(done, total);
    }
    bool monotonic() const {
        for (size_t i = 1; i < calls.size(); ++i) {
            if (calls[i].first < calls[i - 1].first) return false;
            if (calls[i].second < calls[i - 1].second) return false;
        }
        return true;
    }
};

struct CancelPolicy {
    int polls = 0;
    int fire_after = -1; // -1 = never
    static int cb(void* user) {
        auto* c = static_cast<CancelPolicy*>(user);
        return (c->fire_after >= 0 && ++c->polls > c->fire_after) ? 1 : 0;
    }
};

static size_t count_temp_files(const fs::path& dir) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().u8string().find(".openrar-tmp.") != std::string::npos) ++n;
    return n;
}

static std::string last_error() {
    char b[512] = {};
    openrar_archive_get_error(b, sizeof(b));
    return b;
}

// Solid archive: three compressed entries chained with the solid flag.
static fs::path make_solid_archive(const fs::path& dir) {
    fs::path arc = dir / "solid.rar";
    std::vector<ArchiveMutator::PreparedAdd> prepared;
    for (int i = 0; i < 3; ++i) {
        const std::string entry = "s" + std::to_string(i) + ".bin";
        const fs::path src = dir / ("src_" + entry);
        write_bytes(src, make_pattern(1u << 20, 1000u + i)); // 1 MiB each
        ArchiveMutator::PreparedAdd p;
        assert(ArchiveMutator::prepare_add_file(src, entry, 3, "", p));
        prepared.push_back(std::move(p));
    }
    assert(ArchiveMutator::write_batch_add(arc, prepared, {}, "", false, {}, /*solid=*/true));
    return arc;
}

// File-encryption archive (-p): headers clear, payload AES-256-CBC.
static fs::path make_encrypted_archive(const fs::path& dir, const char* password,
                                       bool encrypt_headers, const char* arc_name,
                                       const std::vector<uint8_t>& data) {
    const fs::path src = dir / (std::string(arc_name) + ".src");
    write_bytes(src, data);
    fs::path arc = dir / arc_name;
    assert(ArchiveMutator::add_file_to_archive(arc, src, "payload.bin", 3, {}, 0, password,
                                               encrypt_headers));
    return arc;
}

// Multi-volume set: one entry split across .partNN.rar volumes.
static fs::path make_volume_set(const fs::path& dir, std::vector<uint8_t> data) {
    const fs::path src = dir / "vol_src.bin";
    write_bytes(src, data);
    const fs::path base = dir / "volset.rar";
    assert(ArchiveMutator::add_file_to_archive_vol(base, src, "big.bin", 0, 96 * 1024));
    return base;
}

// ── 1. Plain open/list/extract parity with the buffer surface ────────────────
static void test_plain_roundtrip() {
    std::cout << "Starting test_plain_roundtrip...\n" << std::flush;
    const fs::path dir = make_scratch_dir("plain");
    const auto blob_a = make_pattern(150000, 1);
    const auto blob_b = make_pattern(10, 2);
    const fs::path arc = create_plain_archive(dir, "plain.rar",
                                              {{"a.bin", blob_a}, {"d/", {}}, {"d/b.bin", blob_b}});

    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    assert(openrar_archive_handle_list(h, &count, &e, &p, &ps) == RAR_OK);
    assert(count == 3);
    std::map<std::string, const openrar_archive_entry_t*> by_name;
    const auto* ents = static_cast<const openrar_archive_entry_t*>(e);
    const char* paths = static_cast<const char*>(p);
    for (uint32_t i = 0; i < count; ++i)
        by_name[std::string(paths + ents[i].path_offset, ents[i].path_len)] = &ents[i];
    assert(by_name.count("a.bin") && by_name.count("d/") && by_name.count("d/b.bin"));
    assert(by_name["a.bin"]->size == blob_a.size());
    assert(by_name["a.bin"]->is_dir == 0);
    assert(by_name["d/"]->is_dir == 1);
    assert(by_name["d/b.bin"]->size == blob_b.size());
    openrar_archive_list_free(e, p, ps);

    // Memory extract matches the source bytes.
    uint8_t* out = nullptr;
    size_t len = 0;
    int rc = openrar_archive_handle_extract(h, 0, &out, &len);
    if (rc != RAR_OK) {
        char msg[256] = {0};
        openrar_last_error(msg, static_cast<int>(sizeof msg));
        std::fprintf(stderr, "[diag] handle_extract rc=%d msg='%s' len=%zu\n", rc, msg, len);
        std::fflush(stderr);
    }
    assert(rc == RAR_OK);
    assert(len == blob_a.size() && std::memcmp(out, blob_a.data(), len) == 0);
    openrar_free(out);

    // Extract to path matches too, and no temp file survives.
    const fs::path dest = dir / "out" / "a.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    const auto written = read_bytes(dest);
    assert(written == blob_a);
    assert(count_temp_files(dir) == 0);

    // Streaming test verifies every data entry.
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_OK);
    assert(openrar_archive_handle_test(h, 2, nullptr, nullptr, nullptr) == RAR_OK);
    openrar_archive_close(h);
    std::cout << "[PASS] plain_roundtrip\n";
}

// ── 2. Progress contract + temp hygiene on success ───────────────────────────
static void test_progress_contract() {
    std::cout << "Starting test_progress_contract...\n" << std::flush;
    const fs::path dir = make_scratch_dir("progress");
    const auto blob = make_pattern(2u << 20, 7); // 2 MiB
    const fs::path arc = create_plain_archive(dir, "p.rar", {{"big.bin", blob}});
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);

    ProgressLog log;
    const fs::path dest = dir / "big.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), ProgressLog::cb,
                                                  nullptr, &log) == RAR_OK);
    assert(log.monotonic());
    assert(log.calls.size() >= 2);
    assert(log.calls.back().first == blob.size() && log.calls.back().second == blob.size());
    // Exactly one final (total, total).
    size_t finals = 0;
    for (const auto& c : log.calls)
        if (c.first == blob.size() && c.second == blob.size()) ++finals;
    assert(finals == 1);
    assert(count_temp_files(dir) == 0);
    openrar_archive_close(h);
    std::cout << "[PASS] progress_contract\n";
}

// ── 3. Cancel leaves no temp and no destination ──────────────────────────────
static void test_cancel_mid_extract() {
    std::cout << "Starting test_cancel_mid_extract...\n" << std::flush;
    const fs::path dir = make_scratch_dir("cancel");
    const auto blob = make_pattern(2u << 20, 9);
    const fs::path arc = create_plain_archive(dir, "c.rar", {{"big.bin", blob}});
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);

    CancelPolicy cancel; // fire on the 4th poll
    cancel.fire_after = 3;
    const fs::path dest = dir / "never.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr,
                                                  CancelPolicy::cb, &cancel) == RAR_ERR_ABORTED);
    assert(!fs::exists(dest));
    assert(count_temp_files(dir) == 0);

    // The handle stays usable; a full retry succeeds.
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(read_bytes(dest) == blob);
    openrar_archive_close(h);
    std::cout << "[PASS] cancel_mid_extract\n";
}

// ── 4. Destination guard + vtable splits ─────────────────────────────────────
static void test_guards_and_vtable() {
    std::cout << "Starting test_guards_and_vtable...\n" << std::flush;
    const fs::path dir = make_scratch_dir("guards");
    const auto blob = make_pattern(50000, 11);
    const fs::path arc = create_plain_archive(dir, "g.rar", {{"x.bin", blob}});

    // Extracting ONTO the archive is rejected up front.
    uint32_t fh =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(fh != 0);
    assert(openrar_archive_handle_extract_to_path(fh, 0, arc.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_ERR_INVALID_ARG);
    assert(last_error().find("archive file") != std::string::npos);
    // extract_all is prohibited on file handles.
    uint8_t* b = nullptr;
    size_t bs = 0;
    uint64_t* offs = nullptr;
    uint32_t oc = 0;
    assert(openrar_archive_handle_extract_all(fh, &b, &bs, &offs, &oc) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    openrar_archive_close(fh);
    assert(fs::file_size(arc) > 0); // archive untouched

    // Buffer handle: test is unsupported, extract_all still works, to_path
    // still honors the durability contract.
    const auto buf = read_bytes(arc);
    uint32_t bh = openrar_archive_open(buf.data(), buf.size());
    assert(bh != 0);
    assert(openrar_archive_handle_test(bh, 0, nullptr, nullptr, nullptr) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(openrar_archive_handle_extract_all(bh, &b, &bs, &offs, &oc) == RAR_OK);
    openrar_free(b);
    openrar_free(offs);
    const fs::path dest = dir / "x.bin";
    assert(openrar_archive_handle_extract_to_path(bh, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(read_bytes(dest) == blob);
    openrar_archive_close(bh);
    std::cout << "[PASS] guards_and_vtable\n";
}

// ── 5. Directory entries: create + single (0,0) callback ─────────────────────
static void test_directory_entry() {
    std::cout << "Starting test_directory_entry...\n" << std::flush;
    const fs::path dir = make_scratch_dir("dirs");
    const fs::path arc =
        create_plain_archive(dir, "d.rar", {{"nested/", {}}, {"nested/f.txt", {'h', 'i'}}});
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    ProgressLog log;
    const fs::path dest = dir / "out" / "nested";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), ProgressLog::cb,
                                                  nullptr, &log) == RAR_OK);
    assert(fs::is_directory(dest));
    assert(log.calls.size() == 1 && log.calls[0].first == 0 && log.calls[0].second == 0);
    // The file inside still extracts to the same tree.
    const fs::path fdest = dir / "out" / "nested" / "f.txt";
    assert(openrar_archive_handle_extract_to_path(h, 1, fdest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    const auto got = read_bytes(fdest);
    assert(got.size() == 2 && got[0] == 'h' && got[1] == 'i');
    openrar_archive_close(h);
    std::cout << "[PASS] directory_entry\n";
}

// ── 6. Solid: out-of-order identity, repeat extract, cancel during catch-up ──
static void test_solid_catch_up() {
    std::cout << "Starting test_solid_catch_up...\n" << std::flush;
    const fs::path dir = make_scratch_dir("solid");
    const fs::path arc = make_solid_archive(dir);

    // In-order reference extraction.
    std::vector<std::vector<uint8_t>> reference(3);
    {
        uint32_t h =
            openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h != 0);
        for (uint32_t i = 0; i < 3; ++i) {
            uint8_t* out = nullptr;
            size_t len = 0;
            assert(openrar_archive_handle_extract(h, i, &out, &len) == RAR_OK);
            reference[i].assign(out, out + len);
            openrar_free(out);
        }
        openrar_archive_close(h);
    }
    assert(reference[0].size() == 1u << 20);

    // Out-of-order: byte-identical despite chain restarts.
    {
        uint32_t h =
            openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h != 0);
        const uint32_t order[3] = {2, 0, 1};
        for (uint32_t i : order) {
            const fs::path dest = dir / ("ooo_" + std::to_string(i) + ".bin");
            assert(openrar_archive_handle_extract_to_path(h, i, dest.u8string().c_str(), nullptr,
                                                          nullptr, nullptr) == RAR_OK);
            assert(read_bytes(dest) == reference[i]);
        }
        // Repeated extraction of the same entry succeeds (catch-up re-runs).
        assert(openrar_archive_handle_extract_to_path(h, 2, (dir / "again.bin").u8string().c_str(),
                                                      nullptr, nullptr, nullptr) == RAR_OK);
        assert(read_bytes(dir / "again.bin") == reference[2]);
        openrar_archive_close(h);
    }

    // Cancel during catch-up: aborts, handle stays usable, retry succeeds.
    {
        uint32_t h =
            openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h != 0);
        CancelPolicy cancel;
        cancel.fire_after = 1; // fire on the 2nd poll — mid catch-up
        assert(openrar_archive_handle_extract_to_path(h, 2, (dir / "cx.bin").u8string().c_str(),
                                                      nullptr, CancelPolicy::cb,
                                                      &cancel) == RAR_ERR_ABORTED);
        assert(!fs::exists(dir / "cx.bin"));
        assert(count_temp_files(dir) == 0);
        assert(openrar_archive_handle_extract_to_path(h, 2, (dir / "cx.bin").u8string().c_str(),
                                                      nullptr, nullptr, nullptr) == RAR_OK);
        assert(read_bytes(dir / "cx.bin") == reference[2]);
        openrar_archive_close(h);
    }
    std::cout << "[PASS] solid_catch_up\n";
}

// ── 7. Encrypted payload (-p): lazy password verification ────────────────────
static void test_encrypted_extraction() {
    std::cout << "Starting test_encrypted_extraction...\n" << std::flush;
    const fs::path dir = make_scratch_dir("encrypted");
    const auto blob = make_pattern(300000, 21);
    const fs::path arc = make_encrypted_archive(dir, "pw123", false, "enc.rar", blob);

    // No password: extraction refuses loudly.
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0); // headers are clear — open succeeds
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_ERR_ENCRYPTED);
    assert(openrar_archive_handle_extract_to_path(h, 0, (dir / "no.bin").u8string().c_str(),
                                                  nullptr, nullptr, nullptr) == RAR_ERR_ENCRYPTED);
    openrar_archive_close(h);

    // Wrong password: BAD_PASSWORD, never CRC.
    h = openrar_archive_open_file(arc.u8string().c_str(), "wrong", nullptr, nullptr, nullptr);
    assert(h != 0);
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_ERR_BAD_PASSWORD);
    assert(openrar_archive_handle_extract_to_path(h, 0, (dir / "bad.bin").u8string().c_str(),
                                                  nullptr, nullptr,
                                                  nullptr) == RAR_ERR_BAD_PASSWORD);
    assert(!fs::exists(dir / "bad.bin"));
    openrar_archive_close(h);

    // Correct password: extraction + test verify, bytes match.
    h = openrar_archive_open_file(arc.u8string().c_str(), "pw123", nullptr, nullptr, nullptr);
    assert(h != 0);
    const fs::path dest = dir / "ok.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(read_bytes(dest) == blob);
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_OK);
    openrar_archive_close(h);
    std::cout << "[PASS] encrypted_extraction\n";
}

// ── 8. Header-encrypted (-hp): open-time password flow ───────────────────────
static void test_hp_open_flow() {
    std::cout << "Starting test_hp_open_flow...\n" << std::flush;
    const fs::path dir = make_scratch_dir("hp");
    const auto blob = make_pattern(120000, 31);
    const fs::path arc = make_encrypted_archive(dir, "hp-pw", true, "hp.rar", blob);
    // The checked-in -hp fixture doubles as a cross-check of the open flow.
    const fs::path fixture = fs::path(OPENRAR_SOURCE_DIR) / "tests" / "hello5_hp.rar";

    // No password: open fails with the prompt-me signal.
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h == 0);
    assert(last_error().find("encrypted") != std::string::npos);
    h = openrar_archive_open_file(fixture.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h == 0);
    assert(last_error().find("encrypted") != std::string::npos);

    // Wrong password: the dedicated code.
    h = openrar_archive_open_file(fixture.u8string().c_str(), "nope", nullptr, nullptr, nullptr);
    assert(h == 0);
    assert(last_error().find("wrong password") != std::string::npos);

    // Correct password: list, extract, test all work.
    h = openrar_archive_open_file(arc.u8string().c_str(), "hp-pw", nullptr, nullptr, nullptr);
    assert(h != 0);
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    assert(openrar_archive_handle_list(h, &count, &e, &p, &ps) == RAR_OK);
    assert(count == 1);
    openrar_archive_list_free(e, p, ps);
    const fs::path dest = dir / "payload.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(read_bytes(dest) == blob);
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_OK);
    openrar_archive_close(h);

    // The fixture (WinRAR-made hello5_hp.rar) extracts with its known password.
    h = openrar_archive_open_file(fixture.u8string().c_str(), "secret", nullptr, nullptr, nullptr);
    assert(h != 0);
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_OK);
    openrar_archive_close(h);
    std::cout << "[PASS] hp_open_flow\n";
}

// ── 9. Multi-volume: stitching, rewind, missing volumes ──────────────────────
// Deterministic part ordering: parse the numeric suffix after ".part"
// (stem form "set.part2" -> 2) so volume tests never depend on directory
// iteration order (ext4 is hash-ordered; NTFS/Apfs happen to sort).
static long volume_part_num(const fs::path& p) {
    const std::string s = p.stem().u8string();
    const size_t pos = s.find(".part");
    if (pos == std::string::npos) return 0;
    return std::strtol(s.c_str() + pos + 5, nullptr, 10);
}

static bool volume_part_less(const fs::path& a, const fs::path& b) {
    return volume_part_num(a) < volume_part_num(b);
}

static void test_volume_set() {
    std::cout << "Starting test_volume_set...\n" << std::flush;
    const fs::path dir = make_scratch_dir("volumes");
    const auto blob = make_pattern(500 * 1024, 41); // spans several 96 KiB volumes
    const fs::path base = make_volume_set(dir, blob);

    // Discover the created volumes (.partNN.rar naming). directory_iterator
    // order is NOT sorted on ext4 (hash order) — sort by part number so the
    // rename/open sequence below means part1/part2 deterministically (the
    // CI ubuntu-clang leg hit exactly this: parts[0] was a middle volume,
    // so the missing-first-volume error carried the strict-set message).
    std::vector<fs::path> parts;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().u8string().find(".part") != std::string::npos) parts.push_back(e.path());
    std::sort(parts.begin(), parts.end(), volume_part_less);
    assert(parts.size() >= 2);

    // Open part1 (or the base name): one merged entry, extraction stitches.
    const fs::path part1 = parts[0];
    uint32_t h =
        openrar_archive_open_file(part1.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    assert(openrar_archive_handle_list(h, &count, &e, &p, &ps) == RAR_OK);
    assert(count == 1); // split entry merged into one logical entry
    {
        const auto* ents = static_cast<const openrar_archive_entry_t*>(e);
        assert(ents[0].size == blob.size());
    }
    openrar_archive_list_free(e, p, ps);
    const fs::path dest = dir / "stitched.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(read_bytes(dest) == blob);
    assert(openrar_archive_handle_test(h, 0, nullptr, nullptr, nullptr) == RAR_OK);
    openrar_archive_close(h);

    // Middle-volume rewind: opening part2 resolves back to part1.
    if (parts.size() >= 2) {
        h = openrar_archive_open_file(parts[1].u8string().c_str(), nullptr, nullptr, nullptr,
                                      nullptr);
        assert(h != 0);
        openrar_archive_close(h);

        // Missing first volume: hard error naming the derived part1.
        const fs::path hidden1 = part1.u8string() + ".hidden";
        fs::rename(part1, hidden1);
        h = openrar_archive_open_file(parts[1].u8string().c_str(), nullptr, nullptr, nullptr,
                                      nullptr);
        assert(h == 0);
        if (last_error().find("cannot open first volume") == std::string::npos) {
            std::fprintf(stderr, "volume-rewind error was: %s\n", last_error().c_str());
            assert(false && "expected 'cannot open first volume' (see printed error)");
        }
        fs::rename(hidden1, part1);

        // Missing middle volume at open time: strict set check.
        const fs::path hidden2 = parts[1].u8string() + ".hidden";
        fs::rename(parts[1], hidden2);
        h = openrar_archive_open_file(part1.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h == 0);
        if (last_error().find("missing volume") == std::string::npos) {
            std::fprintf(stderr, "strict-set error was: %s\n", last_error().c_str());
            assert(false && "expected 'missing volume' (see printed error)");
        }
        fs::rename(hidden2, parts[1]);

        // Missing middle volume at extract time: the handle opened while the
        // set was complete still reports the absent volume on extraction.
        uint32_t h2 =
            openrar_archive_open_file(part1.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h2 != 0);
        fs::rename(parts[1], hidden2);
        assert(openrar_archive_handle_extract_to_path(h2, 0, (dir / "gap.bin").u8string().c_str(),
                                                      nullptr, nullptr,
                                                      nullptr) == RAR_ERR_MISSING_VOLUME);
        assert(!fs::exists(dir / "gap.bin"));
        assert(last_error().find("missing volume") != std::string::npos);
        openrar_archive_close(h2);
        fs::rename(hidden2, parts[1]);
    }
    std::cout << "[PASS] volume_set\n";
}

// ── 10. RAM ceiling: chunked decrypt + streaming (Windows measurement) ───────
static void test_ram_ceiling() {
#ifdef _WIN32
    std::cout << "Starting test_ram_ceiling...\n" << std::flush;
    const fs::path dir = make_scratch_dir("ram");
    const size_t kSize = 100u << 20; // 100 MiB
    auto blob = make_pattern(kSize, 51);
    const fs::path arc = make_encrypted_archive(dir, "pw", false, "ram.rar", blob);
    blob.clear(); // drop the source copy before measuring

    PROCESS_MEMORY_COUNTERS before{};
    assert(GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before)));
    uint32_t h = openrar_archive_open_file(arc.u8string().c_str(), "pw", nullptr, nullptr, nullptr);
    assert(h != 0);
    const fs::path dest = dir / "payload.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    PROCESS_MEMORY_COUNTERS after{};
    assert(GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after)));

    const unsigned long long delta = after.PeakWorkingSetSize - before.PeakWorkingSetSize;
    // Chunked decrypt streams in 64 KiB slices: the bound is the dictionary
    // window + slice buffers (docs/invariants.md §2). The historical
    // whole-cipher path would show ≥ 100 MiB here; allow a generous 32 MiB.
    std::cout << "  peak working set delta: " << (delta >> 20) << " MiB\n";
    assert(delta <= (32ull << 20));
    openrar_archive_close(h);
    std::cout << "[PASS] ram_ceiling\n";
#endif
}

// ── 11. In-memory extract cap on file handles ────────────────────────────────
static void test_heap_extract_cap() {
    std::cout << "Starting test_heap_extract_cap...\n" << std::flush;
    const fs::path dir = make_scratch_dir("cap");
    const size_t kSize = (256u << 20) + 1; // one byte over the cap
    const fs::path arc =
        create_plain_archive(dir, "cap.rar", {{"huge.bin", std::vector<uint8_t>(kSize, 0x5A)}});
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint8_t* out = nullptr;
    size_t len = 0;
    assert(openrar_archive_handle_extract(h, 0, &out, &len) == RAR_ERR_NOMEM);
    assert(out == nullptr && len == 0);
    assert(last_error().find("256 MiB") != std::string::npos);
    // The streaming path has no cap.
    const fs::path dest = dir / "huge.bin";
    assert(openrar_archive_handle_extract_to_path(h, 0, dest.u8string().c_str(), nullptr, nullptr,
                                                  nullptr) == RAR_OK);
    assert(fs::file_size(dest) == kSize);
    openrar_archive_close(h);
    std::cout << "[PASS] heap_extract_cap\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr under ctest (piped stdio).
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    // RAM measurement first: PeakWorkingSetSize is monotonic, so this test
    // must run before the large-fixture cap test.
    test_ram_ceiling();
    test_plain_roundtrip();
    test_progress_contract();
    test_cancel_mid_extract();
    test_guards_and_vtable();
    test_directory_entry();
    test_solid_catch_up();
    test_encrypted_extraction();
    test_hp_open_flow();
    test_volume_set();
    test_heap_extract_cap();
    std::cout << "ALL FILE HANDLE TESTS PASSED\n";
    return 0;
}
