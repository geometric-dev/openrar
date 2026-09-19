// Metadata tests (v1.5.0) — openrar_archive_handle_entry_ex /
// openrar_archive_handle_info and the §4.3 documentation refinements.
// Pins the contracts in docs/dll-integration-spec.md §6.13: entry_ex fields
// straight from the cached headers (FILETIME timestamps, solid/encrypted/
// redir/directory flags, redirection target extra), archive-level info with
// the lazily-read CMT payload, RR size and volume provenance, and the
// buffer-handle UNSUPPORTED_FEATURE refusals.

#include "openrar/openrar_dll.h"
#include "openrar/openrar.hpp"

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/header_reader.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/recovery/recovery_writer.hpp"

#include <cassert>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    fs::path dir = fs::temp_directory_path() / "openrar_metadata_tests" / name;
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

static std::string last_error() {
    char b[512] = {};
    openrar_archive_get_error(b, sizeof(b));
    return b;
}

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

// Solid archive: head s0 + chained s1/s2 (mutator's unix-format htime).
static fs::path make_solid_archive(const fs::path& dir, const char* name) {
    fs::path arc = dir / name;
    std::vector<ArchiveMutator::PreparedAdd> prepared;
    for (int i = 0; i < 3; ++i) {
        const std::string entry = "s" + std::to_string(i) + ".bin";
        const fs::path src = dir / (std::string("src_") + entry);
        write_bytes(src, make_pattern(1u << 18, 2000u + i));
        ArchiveMutator::PreparedAdd p;
        p.entry_name = entry;
        p.src_path = src;
        assert(ArchiveMutator::prepare_add_file(src, entry, 3, "", p));
        prepared.push_back(std::move(p));
    }
    assert(ArchiveMutator::write_batch_add(arc, prepared, {}, "", false, {}, /*solid=*/true));
    return arc;
}

// Hand-built archive with a redirection entry (writer serializes
// FHEXTRA_REDIR from the FileBlock fields) and a FILETIME-format htime entry
// (the writer's mtime_win fallback), covering both htime encodings.
static fs::path create_redir_archive(const fs::path& dir, const char* name) {
    fs::path arc = dir / name;
    openrar::io::FileStream out;
    assert(out.open(arc, openrar::io::FileMode::CreateAlways));
    openrar::format::HeaderWriter::write_signature(out);
    openrar::format::MainBlock mb;
    openrar::format::HeaderWriter::write_main_block(out, mb);

    // 1) unix symlink with a target.
    openrar::format::FileBlock link;
    link.file_name = "link";
    link.redir_type = 1; // unixsymlink
    link.redir_target = "target/file.txt";
    link.unp_size = 0;
    link.pack_size = -1; // no data area
    link.attributes = 0xa0ff;
    link.host_os = 1; // Unix
    link.method = 0;
    link.win_size = 0;
    assert(openrar::format::HeaderWriter::write_file_block(out, link, 0));

    // 2) plain file whose only timestamp is a FILETIME-format mtime.
    const std::vector<uint8_t> data = {'w', 'i', 'n', 't'};
    openrar::format::FileBlock fb;
    fb.file_name = "win_time.bin";
    fb.unp_size = data.size();
    fb.pack_size = static_cast<openrar::core::int64>(data.size());
    fb.attributes = 0x20;
    fb.has_crc32 = true;
    openrar::crypto::Crc32 crc;
    crc.update(data.data(), data.size());
    fb.data_crc32 = crc.get();
    fb.method = 0;
    fb.win_size = 0;
    fb.mtime_win = 0x01D9B4C6D5A3E800ULL; // arbitrary known FILETIME
    assert(openrar::format::HeaderWriter::write_file_block(out, fb, 0));
    assert(out.write(data.data(), data.size()) == data.size());

    openrar::format::EndArcBlock eb;
    openrar::format::HeaderWriter::write_end_block(out, eb);
    out.close();
    return arc;
}

static openrar_entry_ex_t query_entry_ex(uint32_t h, uint32_t idx, void** extra,
                                         size_t* extra_len) {
    openrar_entry_ex_t ex{};
    assert(openrar_archive_handle_entry_ex(h, idx, &ex, extra, extra_len) == RAR_OK);
    return ex;
}

// ── 1. entry_ex basics: mutator archive (unix-format htime, mtime only) ──────
static void test_entry_ex_basics() {
    std::cout << "Starting test_entry_ex_basics...\n" << std::flush;
    const fs::path dir = make_scratch_dir("basics");
    const fs::path src = dir / "a.bin";
    write_bytes(src, make_pattern(4096, 7));
    const fs::path arc = dir / "a.rar";
    std::vector<ArchiveMutator::PreparedAdd> prepared;
    ArchiveMutator::PreparedAdd p;
    p.entry_name = "a.bin";
    p.src_path = src;
    assert(ArchiveMutator::prepare_add_file(src, "a.bin", 3, "", p, engine::time_flags::MTIME,
                                            /*window_log2=*/2));
    prepared.push_back(std::move(p));
    assert(ArchiveMutator::write_batch_add(arc, prepared));

    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    void* extra = nullptr;
    size_t extra_len = 0;
    const openrar_entry_ex_t ex = query_entry_ex(h, 0, &extra, &extra_len);
    assert(extra == nullptr && extra_len == 0); // no redirection
    assert(ex.attrs == 0x20);
    assert(ex.host_os == 0); // Windows host
    assert((ex.flags & OPENRAR_ENTRY_FLAG_HAS_MTIME) != 0);
    assert((ex.flags & (OPENRAR_ENTRY_FLAG_HAS_CTIME | OPENRAR_ENTRY_FLAG_HAS_ATIME)) == 0);
    assert(ex.win_size == (256u * 1024)); // window_log2 2 (create parity)
    assert(ex.version_needed == 0);       // RAR5
    assert((ex.flags & OPENRAR_ENTRY_FLAG_DIRECTORY) == 0);
    // FILETIME conversion from the stored unix mtime: within a few seconds of
    // "now" (epoch-independent check for the seconds-since-1601 conversion).
    const uint64_t unix_from_ft = ex.mtime_ft / 10000000ULL - 11644473600ULL;
    const uint64_t now_unix = static_cast<uint64_t>(time(nullptr));
    assert(unix_from_ft + 5 >= now_unix && unix_from_ft <= now_unix + 5);
#ifndef _WIN32
    assert(ex.mtime_ft % 10000000ULL == 0); // whole seconds, no ns record
#endif
    openrar_archive_close(h);
    std::cout << "[PASS] entry_ex_basics\n";
}

// ── 2. entry_ex flags: solid / encrypted / directory ─────────────────────────
static void test_entry_ex_flags() {
    std::cout << "Starting test_entry_ex_flags...\n" << std::flush;
    const fs::path dir = make_scratch_dir("flags");

    // Solid: head has no SOLID flag, chained members do.
    const fs::path solid = make_solid_archive(dir, "solid.rar");
    uint32_t h =
        openrar_archive_open_file(solid.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    void* extra = nullptr;
    size_t extra_len = 0;
    assert((query_entry_ex(h, 0, &extra, &extra_len).flags & OPENRAR_ENTRY_FLAG_SOLID) == 0);
    assert((query_entry_ex(h, 1, &extra, &extra_len).flags & OPENRAR_ENTRY_FLAG_SOLID) != 0);
    assert((query_entry_ex(h, 2, &extra, &extra_len).flags & OPENRAR_ENTRY_FLAG_SOLID) != 0);
    openrar_archive_close(h);

    // Encrypted payload (-p).
    const fs::path enc_src = dir / "enc.bin";
    write_bytes(enc_src, make_pattern(2048, 3));
    const fs::path enc = dir / "enc.rar";
    assert(ArchiveMutator::add_file_to_archive(enc, enc_src, "secret.bin", 3, {}, 0, "pw"));
    h = openrar_archive_open_file(enc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    {
        const openrar_entry_ex_t ex = query_entry_ex(h, 0, &extra, &extra_len);
        assert((ex.flags & OPENRAR_ENTRY_FLAG_ENCRYPTED) != 0);
    }
    openrar_archive_close(h);

    // Directory record.
    const fs::path dir_arc = dir / "dirs.rar";
    const fs::path sub = dir / "subdir";
    fs::create_directories(sub);
    {
        std::vector<ArchiveMutator::PreparedAdd> prepared;
        ArchiveMutator::PreparedAdd p;
        p.entry_name = "subdir/";
        p.src_path = sub;
        assert(ArchiveMutator::prepare_add_dir(sub, "subdir/", p));
        prepared.push_back(std::move(p));
        assert(ArchiveMutator::write_batch_add(dir_arc, prepared));
    }
    h = openrar_archive_open_file(dir_arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    {
        const openrar_entry_ex_t ex = query_entry_ex(h, 0, &extra, &extra_len);
        assert((ex.flags & OPENRAR_ENTRY_FLAG_DIRECTORY) != 0);
        assert(ex.win_size == 0);
    }
    openrar_archive_close(h);
    std::cout << "[PASS] entry_ex_flags\n";
}

// ── 3. entry_ex redirection + FILETIME-format htime ──────────────────────────
static void test_entry_ex_redir_and_filetime() {
    std::cout << "Starting test_entry_ex_redir_and_filetime...\n" << std::flush;
    const fs::path dir = make_scratch_dir("redir");
    const fs::path arc = create_redir_archive(dir, "redir.rar");
    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);

    // Entry 0: unix symlink — REDIR flag, type, and the target extra.
    void* extra = nullptr;
    size_t extra_len = 0;
    const openrar_entry_ex_t link = query_entry_ex(h, 0, &extra, &extra_len);
    assert((link.flags & OPENRAR_ENTRY_FLAG_REDIR) != 0);
    assert(link.redir_type == 1);
    assert(link.host_os == 1);
    assert(extra != nullptr);
    assert(std::string(static_cast<const char*>(extra), extra_len) == "target/file.txt");
    assert(strlen(static_cast<const char*>(extra)) == extra_len); // NUL-terminated
    openrar_archive_entry_ex_free(extra);

    // Entry 1: FILETIME-format htime passes through unchanged.
    const openrar_entry_ex_t wt = query_entry_ex(h, 1, &extra, &extra_len);
    assert(extra == nullptr);
    assert((wt.flags & OPENRAR_ENTRY_FLAG_HAS_MTIME) != 0);
    assert(wt.mtime_ft == 0x01D9B4C6D5A3E800ULL);

    // Invalid index and null args (handle still open here).
    assert(openrar_archive_handle_entry_ex(h, 9, nullptr, nullptr, nullptr) == RAR_ERR_INVALID_ARG);
    openrar_entry_ex_t ex{};
    void* e2 = nullptr;
    size_t l2 = 0;
    const int oor_rc = openrar_archive_handle_entry_ex(h, 42, &ex, &e2, &l2);
    assert(oor_rc == RAR_ERR_INVALID_ARG);
    assert(last_error().find("entry_index OOR") != std::string::npos);
    openrar_archive_close(h);
    std::cout << "[PASS] entry_ex_redir_and_filetime\n";
}

// ── 4. handle_info: comment (lazily read), volumes, RR ───────────────────────
static void test_info_comment_and_volumes() {
    std::cout << "Starting test_info_comment_and_volumes...\n" << std::flush;
    const fs::path dir = make_scratch_dir("info");

    // Comment archive: CMT written by write_batch_add, read lazily by info.
    const std::string comment_text = "archive comment \xc3\xa9"; // UTF-8 payload
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
        const std::vector<openrar::core::byte> comment(comment_text.begin(), comment_text.end());
        assert(
            ArchiveMutator::write_batch_add(cmt_arc, prepared, {}, "", false, {}, false, comment));
    }
    uint32_t h =
        openrar_archive_open_file(cmt_arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    openrar_archive_info_t info{};
    void* cmt = nullptr;
    size_t cmt_len = 0;
    const int info_rc = openrar_archive_handle_info(h, &info, &cmt, &cmt_len);
    std::cout << "  info rc=" << info_rc << " comment_len=" << info.comment_len
              << " cmt_len=" << cmt_len << " cmt_null=" << (cmt == nullptr)
              << " rr=" << info.recovery_size << " err=" << last_error() << "\n"
              << std::flush;
    assert(info_rc == RAR_OK);
    assert(info.comment_len == comment_text.size());
    assert(cmt != nullptr && cmt_len == comment_text.size());
    assert(std::string(static_cast<const char*>(cmt), cmt_len) == comment_text);
    assert(info.volume_count == 1 && info.volume_index == 0);
    assert(info.recovery_size == 0);
    assert((info.flags & 0x1u) == 0); // not MHFL_VOLUME
    openrar_free(cmt);
    openrar_archive_close(h);

    // No comment → absent outputs, still RAR_OK.
    const fs::path plain = dir / "plain.rar";
    {
        const fs::path src = dir / "p_src.bin";
        write_bytes(src, {'x'});
        assert(ArchiveMutator::add_file_to_archive(plain, src, "x.bin", 0));
    }
    h = openrar_archive_open_file(plain.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    assert(openrar_archive_handle_info(h, &info, &cmt, &cmt_len) == RAR_OK);
    assert(info.comment_len == 0 && cmt == nullptr && cmt_len == 0);
    openrar_archive_close(h);

    // Recovery record → recovery_size > 0.
    const fs::path rr = dir / "rr.rar";
    {
        const fs::path src = dir / "rr_src.bin";
        write_bytes(src, make_pattern(16u * 1024, 21));
        assert(ArchiveMutator::add_file_to_archive(rr, src, "rr.bin", 0));
        assert(openrar::recovery::RecoveryWriter::add_recovery_record(rr, 5));
    }
    h = openrar_archive_open_file(rr.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    assert(openrar_archive_handle_info(h, &info, &cmt, &cmt_len) == RAR_OK);
    assert(info.recovery_size > 0);
    openrar_archive_close(h);

    // Multi-volume set: volume_count matches the set size, index 0.
    const fs::path vol_src = dir / "vol_src.bin";
    write_bytes(vol_src, make_pattern(10u * 1024, 77));
    const fs::path base = dir / "set.rar";
    assert(ArchiveMutator::add_file_to_archive_vol(base, vol_src, "vol.bin", 3, 4096));
    const fs::path vol1 = dir / "set.part01.rar";
    assert(fs::exists(vol1));
    h = openrar_archive_open_file(vol1.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(h != 0);
    assert(openrar_archive_handle_info(h, &info, &cmt, &cmt_len) == RAR_OK);
    assert((info.flags & 0x1u) != 0); // MHFL_VOLUME
    assert(info.volume_index == 0);
    assert(info.volume_count >= 2);
    openrar_archive_close(h);
    std::cout << "[PASS] info_comment_and_volumes\n";
}

// ── 5. Buffer handles refuse; null args rejected ─────────────────────────────
static void test_buffer_refusal_and_validation() {
    std::cout << "Starting test_buffer_refusal_and_validation...\n" << std::flush;
    // In-memory buffer handle via the frozen buffer open.
    const std::vector<uint8_t> a = {'h', 'i'};
    const uint8_t* paths[] = {reinterpret_cast<const uint8_t*>("a.txt")};
    const uint8_t* datas[] = {a.data()};
    const size_t sizes[] = {a.size()};
    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    assert(openrar_archive_create(paths, datas, sizes, 1, 3, 2, &rar, &rar_len) == RAR_OK);
    uint32_t h = openrar_archive_open(rar, rar_len);
    assert(h != 0);
    openrar_entry_ex_t ex{};
    void* extra = nullptr;
    size_t extra_len = 0;
    assert(openrar_archive_handle_entry_ex(h, 0, &ex, &extra, &extra_len) ==
           RAR_ERR_UNSUPPORTED_FEATURE);
    assert(extra == nullptr && extra_len == 0);
    openrar_archive_info_t info{};
    void* cmt = nullptr;
    size_t cmt_len = 0;
    assert(openrar_archive_handle_info(h, &info, &cmt, &cmt_len) == RAR_ERR_UNSUPPORTED_FEATURE);
    assert(cmt == nullptr && cmt_len == 0);
    openrar_archive_close(h);
    openrar_free(rar);

    // Null out pointers.
    const fs::path dir = make_scratch_dir("validation");
    const fs::path src = dir / "s.bin";
    write_bytes(src, {'s'});
    const fs::path arc = dir / "s.rar";
    assert(ArchiveMutator::add_file_to_archive(arc, src, "s.bin", 0));
    const uint32_t fh =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    assert(fh != 0);
    assert(openrar_archive_handle_entry_ex(fh, 0, nullptr, &extra, &extra_len) ==
           RAR_ERR_INVALID_ARG);
    assert(openrar_archive_handle_info(fh, nullptr, &cmt, &cmt_len) == RAR_ERR_INVALID_ARG);
    openrar_archive_close(fh);
    std::cout << "[PASS] buffer_refusal_and_validation\n";
}

// ── 6. C++ wrapper: entry_ex / info ──────────────────────────────────────────
static void test_cpp_wrapper() {
    std::cout << "Starting test_cpp_wrapper...\n" << std::flush;
    const fs::path dir = make_scratch_dir("wrapper");
    const fs::path arc = create_redir_archive(dir, "wrap.rar");
    openrar::ArchiveHandle h(arc);
    const openrar::EntryEx ex = h.entry_ex(0);
    assert((ex.flags & OPENRAR_ENTRY_FLAG_REDIR) != 0);
    assert(ex.redir_type == 1 && ex.redir_target == "target/file.txt");
    const openrar::ArchiveInfo ai = h.info();
    assert(ai.comment.empty() && ai.volume_count == 1);
    // Buffer handle throws.
    bool threw = false;
    try {
        const std::vector<uint8_t> blob = {'h', 'i'};
        openrar::ArchiveHandle bh(blob);
        bh.entry_ex(0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] cpp_wrapper\n";
}

// ── 7. RAR7 Header and Dictionary Fraction Round-trip ───────────────────────
static void test_rar7_header_serialization() {
    std::cout << "Starting test_rar7_header_serialization...\n" << std::flush;
    for (openrar::core::uint64 win : {48ULL * 1024 * 1024, 96ULL * 1024 * 1024}) {
        openrar::format::FileBlock block;
        block.file_name = "test_rar7.bin";
        block.unp_size = 1024;
        block.pack_size = 100;
        block.method = 3;
        block.win_size = win;
        block.unp_ver = 1; // RAR7 format version
        block.has_crc32 = true;
        block.data_crc32 = 0x12345678;

        fs::path tmp = fs::temp_directory_path() / "test_rar7_hdr.bin";
        {
            openrar::io::FileStream f;
            assert(f.open(tmp, openrar::io::FileMode::CreateAlways));
            assert(openrar::format::HeaderWriter::write_file_block(f, block));
            f.close();
        }

        {
            openrar::io::FileStream f;
            assert(f.open(tmp, openrar::io::FileMode::ReadOnly));
            openrar::core::uint64 block_type = 0, block_flags = 0, data_size = 0;
            std::vector<openrar::core::byte> body;
            auto res = openrar::format::HeaderReader::read_block_raw(f, block_type, block_flags, body, data_size);
            assert(res == openrar::format::HeaderResult::Ok);
            openrar::format::FileBlock read_block;
            assert(openrar::format::HeaderReader::parse_file_header(body.data(), body.size(), read_block));
            assert(read_block.unp_ver == 1);
            assert(read_block.win_size == win);
            f.close();
        }
        std::error_code ec;
        fs::remove(tmp, ec);
    }
    std::cout << "[PASS] rar7_header_serialization\n";
}

static void test_mutator_exact_dict_serialization() {
    std::cout << "Starting test_mutator_exact_dict_serialization...\n" << std::flush;
    const fs::path dir = make_scratch_dir("exact_dict");
    const fs::path src = dir / "data.bin";
    write_bytes(src, make_pattern(4096, 42));

    const openrar::core::uint64 test_dicts[] = {
        48ULL * 1024 * 1024,
        4ULL * 1024 * 1024 * 1024
    };

    for (openrar::core::uint64 target_dict : test_dicts) {
        const fs::path arc = dir / (std::to_string(target_dict) + ".rar");
        std::vector<ArchiveMutator::PreparedAdd> prepared;
        ArchiveMutator::PreparedAdd p;
        p.entry_name = "data.bin";
        p.src_path = src;
        assert(ArchiveMutator::prepare_add_file(src, "data.bin", 3, "", p, engine::time_flags::MTIME,
                                               target_dict));
        assert(p.fb.win_size == target_dict);
        assert(p.fb.unp_ver == 1);
        prepared.push_back(std::move(p));
        assert(ArchiveMutator::write_batch_add(arc, prepared));

        // Verify exact 64-bit dictionary size in ArchiveReader header
        engine::ArchiveReader ar;
        assert(ar.open(arc));
        assert(ar.entries().size() == 1);
        assert(ar.entries()[0].header.win_size == target_dict);
        assert(ar.entries()[0].header.unp_ver == 1);

        // Verify C DLL entry_ex ABI reporting (32-bit saturation above 4 GiB)
        uint32_t h =
            openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
        assert(h != 0);
        void* extra = nullptr;
        size_t extra_len = 0;
        const openrar_entry_ex_t ex = query_entry_ex(h, 0, &extra, &extra_len);
        if (target_dict > 0xFFFFFFFFULL) {
            assert(ex.win_size == 0xFFFFFFFFu);
        } else {
            assert(ex.win_size == target_dict);
        }
        assert(ex.version_needed == 1);
        openrar_archive_close(h);
    }
    std::cout << "[PASS] mutator_exact_dict_serialization (-mdx48m and -md4g)\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr under ctest (piped stdio).
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_entry_ex_basics();
    test_entry_ex_flags();
    test_entry_ex_redir_and_filetime();
    test_info_comment_and_volumes();
    test_buffer_refusal_and_validation();
    test_cpp_wrapper();
    test_rar7_header_serialization();
    test_mutator_exact_dict_serialization();
    std::cout << "ALL METADATA TESTS PASSED\n";
    return 0;
}
