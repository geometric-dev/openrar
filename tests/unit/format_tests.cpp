#include "../../src/core/types.hpp"
#include "../../src/core/vint.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/format/headers.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/header_reader.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>
#include <doctest.h>
#include "../support/test_runner.hpp"
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;
using namespace openrar::format;

void test_archive_roundtrip() {
    std::filesystem::path test_arc = "build/test_format.rar";
    // ctest runs us with cwd=build/, so the relative path may land in a
    // directory that does not exist yet — create it rather than assert-fail.
    // error_code overloads: a stale lock on the file must fail the open below
    // with a clear assert, not throw an uncaught filesystem_error (0xC0000409).
    std::error_code fs_ec;
    std::filesystem::create_directories(test_arc.parent_path(), fs_ec);
    std::filesystem::remove(test_arc, fs_ec);

    // 1. Write archive
    {
        io::FileStream out;
        bool ok = out.open(test_arc, io::FileMode::CreateAlways);
        REQUIRE(ok);

        // Signature
        ok = HeaderWriter::write_signature(out);
        REQUIRE(ok);

        // Main block with locator
        MainBlock mb;
        mb.arc_flags = MHFL_SOLID;
        mb.has_locator = true;
        mb.locator_qo_offset = 12345;
        mb.locator_rr_offset = 67890;
        ok = HeaderWriter::write_main_block(out, mb);
        REQUIRE(ok);

        // File block
        FileBlock fb;
        fb.file_name = "docs/readme.txt";
        fb.unp_size = 100;
        fb.pack_size = 100;
        fb.attributes = 0x20; // FILE_ATTRIBUTE_ARCHIVE
        fb.has_crc32 = true;
        fb.data_crc32 = 0x12345678;
        fb.has_blake2sp = true;
        for (size_t i = 0; i < 32; ++i) fb.blake2sp[i] = static_cast<core::byte>(i);
        fb.mtime_win = 133000000000000000ULL;
        fb.method = 0;   // store
        fb.win_size = 0; // store (no dict)
        fb.host_os = 0;  // Windows
        ok = HeaderWriter::write_file_block(out, fb);
        REQUIRE(ok);

        // Write 100 bytes payload
        core::byte payload[100];
        std::memset(payload, 'A', 100);
        out.write(payload, 100);

        // End block
        EndArcBlock eb;
        eb.end_flags = 0;
        ok = HeaderWriter::write_end_block(out, eb);
        REQUIRE(ok);
    }

    // 2. Read archive back and verify
    {
        io::FileStream in;
        bool ok = in.open(test_arc, io::FileMode::ReadOnly);
        REQUIRE(ok);

        // Verify signature
        REQUIRE(HeaderReader::read_signature(in));

        // Read Main block
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_MAIN);
        REQUIRE(data_size == 0);

        MainBlock read_mb;
        REQUIRE(HeaderReader::parse_main_header(body.data(), body.size(), read_mb));
        REQUIRE(read_mb.arc_flags == MHFL_SOLID);
        REQUIRE(read_mb.has_locator);
        REQUIRE(read_mb.locator_qo_offset == 12345);
        REQUIRE(read_mb.locator_rr_offset == 67890);

        // Read File block
        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_FILE);
        REQUIRE(data_size == 100);

        FileBlock read_fb;
        REQUIRE(HeaderReader::HeaderReader::parse_file_header(body.data(), body.size(), read_fb));
        REQUIRE(read_fb.file_name == "docs/readme.txt");
        REQUIRE(read_fb.unp_size == 100);
        REQUIRE(read_fb.pack_size == 100);
        REQUIRE((read_fb.has_crc32 && read_fb.data_crc32 == 0x12345678));
        REQUIRE(read_fb.has_blake2sp);
        for (size_t i = 0; i < 32; ++i) {
            REQUIRE(read_fb.blake2sp[i] == static_cast<core::byte>(i));
        }
        REQUIRE(read_fb.mtime_win == 133000000000000000ULL);
        REQUIRE(read_fb.method == 0);
        REQUIRE(read_fb.win_size == 0);

        // Skip payload
        in.seek(data_size, io::SeekOrigin::Current);

        // Read End block
        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_ENDARC);
        REQUIRE(data_size == 0);

        EndArcBlock read_eb;
        REQUIRE(HeaderReader::parse_end_header(body.data(), body.size(), read_eb));
        REQUIRE(read_eb.end_flags == 0);
    }

    std::filesystem::remove(test_arc);
    std::cout << "[PASS] RAR 5.0 Header Serialization & Parsing Roundtrip\n";
}

// Regression: malformed header bodies (attacker-controlled vints in the
// extra area and name length) must neither read out of bounds nor hang.
// Valid records must still parse identically to before the hardening.
void test_malformed_headers_safe() {
    auto vintbytes = [](core::uint64 v) {
        std::vector<core::byte> out;
        core::push_vint(out, v);
        return out;
    };

    auto craft_file_body =
        [](core::uint64 head_flags, const std::vector<core::byte>& extra_area,
           const std::vector<core::byte>& name_bytes) -> std::vector<core::byte> {
        std::vector<core::byte> body;
        core::push_vint(body, HEAD_FILE);
        core::push_vint(body, head_flags);
        if (head_flags & HFL_EXTRA) core::push_vint(body, extra_area.size());
        core::push_vint(body, 0); // file_flags
        core::push_vint(body, 0); // unp_size
        core::push_vint(body, 0); // attributes
        core::push_vint(body, 0); // comp_info: method 0 -> win_size 0
        core::push_vint(body, 0); // host_os
        core::push_vint(body, name_bytes.size());
        body.insert(body.end(), name_bytes.begin(), name_bytes.end());
        body.insert(body.end(), extra_area.begin(), extra_area.end());
        return body;
    };

    // 1. Valid BLAKE2sp hash record (rec_size 34 = type + hash_type + digest)
    //    must parse exactly as before the bounds hardening.
    {
        std::vector<core::byte> extra = vintbytes(34);
        extra.push_back(FHEXTRA_HASH);
        extra.push_back(0); // hash_type BLAKE2sp
        for (int i = 0; i < 32; ++i) extra.push_back(static_cast<core::byte>(i));
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        REQUIRE(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        REQUIRE(fb.has_blake2sp);
        for (int i = 0; i < 32; ++i) REQUIRE(fb.blake2sp[i] == static_cast<core::byte>(i));
    }

    // 2. Oversized rec_size truncates the actual record: must not read past
    //    rec_end (old code memcpy'd 32 bytes beyond the body buffer).
    {
        std::vector<core::byte> extra = vintbytes(34);
        extra.push_back(FHEXTRA_HASH);
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        REQUIRE(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        REQUIRE(!fb.has_blake2sp);
    }

    // 3. 2^64-1 record size must terminate (overflow-safe clamp), not wrap.
    //    0x7F is a genuinely unknown file-extra type (0x01 is FHEXTRA_CRYPT).
    {
        std::vector<core::byte> extra = vintbytes(~core::uint64(0));
        extra.push_back(0x7F); // unknown record type
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        REQUIRE(HeaderReader::parse_file_header(body.data(), body.size(), fb));
    }
    // 3b. H-15: a truncated FHEXTRA_CRYPT record (no salt/IV/check) must
    //     fail the header instead of yielding zero-filled key material.
    {
        std::vector<core::byte> extra = vintbytes(~core::uint64(0));
        extra.push_back(FHEXTRA_CRYPT);
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        REQUIRE(!HeaderReader::parse_file_header(body.data(), body.size(), fb));
    }
    // 3c. M-11: a declared extra area larger than the room after the fixed
    //     fields is malformed, not silently re-anchored.
    {
        std::vector<core::byte> extra = vintbytes(1);
        extra.push_back(0x7F);
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        // Re-encode the extra_size vint (3rd field) as 100: overlaps the body.
        body[2] = 100;
        FileBlock fb;
        REQUIRE(!HeaderReader::parse_file_header(body.data(), body.size(), fb));
    }

    // 4. Near-2^64 name length must be rejected, not terminate the process.
    {
        std::vector<core::byte> body;
        core::push_vint(body, HEAD_FILE);
        core::push_vint(body, 0);                     // no extra, no data
        core::push_vint(body, 0);                     // file_flags
        core::push_vint(body, 0);                     // unp_size
        core::push_vint(body, 0);                     // attributes
        core::push_vint(body, 0);                     // comp_info
        core::push_vint(body, 0);                     // host_os
        core::push_vint(body, core::uint64(1) << 60); // name_len huge
        FileBlock fb;
        REQUIRE(!HeaderReader::parse_file_header(body.data(), body.size(), fb));
    }

    // 5. Main header: 2^64-1 record size in the locator loop must terminate too.
    {
        std::vector<core::byte> extra = vintbytes(~core::uint64(0));
        extra.push_back(0x01);
        std::vector<core::byte> body;
        core::push_vint(body, HEAD_MAIN);
        core::push_vint(body, HFL_EXTRA);
        core::push_vint(body, extra.size());
        core::push_vint(body, 0); // arc_flags
        body.insert(body.end(), extra.begin(), extra.end());
        MainBlock mb;
        REQUIRE(HeaderReader::parse_main_header(body.data(), body.size(), mb));
    }

    std::cout << "[PASS] Malformed header bodies are bounded and safe\n";
}


// Table flavor round-trip: unp_ver 1 + FCI_RAR5_COMPAT is RAR7 dictionary
// sizing over the RAR5 algorithm (430 tables); only unp_ver 1 WITHOUT the
// compat flag is the RAR7 algorithm (446 tables). Mirrors unrar archive_reader.cpp.
void test_extra_dist_flavor_from_comp_info() {
    struct Case {
        core::uint32 unp_ver;
        bool extra_dist;
        core::uint64 win;
        bool win_roundtrips; // false: below the 128 KiB header minimum
    } cases[] = {
        {0, false, 0x400000, true},
        {1, false, 8ULL << 30, true}, // what our encoder writes for > 4 GiB
        {1, true, 8ULL << 30, true},  // RAR7-algorithm member (RAR 7 -md8g)
        {1, true, 0x400000, true},    // flavor is independent of the window size
        {1, false, 0x5000000, true},  // non-pow2 (fractional) RAR7 sizing, RAR5 algo
        // The compat flag must not depend on the window reaching 128 KiB: a
        // library caller can pair unp_ver 1 with a tiny explicit dictionary.
        {1, false, 100000, false},
        {1, false, 0x1FFFF, false},
        {1, false, 0, false},
        {1, true, 100000, false}, // RAR7-algorithm member keeps the flag clear
    };
    for (const Case& c : cases) {
        FileBlock fb;
        fb.file_name = "f";
        fb.method = 3;
        fb.unp_size = 1;
        fb.pack_size = 1;
        fb.unp_ver = c.unp_ver;
        fb.win_size = c.win;
        fb.extra_dist = c.extra_dist;
        std::vector<core::byte> blk = HeaderWriter::serialize_file_block(fb);
        // Skip CRC32 + size vint to reach the body.
        size_t off = 4, n = 0;
        core::uint64 body_size = 0;
        REQUIRE(core::read_vint(blk.data() + off, blk.size() - off, body_size, n));
        off += n;
        FileBlock back;
        REQUIRE(HeaderReader::parse_file_header(blk.data() + off, static_cast<size_t>(body_size),
                                                back));
        REQUIRE(back.unp_ver == c.unp_ver);
        REQUIRE(back.extra_dist == (c.unp_ver == 1 && c.extra_dist));
        if (c.win_roundtrips) REQUIRE(back.win_size == c.win);
    }
    std::cout << "[PASS] ExtraDist table flavor follows FCI_RAR5_COMPAT, not the window\n";
}

// Regression (report M11): FHEXTRA_VERSION (0x04) collided with the
// 0x04-compat redirect branch, so version records the writer emits were
// never parsed back (has_file_version stayed false). The reader now probes
// the version shape ([flags vint == 0][version vint] filling the record)
// before falling back to the legacy redirect parse. Both shapes must
// roundtrip in one extra area.
void test_version_redir_type_collision() {
    std::filesystem::path test_arc = "build/test_version_collision.rar";
    std::error_code fs_ec;
    std::filesystem::create_directories(test_arc.parent_path(), fs_ec);
    std::filesystem::remove(test_arc, fs_ec);
    {
        io::FileStream out;
        REQUIRE(out.open(test_arc, io::FileMode::CreateAlways));
        REQUIRE(HeaderWriter::write_signature(out));
        MainBlock mb;
        REQUIRE(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "versioned.bin";
        fb.unp_size = 1;
        fb.pack_size = 1;
        fb.method = 0;
        fb.win_size = 0;
        fb.has_file_version = true;
        fb.file_version = 7;
        REQUIRE(HeaderWriter::write_file_block(out, fb));
        const core::byte payload[1] = {'x'};
        REQUIRE(out.write(payload, 1) == 1);

        FileBlock redir;
        redir.file_name = "legacy.link";
        redir.unp_size = 0;
        redir.pack_size = -1;
        redir.method = 0;
        redir.win_size = 0;
        redir.redir_type = 1; // UNIXSYMLINK-shape legacy record
        redir.redir_target = "versioned.bin";
        REQUIRE(HeaderWriter::write_file_block(out, redir));

        EndArcBlock eb;
        REQUIRE(HeaderWriter::write_end_block(out, eb));
    }
    {
        io::FileStream in;
        REQUIRE(in.open(test_arc, io::FileMode::ReadOnly));
        REQUIRE(HeaderReader::read_signature(in));
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_MAIN);

        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_FILE);
        REQUIRE(data_size == 1);
        FileBlock fb;
        REQUIRE(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        REQUIRE((fb.has_file_version && fb.file_version == 7));
        {
            core::byte skip[1];
            REQUIRE(in.read(skip, 1) == 1);
        } // consume payload

        REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        REQUIRE(type == HEAD_FILE);
        FileBlock redir;
        REQUIRE(HeaderReader::parse_file_header(body.data(), body.size(), redir));
        REQUIRE((redir.redir_type == 1 && redir.redir_target == "versioned.bin"));
        REQUIRE(!redir.has_file_version);
    }
    std::error_code rm_ec;
    std::filesystem::remove(test_arc, rm_ec);
    std::cout << "[PASS] FHEXTRA_VERSION / 0x04-compat REDIR collision roundtrip\n";
}

// Regression (report L1): the MHEXTRA_LOCATOR record's Size field must cover
// type + flags + payload and exclude the size vint itself (1 + 1 + 10 + 10 =
// 22, matching the single-offset branches' 12). parse_main_header clamps
// rec_end to the extra-area end, so the wrong size is invisible to a
// roundtrip parse; assert the written bytes instead. With both locator
// offsets and no other extra records the extra area is exactly
// [size vint][type][flags vint][10-byte QLIST][10-byte RR] = 23 bytes at the
// end of the header body, so its first byte is the record size vint.
void test_locator_record_size_excludes_size_vint() {
    std::filesystem::path test_arc = "build/test_locator_size.rar";
    std::error_code fs_ec;
    std::filesystem::create_directories(test_arc.parent_path(), fs_ec);
    std::filesystem::remove(test_arc, fs_ec);
    {
        io::FileStream out;
        REQUIRE(out.open(test_arc, io::FileMode::CreateAlways));
        MainBlock mb;
        mb.arc_flags = 0;
        mb.has_locator = true;
        mb.locator_qo_offset = 12345;
        mb.locator_rr_offset = 67890;
        REQUIRE(HeaderWriter::write_main_block(out, mb));
    }
    io::FileStream in;
    REQUIRE(in.open(test_arc, io::FileMode::ReadOnly));
    core::uint64 type = 0, flags = 0, data_size = 0;
    std::vector<core::byte> body;
    REQUIRE(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
    REQUIRE(type == HEAD_MAIN);
    REQUIRE(body.size() >= 23);
    const size_t extra_start = body.size() - 23;
    REQUIRE(body[extra_start] == static_cast<core::byte>(22));

    // The corrected record still parses back with both offsets.
    MainBlock read_mb;
    REQUIRE(HeaderReader::parse_main_header(body.data(), body.size(), read_mb));
    REQUIRE(read_mb.has_locator);
    REQUIRE(read_mb.locator_qo_offset == 12345);
    REQUIRE(read_mb.locator_rr_offset == 67890);
    std::filesystem::remove(test_arc, fs_ec);
    std::cout << "[PASS] locator record size excludes the size vint (22, not 23)\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_archive_roundtrip", test_archive_roundtrip, false},
    {"test_version_redir_type_collision", test_version_redir_type_collision, false},
    {"test_locator_record_size_excludes_size_vint", test_locator_record_size_excludes_size_vint,
     false},
    {"test_malformed_headers_safe", test_malformed_headers_safe, false},
    {"test_extra_dist_flavor_from_comp_info", test_extra_dist_flavor_from_comp_info, false}};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All Milestone 3 Header Serialization & Parsing Primitives PASSED!\n";
    return rc;
}
