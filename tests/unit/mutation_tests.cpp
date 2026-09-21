// Mutation tests (v1.4.0) — openrar_archive_delete_entries_file /
// openrar_archive_add_files_file over the atomic ArchiveMutator engine.
// Pins the contracts in docs/dll-integration-spec.md §6.12 and
// docs/invariants.md §1: file-handle listing index space, suffix-only
// solid delete, 'u' replacement semantics, atomicity, RAR_ERR_BUSY handle
// collision, CMT preservation and QO stripping.
//
// Fixtures are built engine-side (openrar_core): plain archives through the
// C ABI create export, solid sets and comments through ArchiveMutator, and
// a synthetic QuickOpen service header through HeaderWriter.

#include "openrar/openrar_dll.h"
#include "openrar/openrar.hpp"

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/compress/compress_plan.hpp"
#include "../../src/crypto/sha256.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
namespace engine = openrar::archive;

using engine::ArchiveMutator;

// ── Helpers ──────────────────────────────────────────────────────────────────

static fs::path make_scratch_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / "openrar_mutation_tests" / name;
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

static std::string last_error() {
    char b[512] = {};
    openrar_archive_get_error(b, sizeof(b));
    return b;
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

// Plain (non-solid) archive through the C ABI create export.
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

// Solid archive: three compressed entries, head s0 followed by chained s1/s2.
static fs::path make_solid_archive(const fs::path& dir, const char* name) {
    fs::path arc = dir / name;
    std::vector<ArchiveMutator::PreparedAdd> prepared;
    for (int i = 0; i < 3; ++i) {
        const std::string entry = "s" + std::to_string(i) + ".bin";
        const fs::path src = dir / (std::string("src_") + entry);
        write_bytes(src, make_pattern(1u << 18, 2000u + i)); // 256 KiB each
        ArchiveMutator::PreparedAdd p;
        p.entry_name = entry;
        p.src_path = src;
        assert(ArchiveMutator::prepare_add_file(src, entry, 3, "", p));
        prepared.push_back(std::move(p));
    }
    assert(ArchiveMutator::write_batch_add(arc, prepared, {}, "", false, {}, /*solid=*/true));
    return arc;
}

// Multi-volume set: one entry split across .partNN.rar volumes.
static fs::path make_volume_set(const fs::path& dir, const char* name) {
    const fs::path src = dir / "vol_src.bin";
    write_bytes(src, make_pattern(10u * 1024, 77));
    fs::path arc = dir / name;
    assert(ArchiveMutator::add_file_to_archive_vol(arc, src, "vol.bin", 3, 4096));
    return arc;
}

// Entry names in file-handle listing order (what the delete indices refer to).
static std::vector<std::string> handle_names(const fs::path& arc) {
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint32_t count = 0;
    void* ents = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    assert(openrar_archive_handle_list(h, &count, &ents, &paths, &paths_sz) == RAR_OK);
    auto* e = static_cast<const openrar_archive_entry_t*>(ents);
    const char* p = static_cast<const char*>(paths);
    std::vector<std::string> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out.emplace_back(p + e[i].path_offset, e[i].path_len);
    }
    openrar_archive_list_free(ents, paths, paths_sz);
    openrar_archive_close(h);
    return out;
}

static std::vector<uint8_t> handle_extract(const fs::path& arc, uint32_t index) {
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint8_t* out = nullptr;
    size_t len = 0;
    int rc = openrar_archive_handle_extract(h, index, &out, &len);
    assert(rc == RAR_OK);
    std::vector<uint8_t> data(out, out + len);
    if (out) openrar_free(out);
    openrar_archive_close(h);
    return data;
}

// Same, on a caller-owned handle (used to prove a handle stays usable after
// a refused mutation).
static std::vector<uint8_t> handle_extract_via_handle(uint32_t h, uint32_t index) {
    uint8_t* out = nullptr;
    size_t len = 0;
    int rc = openrar_archive_handle_extract(h, index, &out, &len);
    assert(rc == RAR_OK);
    std::vector<uint8_t> data(out, out + len);
    if (out) openrar_free(out);
    return data;
}

// Engine-side view (services included) for CMT/QO assertions.
static bool has_service(const fs::path& arc, const char* type) {
    engine::ArchiveReader reader;
    assert(reader.open(arc));
    for (const auto& e : reader.entries()) {
        if (e.header.is_service && e.header.service_type == type) return true;
    }
    return false;
}

// Archive with one file plus a synthetic QuickOpen service header (dummy
// payload — the reader treats QO generically), to pin the QO-strip contract.
static fs::path create_qo_archive(const fs::path& dir, const char* name) {
    const std::vector<uint8_t> data = {'q', 'o', ' ', 'f', 'i', 'x', 't', 'u', 'r', 'e'};
    fs::path arc = dir / name;
    openrar::io::FileStream out;
    assert(out.open(arc, openrar::io::FileMode::CreateAlways));
    openrar::format::HeaderWriter::write_signature(out);
    openrar::format::MainBlock mb;
    openrar::format::HeaderWriter::write_main_block(out, mb);

    openrar::format::FileBlock qo;
    qo.is_service = true;
    qo.service_type = "QO";
    qo.file_name = "QO";
    qo.unp_size = 4;
    qo.pack_size = 4;
    qo.attributes = 0x20;
    qo.has_crc32 = true;
    openrar::crypto::Crc32 qo_crc;
    const uint8_t qo_payload[4] = {0, 0, 0, 0};
    qo_crc.update(qo_payload, sizeof(qo_payload));
    qo.data_crc32 = qo_crc.get();
    qo.method = 0;
    qo.win_size = 0;
    assert(openrar::format::HeaderWriter::write_file_block(out, qo, 0));
    assert(out.write(qo_payload, sizeof(qo_payload)) == sizeof(qo_payload));

    openrar::format::FileBlock fb;
    fb.file_name = "kept.bin";
    fb.unp_size = data.size();
    fb.pack_size = static_cast<openrar::core::int64>(data.size());
    fb.attributes = 0x20;
    fb.has_crc32 = true;
    openrar::crypto::Crc32 crc;
    crc.update(data.data(), data.size());
    fb.data_crc32 = crc.get();
    fb.method = 0;
    fb.win_size = 0;
    assert(openrar::format::HeaderWriter::write_file_block(out, fb, 0));
    assert(out.write(data.data(), data.size()) == data.size());

    openrar::format::EndArcBlock eb;
    openrar::format::HeaderWriter::write_end_block(out, eb);
    out.close();
    return arc;
}

// ── 1. Delete roundtrip: entry gone, survivors byte-identical ────────────────
static void test_delete_roundtrip() {
    std::cout << "Starting test_delete_roundtrip...\n" << std::flush;
    const fs::path dir = make_scratch_dir("roundtrip");
    const std::vector<uint8_t> a = make_pattern(4096, 1), b = make_pattern(4096, 2),
                               c = make_pattern(4096, 3);
    const fs::path arc =
        create_plain_archive(dir, "del.rar", {{"a.txt", a}, {"b.txt", b}, {"c.txt", c}});

    assert(handle_names(arc) == std::vector<std::string>({"a.txt", "b.txt", "c.txt"}));

    const uint32_t idx[1] = {1}; // b.txt
    assert(openrar_archive_delete_entries_file(arc.u8string().c_str(), idx, 1) == RAR_OK);

    // Re-open: entry gone, others byte-identical.
    assert(handle_names(arc) == std::vector<std::string>({"a.txt", "c.txt"}));
    assert(handle_extract(arc, 0) == a);
    assert(handle_extract(arc, 1) == c);
    std::cout << "[PASS] delete_roundtrip\n";
}

// ── 2. Solid orphan delete refused; archive still loads ──────────────────────
static void test_solid_orphan_delete_refused() {
    std::cout << "Starting test_solid_orphan_delete_refused...\n" << std::flush;
    const fs::path dir = make_scratch_dir("solid_orphan");
    const fs::path arc = make_solid_archive(dir, "solid.rar");

    // Head delete (index 0) with later members retained → refused.
    const uint32_t head[1] = {0};
    assert(openrar_archive_delete_entries_file(arc.u8string().c_str(), head, 1) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(last_error().find("cannot delete head of solid block") != std::string::npos);

    // Mid-run delete (index 1) with later member retained → refused.
    const uint32_t mid[1] = {1};
    assert(openrar_archive_delete_entries_file(arc.u8string().c_str(), mid, 1) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(last_error().find("cannot delete entries from solid archive") != std::string::npos);

    // Refusals leave the archive loadable and its data intact.
    assert(handle_names(arc) == std::vector<std::string>({"s0.bin", "s1.bin", "s2.bin"}));
    assert(handle_extract(arc, 2) == read_bytes(dir / "src_s2.bin"));
    std::cout << "[PASS] solid_orphan_delete_refused\n";
}

// ── 3. Suffix delete and whole-run delete allowed ────────────────────────────
static void test_solid_suffix_and_whole_run_delete() {
    std::cout << "Starting test_solid_suffix_and_whole_run_delete...\n" << std::flush;
    const fs::path dir = make_scratch_dir("solid_suffix");

    // Suffix: delete the last member of the run → allowed.
    const fs::path suffix_arc = make_solid_archive(dir, "suffix.rar");
    const std::vector<uint8_t> s0 = read_bytes(dir / "src_s0.bin");
    const std::vector<uint8_t> s1 = read_bytes(dir / "src_s1.bin");
    const uint32_t tail[1] = {2};
    assert(openrar_archive_delete_entries_file(suffix_arc.u8string().c_str(), tail, 1) == RAR_OK);
    assert(handle_names(suffix_arc) == std::vector<std::string>({"s0.bin", "s1.bin"}));
    assert(handle_extract(suffix_arc, 0) == s0);
    assert(handle_extract(suffix_arc, 1) == s1);

    // Whole run: delete every member → allowed.
    const fs::path whole_arc = make_solid_archive(dir, "whole.rar");
    const uint32_t all[3] = {0, 1, 2};
    assert(openrar_archive_delete_entries_file(whole_arc.u8string().c_str(), all, 3) == RAR_OK);
    assert(handle_names(whole_arc).empty());
    std::cout << "[PASS] solid_suffix_and_whole_run_delete\n";
}

// ── 4. Locked / multi-volume / header-encrypted refused ──────────────────────
static void test_locked_volume_hp_refused() {
    std::cout << "Starting test_locked_volume_hp_refused...\n" << std::flush;
    const fs::path dir = make_scratch_dir("refusals");

    // Locked archive: delete and add both refused.
    const fs::path locked = create_plain_archive(dir, "locked.rar", {{"x.txt", {'x'}}});
    assert(ArchiveMutator::lock_archive(locked));
    const uint32_t idx0[1] = {0};
    assert(openrar_archive_delete_entries_file(locked.u8string().c_str(), idx0, 1) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    const fs::path src = dir / "add_src.bin";
    write_bytes(src, {'n', 'e', 'w'});
    const std::string src_u8 = src.u8string();
    const char* srcs[1] = {src_u8.c_str()};
    const char* names[1] = {"x.txt"};
    assert(openrar_archive_add_files_file(locked.u8string().c_str(), srcs, names, 1, 3, 2) ==
           RAR_ERR_UNSUPPORTED_FEATURE);

    // Multi-volume set: refused. The mutation exports take the path
    // literally (no first-volume derivation like open_file), so the first
    // volume's own name is what gets mutated/refused.
    const fs::path vol = make_volume_set(dir, "set.rar");
    const fs::path vol1 = dir / "set.part01.rar";
    assert(fs::exists(vol1));
    assert(openrar_archive_delete_entries_file(vol1.u8string().c_str(), idx0, 1) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(openrar_archive_add_files_file(vol1.u8string().c_str(), srcs, names, 1, 3, 2) ==
           RAR_ERR_UNSUPPORTED_FEATURE);

    // Header-encrypted (-hp): no password on this surface → refused, both.
    const fs::path hp_src = dir / "hp_src.bin";
    write_bytes(hp_src, make_pattern(1024, 9));
    const fs::path hp = dir / "hp.rar";
    assert(ArchiveMutator::add_file_to_archive(hp, hp_src, "payload.bin", 3, {}, 0, "pw",
                                               /*encrypt_headers=*/true));
    const int hp_rc = openrar_archive_delete_entries_file(hp.u8string().c_str(), idx0, 1);
    if (hp_rc != RAR_ERR_UNSUPPORTED_FEATURE) {
        std::cout << "  hp delete rc=" << hp_rc << " err=" << last_error() << "\n" << std::flush;
    }
    assert(hp_rc == RAR_ERR_UNSUPPORTED_FEATURE);
    assert(last_error().find("mutating header-encrypted archive") != std::string::npos);
    assert(openrar_archive_add_files_file(hp.u8string().c_str(), srcs, names, 1, 3, 2) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    std::cout << "[PASS] locked_volume_hp_refused\n";
}

// ── 5. 'u' semantics: same-name entry replaced, new content byte-exact ───────
static void test_add_replace_u_semantics() {
    std::cout << "Starting test_add_replace_u_semantics...\n" << std::flush;
    const fs::path dir = make_scratch_dir("u_semantics");
    const std::vector<uint8_t> a_old = {'O', 'L', 'D'}, b = {'B'};
    const fs::path arc = create_plain_archive(dir, "u.rar", {{"a.txt", a_old}, {"b.txt", b}});

    const std::vector<uint8_t> a_new = make_pattern(2048, 42);
    const fs::path a_src = dir / "a_new.txt";
    write_bytes(a_src, a_new);
    const std::string a_src_u8 = a_src.u8string();
    const std::string arc_u8 = arc.u8string();
    const char* srcs[1] = {a_src_u8.c_str()};
    const char* names[1] = {"a.txt"};
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, names, 1, 3, 2) == RAR_OK);

    // Same entry count (old instance stripped, new appended), new content.
    assert(handle_names(arc) == std::vector<std::string>({"b.txt", "a.txt"}));
    assert(handle_extract(arc, 0) == b);
    const std::vector<uint8_t> got = handle_extract(arc, 1);
    if (got != a_new) {
        std::cout << "  u_semantics: got size=" << got.size() << " want=" << a_new.size()
                  << " first8=";
        for (size_t i = 0; i < 8 && i < got.size(); ++i) std::cout << (int)got[i] << ",";
        std::cout << "\n" << std::flush;
    }
    assert(got == a_new);
    std::cout << "[PASS] add_replace_u_semantics\n";
}

// ── 5b. New names append; solid archives continue the stream ─────────────────
static void test_solid_append_new_name() {
    std::cout << "Starting test_solid_append_new_name...\n" << std::flush;
    const fs::path dir = make_scratch_dir("solid_append");
    const fs::path arc = make_solid_archive(dir, "solid.rar");

    const std::vector<uint8_t> extra = make_pattern(4096, 31);
    const fs::path extra_src = dir / "extra.bin";
    write_bytes(extra_src, extra);
    const std::string extra_src_u8 = extra_src.u8string();
    const std::string solid_arc_u8 = arc.u8string();
    const char* srcs[1] = {extra_src_u8.c_str()};
    const char* names[1] = {"extra.bin"};
    assert(openrar_archive_add_files_file(solid_arc_u8.c_str(), srcs, names, 1, 3, 2) == RAR_OK);
    assert(handle_names(arc) ==
           std::vector<std::string>({"s0.bin", "s1.bin", "s2.bin", "extra.bin"}));
    assert(handle_extract(arc, 3) == extra);
    // The retained solid chain still decodes.
    assert(handle_extract(arc, 2) == read_bytes(dir / "src_s2.bin"));

    // Replacing a solid member (head or chained) is refused; the archive is
    // unchanged afterwards.
    const char* rnames[1] = {"s1.bin"};
    assert(openrar_archive_add_files_file(solid_arc_u8.c_str(), srcs, rnames, 1, 3, 2) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(last_error().find("cannot replace entry in solid archive") != std::string::npos);
    assert(handle_names(arc).size() == 4);
    std::cout << "[PASS] solid_append_new_name\n";
}

// ── 6. Atomicity: failed batch leaves the original untouched ─────────────────
static void test_add_atomicity() {
    std::cout << "Starting test_add_atomicity...\n" << std::flush;
    const fs::path dir = make_scratch_dir("atomicity");
    const std::vector<uint8_t> a = {'A'}, b = {'B'};
    const fs::path arc = create_plain_archive(dir, "at.rar", {{"a.txt", a}, {"b.txt", b}});
    const auto before = read_bytes(arc);

    // Second source missing → the whole batch fails, original untouched.
    const fs::path good = dir / "good.txt";
    write_bytes(good, {'G'});
    const std::string good_u8 = good.u8string();
    const std::string missing_u8 = (dir / "missing.bin").u8string();
    const std::string at_arc_u8 = arc.u8string();
    const char* srcs[2] = {good_u8.c_str(), missing_u8.c_str()};
    const char* names[2] = {"good.txt", "missing.bin"};
    assert(openrar_archive_add_files_file(at_arc_u8.c_str(), srcs, names, 2, 3, 2) == RAR_ERR_IO);
    assert(read_bytes(arc) == before);
    assert(handle_names(arc) == std::vector<std::string>({"a.txt", "b.txt"}));
    std::cout << "[PASS] add_atomicity\n";
}

// ── 7. RAR_ERR_BUSY while a file-mode handle holds the archive ───────────────
static void test_busy_handle_collision() {
    std::cout << "Starting test_busy_handle_collision...\n" << std::flush;
    const fs::path dir = make_scratch_dir("busy");
    const std::vector<uint8_t> a = make_pattern(1024, 5);
    const fs::path arc = create_plain_archive(dir, "busy.rar", {{"a.txt", a}});

    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);

    const uint32_t idx[1] = {0};
    assert(openrar_archive_delete_entries_file(arc.u8string().c_str(), idx, 1) == RAR_ERR_BUSY);
    assert(last_error().find("close it before mutating") != std::string::npos);

    const fs::path src = dir / "z.bin";
    write_bytes(src, {'z'});
    const std::string z_src_u8 = src.u8string();
    const std::string busy_arc_u8 = arc.u8string();
    const char* srcs[1] = {z_src_u8.c_str()};
    const char* names[1] = {"z.bin"};
    assert(openrar_archive_add_files_file(busy_arc_u8.c_str(), srcs, names, 1, 3, 2) ==
           RAR_ERR_BUSY);

    // The handle still works (its archive was not touched).
    assert(handle_extract_via_handle(h, 0) == a);

    openrar_archive_close(h);
    // Closed → both mutations succeed.
    assert(openrar_archive_delete_entries_file(busy_arc_u8.c_str(), idx, 1) == RAR_OK);
    assert(openrar_archive_add_files_file(busy_arc_u8.c_str(), srcs, names, 1, 3, 2) == RAR_OK);
    assert(handle_names(arc) == std::vector<std::string>({"z.bin"}));
    std::cout << "[PASS] busy_handle_collision\n";
}

// ── 8. Argument validation parity ────────────────────────────────────────────
static void test_validation_errors() {
    std::cout << "Starting test_validation_errors...\n" << std::flush;
    const fs::path dir = make_scratch_dir("validation");
    const fs::path arc =
        create_plain_archive(dir, "v.rar", {{"a.txt", {'a'}}, {"b.txt", {'b'}}, {"c.txt", {'c'}}});
    const std::string arc_u8 = arc.u8string();

    // Delete: null args, count 0, out-of-range index (with the pinned detail).
    const uint32_t idx[1] = {0};
    assert(openrar_archive_delete_entries_file(nullptr, idx, 1) == RAR_ERR_INVALID_ARG);
    assert(openrar_archive_delete_entries_file(arc_u8.c_str(), nullptr, 1) == RAR_ERR_INVALID_ARG);
    assert(openrar_archive_delete_entries_file(arc_u8.c_str(), idx, 0) == RAR_ERR_INVALID_ARG);
    const uint32_t oor[1] = {3};
    assert(openrar_archive_delete_entries_file(arc_u8.c_str(), oor, 1) == RAR_ERR_INVALID_ARG);
    assert(last_error().find("entry index 3 out of range (archive has 3 entries)") !=
           std::string::npos);

    // Add: method / window_log2 parity with create, null args, count 0.
    const fs::path src = dir / "s.bin";
    write_bytes(src, {'s'});
    const std::string s_src_u8 = src.u8string();
    const char* srcs[1] = {s_src_u8.c_str()};
    const char* names[1] = {"s.bin"};
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, names, 1, 1, 2) ==
           RAR_ERR_INVALID_ARG); // method ∉ {0,3,5}
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, names, 1, 3, 0) ==
           RAR_ERR_INVALID_ARG); // window_log2 < 1
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, names, 1, 3, 5) ==
           RAR_ERR_INVALID_ARG); // window_log2 > 4
    assert(openrar_archive_add_files_file(nullptr, srcs, names, 1, 3, 2) == RAR_ERR_INVALID_ARG);
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, names, 0, 3, 2) ==
           RAR_ERR_INVALID_ARG);
    const char* null_names[1] = {nullptr};
    assert(openrar_archive_add_files_file(arc_u8.c_str(), srcs, null_names, 1, 3, 2) ==
           RAR_ERR_INVALID_ARG);

    // Missing archive → RAR_ERR_IO.
    const std::string missing = (dir / "nope.rar").u8string();
    assert(openrar_archive_delete_entries_file(missing.c_str(), idx, 1) == RAR_ERR_IO);
    assert(openrar_archive_add_files_file(missing.c_str(), srcs, names, 1, 3, 2) == RAR_ERR_IO);
    std::cout << "[PASS] validation_errors\n";
}

// ── 9. Comment (CMT) preserved, QuickOpen stripped ───────────────────────────
static void test_comment_preserved_qo_stripped() {
    std::cout << "Starting test_comment_preserved_qo_stripped...\n" << std::flush;
    const fs::path dir = make_scratch_dir("cmt_qo");

    // CMT: write_batch_add with a comment, then delete through the export.
    const fs::path cmt_arc = dir / "cmt.rar";
    {
        const fs::path src = dir / "c_src.bin";
        write_bytes(src, make_pattern(2048, 11));
        std::vector<ArchiveMutator::PreparedAdd> prepared;
        ArchiveMutator::PreparedAdd p;
        p.entry_name = "f.bin";
        p.src_path = src;
        assert(ArchiveMutator::prepare_add_file(src, "f.bin", 3, "", p));
        prepared.push_back(std::move(p));
        const std::vector<openrar::core::byte> comment = {'h', 'i', ' ', 'c', 'm', 't'};
        assert(
            ArchiveMutator::write_batch_add(cmt_arc, prepared, {}, "", false, {}, false, comment));
    }
    assert(has_service(cmt_arc, "CMT"));
    const uint32_t idx0[1] = {0};
    assert(openrar_archive_delete_entries_file(cmt_arc.u8string().c_str(), idx0, 1) == RAR_OK);
    assert(has_service(cmt_arc, "CMT")); // comment survived the rewrite

    // Same for the add path.
    const fs::path cmt_arc2 = dir / "cmt2.rar";
    {
        const fs::path src = dir / "c_src2.bin";
        write_bytes(src, make_pattern(2048, 12));
        std::vector<ArchiveMutator::PreparedAdd> prepared;
        ArchiveMutator::PreparedAdd p;
        p.entry_name = "f.bin";
        p.src_path = src;
        assert(ArchiveMutator::prepare_add_file(src, "f.bin", 3, "", p));
        prepared.push_back(std::move(p));
        const std::vector<openrar::core::byte> comment = {'c', 'm', 't', '2'};
        assert(
            ArchiveMutator::write_batch_add(cmt_arc2, prepared, {}, "", false, {}, false, comment));
    }
    const fs::path add_src = dir / "add.bin";
    write_bytes(add_src, {'A'});
    const std::string add_src_u8 = add_src.u8string();
    const std::string cmt2_arc_u8 = cmt_arc2.u8string();
    const char* srcs[1] = {add_src_u8.c_str()};
    const char* names[1] = {"add.bin"};
    assert(openrar_archive_add_files_file(cmt2_arc_u8.c_str(), srcs, names, 1, 3, 2) == RAR_OK);
    assert(has_service(cmt_arc2, "CMT"));

    // QO: always stripped by the rewrite, file entries preserved.
    const fs::path qo_arc = create_qo_archive(dir, "qo.rar");
    assert(has_service(qo_arc, "QO"));
    assert(handle_names(qo_arc) == std::vector<std::string>({"kept.bin"})); // QO never listed
    assert(openrar_archive_delete_entries_file(qo_arc.u8string().c_str(), idx0, 1) == RAR_OK);
    assert(!has_service(qo_arc, "QO"));
    std::cout << "[PASS] comment_preserved_qo_stripped\n";
}

// ── 10. Directory record add (non-recursive) ─────────────────────────────────
static void test_add_directory_record() {
    std::cout << "Starting test_add_directory_record...\n" << std::flush;
    const fs::path dir = make_scratch_dir("dir_record");
    const fs::path arc = create_plain_archive(dir, "d.rar", {{"f.txt", {'f'}}});
    const fs::path sub = dir / "subdir";
    fs::create_directories(sub);
    write_bytes(sub / "inner.txt", {'i'}); // must NOT be picked up

    const std::string sub_u8 = sub.u8string();
    const std::string dir_arc_u8 = arc.u8string();
    const char* srcs[1] = {sub_u8.c_str()};
    const char* names[1] = {"subdir/"};
    assert(openrar_archive_add_files_file(dir_arc_u8.c_str(), srcs, names, 1, 3, 2) == RAR_OK);
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    uint32_t count = 0;
    void* ents = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    assert(openrar_archive_handle_list(h, &count, &ents, &paths, &paths_sz) == RAR_OK);
    auto* e = static_cast<const openrar_archive_entry_t*>(ents);
    assert(count == 2);
    // Order: original entry first, directory appended.
    const char* p = static_cast<const char*>(paths);
    assert(std::string(p + e[1].path_offset, e[1].path_len) == "subdir/");
    assert(e[1].is_dir == 1);
    openrar_archive_list_free(ents, paths, paths_sz);
    openrar_archive_close(h);
    std::cout << "[PASS] add_directory_record\n";
}

// ── 11. C++ wrapper ergonomics (delete_entries / add_files) ──────────────────
static void test_cpp_wrapper() {
    std::cout << "Starting test_cpp_wrapper...\n" << std::flush;
    const fs::path dir = make_scratch_dir("wrapper");
    const std::vector<uint8_t> a = {'h', 'i'}, b = {'b', 'y', 'e'};
    const fs::path arc = create_plain_archive(dir, "wrap.rar", {{"a.txt", a}, {"b.txt", b}});

    openrar::delete_entries(arc, {0});
    auto entries = openrar::list_archive_file(arc);
    assert(entries.size() == 1 && entries[0].path == "b.txt");

    const fs::path src = dir / "c.txt";
    write_bytes(src, {'n', 'e', 'w'});
    openrar::add_files(arc, {{src, "c.txt"}}, openrar::AddOptions{3, 4});
    entries = openrar::list_archive_file(arc);
    assert(entries.size() == 2 && entries[1].path == "c.txt");
    {
        openrar::ArchiveHandle h(arc);
        assert(h.extract(1) == std::vector<uint8_t>({'n', 'e', 'w'}));
    }

    // Wrapper errors surface as exceptions with the DLL detail.
    bool threw = false;
    try {
        openrar::delete_entries(arc, {99});
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("out of range") != std::string::npos;
    }
    assert(threw);
    std::cout << "[PASS] cpp_wrapper\n";
}

// ── 12. Plan/Schedule/Execute separation unit tests ──────────────────────────
static void test_mutator_plan_single_file_stored() {
    std::cout << "Starting test_mutator_plan_single_file_stored...\n" << std::flush;
    std::vector<openrar::compress::EntryPlan> reqs(1);
    reqs[0].method = 0;
    reqs[0].is_dir = false;
    auto plan = openrar::compress::CompressPlan::plan_entries(reqs, false);
    assert(plan.entries.size() == 1);
    assert(plan.entries[0].decision == openrar::compress::EntryDecision::Stored);
    assert(!plan.entries[0].is_solid_chain);
    std::cout << "[PASS] MutatorPlan_SingleFileStored\n";
}

static void test_mutator_plan_single_file_compressed() {
    std::cout << "Starting test_mutator_plan_single_file_compressed...\n" << std::flush;
    std::vector<openrar::compress::EntryPlan> reqs(1);
    reqs[0].method = 3;
    reqs[0].is_dir = false;
    auto plan = openrar::compress::CompressPlan::plan_entries(reqs, false);
    assert(plan.entries.size() == 1);
    assert(plan.entries[0].decision == openrar::compress::EntryDecision::BlockStream);
    assert(!plan.entries[0].is_solid_chain);
    std::cout << "[PASS] MutatorPlan_SingleFileCompressed\n";
}

static void test_mutator_plan_solid_chain_three_files() {
    std::cout << "Starting test_mutator_plan_solid_chain_three_files...\n" << std::flush;
    std::vector<openrar::compress::EntryPlan> reqs(3);
    reqs[0].method = 3; reqs[0].is_dir = false;
    reqs[1].method = 0; reqs[1].is_dir = false; // stored
    reqs[2].method = 3; reqs[2].is_dir = false;
    auto plan = openrar::compress::CompressPlan::plan_entries(reqs, /*solid_mode=*/true);
    assert(plan.entries.size() == 3);
    assert(plan.entries[0].decision == openrar::compress::EntryDecision::BlockStream);
    assert(!plan.entries[0].is_solid_chain); // first entry starts chain
    assert(plan.entries[1].decision == openrar::compress::EntryDecision::Stored);
    assert(!plan.entries[1].is_solid_chain); // stored entry never solid
    assert(plan.entries[2].decision == openrar::compress::EntryDecision::BlockStream);
    assert(plan.entries[2].is_solid_chain);  // second compressed entry continues chain
    std::cout << "[PASS] MutatorPlan_SolidChain_ThreeFiles\n";
}

static void test_mutator_plan_output_bit_identical() {
    std::cout << "Starting test_mutator_plan_output_bit_identical...\n" << std::flush;
    const fs::path fixtures_root = fs::path(OPENRAR_SOURCE_DIR) / "tests" / "fixtures" / "golden" / "writer";
    const fs::path golden_solid = fixtures_root / "writer_solid=solid=1=comp=m3.rar";
    const fs::path golden_sha = fixtures_root / "writer_solid=solid=1=comp=m3.rar.sha256";
    if (!fs::exists(golden_solid) || !fs::exists(golden_sha)) {
        std::cout << "[SKIP] golden solid archive not found\n";
        return;
    }

    std::ifstream sf(golden_sha);
    std::string expected_sha;
    sf >> expected_sha;

    std::ifstream gf(golden_solid, std::ios::binary);
    std::vector<uint8_t> gbytes((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());
    uint8_t digest[32];
    openrar::crypto::Sha256::compute(gbytes.data(), gbytes.size(), digest);
    char hex[65];
    for (size_t i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    assert(std::string(hex) == expected_sha);

    openrar::archive::ArchiveReader r;
    assert(r.open(golden_solid));
    assert(r.entries().size() >= 2);
    for (const auto& e : r.entries()) assert(r.test_entry(e));

    std::cout << "[PASS] MutatorPlan_OutputBitIdentical\n";
}

static void test_execution_plan_workspace_gating() {
    std::cout << "Starting test_execution_plan_workspace_gating...\n" << std::flush;
    std::vector<openrar::compress::EntryPlan> reqs;
    {
        openrar::compress::EntryPlan ep;
        ep.is_dir = false;
        ep.method = 0; // Stored
        ep.raw_size = 5000;
        reqs.push_back(ep);
    }
    {
        openrar::compress::EntryPlan ep;
        ep.is_dir = false;
        ep.method = 3; // Normal (2 MB dict)
        ep.raw_size = 10000;
        reqs.push_back(ep);
    }
    {
        openrar::compress::EntryPlan ep;
        ep.is_dir = true;
        ep.method = 0;
        ep.raw_size = 0;
        reqs.push_back(ep);
    }
    {
        openrar::compress::EntryPlan ep;
        ep.is_dir = false;
        ep.method = 5; // Best (16 MB dict)
        ep.raw_size = 50000;
        reqs.push_back(ep);
    }

    auto cp = openrar::compress::CompressPlan::plan_entries(reqs, false, 3);
    auto ep = openrar::compress::ExecutionPlan::from_compress_plan(cp);

    assert(ep.entries.size() == 4);
    // Entry 0: Stored
    assert(ep.entries[0].decision == openrar::compress::EntryDecision::Stored);
    assert(ep.entries[0].estimated_workspace_bytes == 5000);

    // Entry 1: Method 3 (8 MB dict)
    assert(ep.entries[1].decision == openrar::compress::EntryDecision::BlockStream);
    assert(ep.entries[1].dict_size == 0x800000ULL);
    uint64_t expected_m3 = (0x800000ULL * 5) + (6ULL * 1024ULL * 1024ULL);
    assert(ep.entries[1].estimated_workspace_bytes == expected_m3);

    // Entry 2: Directory (0 bytes -> 1 byte floor)
    assert(ep.entries[2].decision == openrar::compress::EntryDecision::Stored);
    assert(ep.entries[2].estimated_workspace_bytes == 1);

    // Entry 3: Method 5 (64 MB dict)
    assert(ep.entries[3].decision == openrar::compress::EntryDecision::BlockStream);
    assert(ep.entries[3].dict_size == 0x4000000ULL);
    uint64_t expected_m5 = (0x4000000ULL * 5) + (6ULL * 1024ULL * 1024ULL);
    assert(ep.entries[3].estimated_workspace_bytes == expected_m5);

    // Cumulative and peak
    assert(ep.peak_entry_workspace == expected_m5);
    assert(ep.estimated_workspace_bytes == (5000 + expected_m3 + 1 + expected_m5));

    std::cout << "[PASS] ExecutionPlan_WorkspaceGating\n";
}

static void test_streaming_and_spooling() {
    fs::path dir = make_scratch_dir("spooling");
    std::error_code ec;

    // 1. Large stored file (17 MiB, method 0) -> direct stream without buffering
    fs::path f_store = dir / "store_17m.dat";
    {
        std::ofstream fs(f_store, std::ios::binary);
        std::vector<char> chunk(1024 * 1024, 'A');
        for (int i = 0; i < 17; ++i) {
            fs.write(chunk.data(), chunk.size());
        }
    }

    ArchiveMutator::PreparedAdd prep_store;
    prep_store.src_path = f_store;
    prep_store.entry_name = "store_17m.dat";
    bool ok = ArchiveMutator::prepare_add_file(f_store, "store_17m.dat", 0, "", prep_store);
    assert(ok);
    // Direct stream: payload empty and spool_path empty
    assert(prep_store.payload.empty());
    assert(prep_store.spool_path.empty());

    fs::path arc_store = dir / "store.rar";
    std::vector<ArchiveMutator::PreparedAdd> batch_store;
    batch_store.push_back(std::move(prep_store));
    ok = ArchiveMutator::write_batch_add(arc_store, batch_store);
    assert(ok);
    assert(fs::exists(arc_store));

    // Verify extraction of stored file
    engine::ArchiveReader reader_store;
    assert(reader_store.open(arc_store));
    assert(reader_store.entries().size() == 1);
    fs::path out_store = dir / "store_out.dat";
    assert(reader_store.extract_entry(reader_store.entries()[0], out_store));
    assert(fs::file_size(out_store) == 17 * 1024 * 1024);

    // 2. Large encrypted spooled file (> 16 MiB pseudo-random data with method 1)
    fs::path f_rand = dir / "rand_17m.dat";
    {
        std::ofstream fs(f_rand, std::ios::binary);
        std::vector<uint32_t> buf(256 * 1024);
        uint32_t state = 123456789;
        for (int i = 0; i < 17; ++i) { // 17 MiB total
            for (size_t j = 0; j < buf.size(); ++j) {
                state = state * 1664525u + 1013904223u;
                buf[j] = state;
            }
            fs.write(reinterpret_cast<const char*>(buf.data()), buf.size() * sizeof(uint32_t));
        }
    }

    ArchiveMutator::PreparedAdd prep_enc;
    prep_enc.src_path = f_rand;
    prep_enc.entry_name = "rand_17m.dat";
    std::string psw = "SpoolPassword999";
    ok = ArchiveMutator::prepare_add_file(f_rand, "rand_17m.dat", 1, psw, prep_enc);
    assert(ok);
    assert(prep_enc.payload.empty());
    assert(!prep_enc.spool_path.empty());
    assert(fs::exists(prep_enc.spool_path));

    // Verify spool file has data
    {
        std::ifstream sf(prep_enc.spool_path, std::ios::binary);
        std::vector<char> header_bytes(64);
        sf.read(header_bytes.data(), header_bytes.size());
        assert(sf.gcount() == 64);
    }

    fs::path arc_enc = dir / "enc.rar";
    std::filesystem::path spool_temp_path = prep_enc.spool_path;
    std::vector<ArchiveMutator::PreparedAdd> batch_enc;
    batch_enc.push_back(std::move(prep_enc));
    ok = ArchiveMutator::write_batch_add(arc_enc, batch_enc, {}, psw);
    assert(ok);
    assert(fs::exists(arc_enc));

    // SpoolFileGuard must have cleaned up the spool file
    assert(!fs::exists(spool_temp_path));

    // Verify extraction with password
    engine::ArchiveReader reader_enc;
    assert(reader_enc.open(arc_enc, psw));
    assert(reader_enc.entries().size() == 1);
    fs::path out_enc = dir / "enc_out.dat";
    assert(reader_enc.extract_entry(reader_enc.entries()[0], out_enc, psw));
    assert(fs::file_size(out_enc) == 17 * 1024 * 1024);

    // Verify bit-identical match between original rand file and extracted file
    {
        std::ifstream f1(f_rand, std::ios::binary);
        std::ifstream f2(out_enc, std::ios::binary);
        std::vector<char> b1(64 * 1024), b2(64 * 1024);
        while (f1.read(b1.data(), b1.size())) {
            assert(f2.read(b2.data(), b2.size()));
            assert(std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
        }
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] StreamingAndSpooling\n";
}

static void test_deferred_store_and_adaptive_clamping() {
    fs::path dir = make_scratch_dir("deferred_and_adaptive");
    std::error_code ec;

    // 1. Adaptive clamping:
    // Create a 32 KiB file
    fs::path f_32k = dir / "f_32k.bin";
    std::vector<uint8_t> data_32k = make_pattern(32 * 1024, 42);
    write_bytes(f_32k, data_32k);

    // Auto window_log2 (0) with method 3 -> clamps to 128 KiB (floor)
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_32k, "f_32k.bin", 3, "", prep,
                                                   openrar::archive::time_flags::MTIME, 0, false, false, false);
        assert(ok);
        assert(prep.fb.win_size == 128 * 1024);
    }

    // Explicit window_log2 = 2 (256 KiB) with method 3 -> honors 256 KiB
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_32k, "f_32k.bin", 3, "", prep,
                                                   openrar::archive::time_flags::MTIME, 2, false, false, false);
        assert(ok);
        assert(prep.fb.win_size == 256 * 1024);
    }

    // Auto window_log2 (0) with method 5 -> clamps to 128 KiB
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_32k, "f_32k.bin", 5, "", prep,
                                                   openrar::archive::time_flags::MTIME, 0, false, false, false);
        assert(ok);
        assert(prep.fb.win_size == 128 * 1024);
    }

    // Solid archive with auto window_log2 (0) and method 3 -> does NOT clamp, stays 8 MiB
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_32k, "f_32k.bin", 3, "", prep,
                                                   openrar::archive::time_flags::MTIME, 0, false, false, true);
        assert(ok);
        assert(prep.fb.win_size == 8 * 1024 * 1024);
    }

    // Solid archive with auto window_log2 (0) and method 5 -> does NOT clamp, stays 64 MiB
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_32k, "f_32k.bin", 5, "", prep,
                                                   openrar::archive::time_flags::MTIME, 0, false, false, true);
        assert(ok);
        assert(prep.fb.win_size == 64 * 1024 * 1024);
    }

    // 1 MiB file with auto window_log2 (0) and method 5 -> clamps to 1 MiB
    fs::path f_1m = dir / "f_1m.bin";
    std::vector<uint8_t> data_1m = make_pattern(1024 * 1024, 99);
    write_bytes(f_1m, data_1m);
    {
        ArchiveMutator::PreparedAdd prep;
        bool ok = ArchiveMutator::prepare_add_file(f_1m, "f_1m.bin", 5, "", prep,
                                                   openrar::archive::time_flags::MTIME, 0, false, false, false);
        assert(ok);
        assert(prep.fb.win_size == 1024 * 1024);
    }

    // 2. Deferred CRC on stored file:
    // Create an 18 MiB file (> SPOOL_MEMORY_THRESHOLD = 16 MiB)
    fs::path f_18m = dir / "f_18m.bin";
    openrar::crypto::Crc32 expected_crc_calc;
    {
        std::ofstream fs(f_18m, std::ios::binary);
        std::vector<char> chunk(1024 * 1024);
        for (size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = static_cast<char>((i * 7 + 13) & 0xFF);
        }
        for (int i = 0; i < 18; ++i) {
            chunk[0] = static_cast<char>(i);
            fs.write(chunk.data(), chunk.size());
            expected_crc_calc.update(chunk.data(), chunk.size());
        }
    }
    uint32_t expected_crc = expected_crc_calc.get();

    ArchiveMutator::PreparedAdd prep_store;
    prep_store.src_path = f_18m;
    prep_store.entry_name = "f_18m.bin";
    bool ok = ArchiveMutator::prepare_add_file(f_18m, "f_18m.bin", 0, "", prep_store);
    assert(ok);
    assert(prep_store.needs_deferred_crc);
    assert(prep_store.fb.has_crc32);
    assert(prep_store.fb.data_crc32 == 0); // Placeholder before write

    fs::path arc_path = dir / "test_deferred_crc.rar";
    std::vector<ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prep_store));
    ok = ArchiveMutator::write_batch_add(arc_path, batch);
    assert(ok);
    assert(fs::exists(arc_path));

    // Verify written archive header has back-patched CRC
    engine::ArchiveReader reader;
    assert(reader.open(arc_path));
    assert(reader.entries().size() == 1);
    const auto& entry = reader.entries()[0];
    assert(entry.header.has_crc32);
    assert(entry.header.data_crc32 == expected_crc);
    assert(reader.test_entry(entry));

    // Verify extraction bit-identical
    fs::path extracted = dir / "f_18m_extracted.bin";
    assert(reader.extract_entry(entry, extracted));
    assert(fs::file_size(extracted) == 18 * 1024 * 1024);
    {
        std::ifstream f1(f_18m, std::ios::binary);
        std::ifstream f2(extracted, std::ios::binary);
        std::vector<char> b1(1024 * 1024), b2(1024 * 1024);
        while (f1.read(b1.data(), b1.size())) {
            assert(f2.read(b2.data(), b2.size()));
            assert(std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
        }
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] DeferredStoreAndAdaptiveClamping\n";
}

// ── v1.21.2: off-grid dictionary windows must be snapped to the FCI grid ────
// A library caller passing an arbitrary dict_size (e.g. 3'000'000 bytes,
// between grid steps) previously produced an encoder whose slot-table and
// match horizon were chosen from the REQUESTED window while the header
// recorded the FLOOR-QUANTIZED value — a window smaller than a distance the
// encoder actually used is undeclarable. The snap makes them agree.
static void test_off_grid_dict_snap_roundtrip() {
    std::cout << "Starting test_off_grid_dict_snap_roundtrip...\n" << std::flush;
    const fs::path dir = make_scratch_dir("off_grid_dict");
    // 3'000'000 = 2 MiB + 902'848: NOT on the FCI grid (steps of 64 KiB above
    // the 2 MiB base), and > 16 MiB spool threshold is irrelevant here — this
    // exercises the in-memory path with an explicit off-grid dict_size.
    const std::uint64_t off_grid = 3000000ULL;
    const std::vector<uint8_t> data = make_pattern(1 << 21, 11);  // 2 MiB payload
    const fs::path src = dir / "src.bin";
    write_bytes(src, data);
    const fs::path arc = dir / "offgrid.rar";

    ArchiveMutator::PreparedAdd prep;
    assert(ArchiveMutator::prepare_add_file(src, "data.bin", 3, "", prep,
                                            engine::time_flags::MTIME, off_grid));
    std::vector<ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prep));
    assert(ArchiveMutator::write_batch_add(arc, batch, {}, "", false, {}, /*solid=*/false));

    // The written header must record a value the FCI decode returns exactly:
    // base 2 MiB + fraction*(64 KiB), i.e. the snapped window — never the raw
    // off-grid request. Read it back and require a clean roundtrip, which
    // fails with "distance > window" if encoder and header disagree.
    engine::ArchiveReader reader;
    assert(reader.open(arc));
    bool found = false;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service || e.header.file_name != "data.bin") continue;
        found = true;
        const std::uint64_t win = e.header.win_size;
        const std::uint64_t base = 2ULL * 1024 * 1024;
        assert(win >= base && win <= base + 31 * (base / 32));
        assert((win - base) % (base / 32) == 0 && "header window is not on the FCI grid");
    }
    assert(found && "entry missing after off-grid dict add");
    reader.close();

    // Extract and verify byte-exactness through the quantized window.
    assert(handle_extract(arc, 0) == data && "off-grid dict roundtrip mismatch");

    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
    std::cout << "test_off_grid_dict_snap_roundtrip OK\n";
}

static void test_direct_stream_compression_and_backpatch() {
    fs::path dir = make_scratch_dir("direct_stream");
    std::error_code ec;

    // 1. Large compressible file (> 16 MiB SPOOL_MEMORY_THRESHOLD)
    // 18 MiB repetitive data
    fs::path f_18m = dir / "f_18m_compress.bin";
    openrar::crypto::Crc32 expected_crc_calc;
    {
        std::ofstream fs(f_18m, std::ios::binary);
        std::vector<char> chunk(1024 * 1024);
        for (size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = static_cast<char>((i % 256) ^ 0x5A);
        }
        for (int i = 0; i < 18; ++i) {
            fs.write(chunk.data(), chunk.size());
            expected_crc_calc.update(chunk.data(), chunk.size());
        }
    }
    uint32_t expected_crc = expected_crc_calc.get();

    ArchiveMutator::PreparedAdd prep;
    prep.src_path = f_18m;
    prep.entry_name = "f_18m_compress.bin";
    bool ok = ArchiveMutator::prepare_add_file(f_18m, "f_18m_compress.bin", 3, "", prep,
                                               openrar::archive::time_flags::MTIME, 0, false, false, false,
                                               /*direct_stream=*/true);
    assert(ok);
    assert(prep.needs_direct_stream);
    assert(prep.spool_path.empty());
    assert(prep.payload.empty());
    assert(prep.fb.pack_size == 0); // Placeholder
    assert(prep.fb.data_crc32 == 0); // Placeholder

    fs::path arc_path = dir / "direct_stream.rar";
    std::vector<ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prep));
    ok = ArchiveMutator::write_batch_add(arc_path, batch);
    assert(ok);
    assert(fs::exists(arc_path));

    // Verify written archive with ArchiveReader
    {
        engine::ArchiveReader reader;
        assert(reader.open(arc_path));
        assert(reader.entries().size() == 1);
        const auto& entry = reader.entries()[0];
        assert(entry.header.has_crc32);
        assert(entry.header.data_crc32 == expected_crc);
        assert(entry.header.unp_size == 18 * 1024 * 1024);
        assert(entry.header.pack_size < 18 * 1024 * 1024);
        assert(entry.header.pack_size > 0);
        assert(reader.test_entry(entry));

        fs::path extracted = dir / "extracted_18m.bin";
        assert(reader.extract_entry(entry, extracted));
        assert(fs::file_size(extracted) == 18 * 1024 * 1024);

        std::ifstream f1(f_18m, std::ios::binary);
        std::ifstream f2(extracted, std::ios::binary);
        std::vector<char> b1(1024 * 1024), b2(1024 * 1024);
        while (f1.read(b1.data(), b1.size())) {
            assert(f2.read(b2.data(), b2.size()));
            assert(std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
        }
    }

    // 2. Incompressible file (> 16 MiB) with compression method 1 to verify store fallback
    fs::path f_rand = dir / "rand_17m.bin";
    openrar::crypto::Crc32 rand_crc_calc;
    {
        std::ofstream fs(f_rand, std::ios::binary);
        std::vector<uint32_t> buf(256 * 1024);
        uint32_t state = 987654321;
        for (int i = 0; i < 17; ++i) { // 17 MiB random data
            for (size_t j = 0; j < buf.size(); ++j) {
                state = state * 1664525u + 1013904223u;
                buf[j] = state;
            }
            fs.write(reinterpret_cast<const char*>(buf.data()), buf.size() * sizeof(uint32_t));
            rand_crc_calc.update(buf.data(), buf.size() * sizeof(uint32_t));
        }
    }
    uint32_t rand_crc = rand_crc_calc.get();

    ArchiveMutator::PreparedAdd prep_rand;
    prep_rand.src_path = f_rand;
    prep_rand.entry_name = "rand_17m.bin";
    ok = ArchiveMutator::prepare_add_file(f_rand, "rand_17m.bin", 1, "", prep_rand,
                                          openrar::archive::time_flags::MTIME, 0, false, false, false,
                                          /*direct_stream=*/true);
    assert(ok);
    assert(prep_rand.needs_direct_stream);

    fs::path arc_fallback = dir / "store_fallback.rar";
    std::vector<ArchiveMutator::PreparedAdd> batch_fallback;
    batch_fallback.push_back(std::move(prep_rand));
    ok = ArchiveMutator::write_batch_add(arc_fallback, batch_fallback);
    assert(ok);
    assert(fs::exists(arc_fallback));

    {
        engine::ArchiveReader reader;
        assert(reader.open(arc_fallback));
        assert(reader.entries().size() == 1);
        const auto& entry = reader.entries()[0];
        assert(entry.header.method == 0); // Downgraded to store because compression expanded
        assert(entry.header.unp_size == 17 * 1024 * 1024);
        assert(entry.header.pack_size == 17 * 1024 * 1024);
        assert(entry.header.data_crc32 == rand_crc);
        assert(reader.test_entry(entry));

        fs::path extracted_rand = dir / "extracted_rand.bin";
        assert(reader.extract_entry(entry, extracted_rand));
        assert(fs::file_size(extracted_rand) == 17 * 1024 * 1024);
    }

    // 3. Test openrar_archive_create_file on 18 MiB file (using direct streaming through DLL)
    fs::path arc_dll = dir / "dll_direct.rar";
    std::string s_src = f_18m.string();
    const char* src_p = s_src.c_str();
    const char* arc_n = "dll_18m.bin";
    int rc = openrar_archive_create_file(arc_dll.string().c_str(), &src_p, &arc_n, 1, 3, 0);
    assert(rc == RAR_OK);
    assert(fs::exists(arc_dll));
    {
        engine::ArchiveReader reader;
        assert(reader.open(arc_dll));
        assert(reader.entries().size() == 1);
        const auto& entry = reader.entries()[0];
        assert(entry.header.has_crc32);
        assert(entry.header.data_crc32 == expected_crc);
        assert(reader.test_entry(entry));
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] DirectStreamCompressionAndBackpatch\n";
}

// ── ADS children die with their file: delete must not orphan STM children ────
// Fixture: victim.bin + trailing child STM service (NTFS ADS shape:
// HFL_CHILD|HFL_INHERITED) + survivor.txt. Extraction pairs child services
// with the immediately preceding file entry, so a delete that keeps the STM
// would reattach victim.bin's stream onto survivor.txt (v1.21.1 fix).
static fs::path create_ads_archive(const fs::path& dir, const char* name) {
    const std::vector<uint8_t> victim = make_pattern(1024, 7);
    const std::vector<uint8_t> keep = make_pattern(1024, 8);
    const std::vector<uint8_t> stm_payload = {'Z', 'o', 'n', 'e', '.', 'I', 'd'};
    fs::path arc = dir / name;
    openrar::io::FileStream out;
    assert(out.open(arc, openrar::io::FileMode::CreateAlways));
    using openrar::format::HeaderWriter;
    assert(HeaderWriter::write_signature(out));
    openrar::format::MainBlock mb;
    assert(HeaderWriter::write_main_block(out, mb));

    auto write_entry = [&](openrar::format::FileBlock fb, const std::vector<uint8_t>& payload,
                           uint64_t extra_flags) {
        fb.has_crc32 = true;
        openrar::crypto::Crc32 crc;
        crc.update(payload.data(), payload.size());
        fb.data_crc32 = crc.get();
        assert(HeaderWriter::write_file_block(out, fb, extra_flags));
        assert(out.write(payload.data(), payload.size()) == payload.size());
    };

    openrar::format::FileBlock victim_fb;
    victim_fb.file_name = "victim.bin";
    victim_fb.unp_size = victim.size();
    victim_fb.pack_size = static_cast<openrar::core::int64>(victim.size());
    victim_fb.attributes = 0x20;
    victim_fb.method = 0;
    victim_fb.win_size = 0;
    write_entry(victim_fb, victim, 0);

    openrar::format::FileBlock stm;
    stm.is_service = true;
    stm.service_type = "STM";
    stm.file_name = "STM";
    stm.sub_data.assign({':', 'a', 'd', 's'});
    stm.unp_size = stm_payload.size();
    stm.pack_size = static_cast<openrar::core::int64>(stm_payload.size());
    stm.attributes = 0x20;
    stm.method = 0;
    stm.win_size = 0;
    stm.unp_ver = 0;
    write_entry(stm, stm_payload,
                openrar::format::HFL_CHILD | openrar::format::HFL_INHERITED);

    openrar::format::FileBlock keep_fb;
    keep_fb.file_name = "survivor.txt";
    keep_fb.unp_size = keep.size();
    keep_fb.pack_size = static_cast<openrar::core::int64>(keep.size());
    keep_fb.attributes = 0x20;
    keep_fb.method = 0;
    keep_fb.win_size = 0;
    write_entry(keep_fb, keep, 0);

    openrar::format::EndArcBlock eb;
    assert(HeaderWriter::write_end_block(out, eb));
    out.close();
    return arc;
}

static void test_delete_removes_ads_children() {
    std::cout << "Starting test_delete_removes_ads_children...\n" << std::flush;
    const fs::path dir = make_scratch_dir("ads_orphan");
    const fs::path arc = create_ads_archive(dir, "ads.rar");

    // Precondition: reader sees victim.bin, the STM child, survivor.txt.
    {
        engine::ArchiveReader reader;
        assert(reader.open(arc));
        size_t stm_count = 0;
        for (const auto& e : reader.entries()) {
            if (e.header.is_service && e.header.service_type == "STM") ++stm_count;
        }
        assert(stm_count == 1);
        reader.close();
    }

    assert(engine::ArchiveMutator::delete_entries(arc, {"victim.bin"}));

    // Postcondition: the STM child died with its file.
    engine::ArchiveReader reader;
    assert(reader.open(arc));
    std::vector<std::string> names;
    size_t stm_count = 0;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service && e.header.service_type == "STM") ++stm_count;
        if (!e.header.is_service) names.push_back(e.header.file_name);
    }
    reader.close();
    assert(names == std::vector<std::string>({"survivor.txt"}));
    assert(stm_count == 0 && "orphaned STM child survived the delete");
    std::cout << "test_delete_removes_ads_children OK\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr under ctest (piped stdio).
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    test_delete_roundtrip();
    test_delete_removes_ads_children();
    test_solid_orphan_delete_refused();
    test_solid_suffix_and_whole_run_delete();
    test_locked_volume_hp_refused();
    test_add_replace_u_semantics();
    test_solid_append_new_name();
    test_add_atomicity();
    test_busy_handle_collision();
    test_validation_errors();
    test_comment_preserved_qo_stripped();
    test_add_directory_record();
    test_cpp_wrapper();
    test_mutator_plan_single_file_stored();
    test_mutator_plan_single_file_compressed();
    test_mutator_plan_solid_chain_three_files();
    test_mutator_plan_output_bit_identical();
    test_execution_plan_workspace_gating();
    test_streaming_and_spooling();
    test_deferred_store_and_adaptive_clamping();
    test_direct_stream_compression_and_backpatch();
    test_off_grid_dict_snap_roundtrip();
    std::cout << "ALL MUTATION TESTS PASSED\n";
    return 0;
}

