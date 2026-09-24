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
        assert(ok);

        // Signature
        ok = HeaderWriter::write_signature(out);
        assert(ok);

        // Main block with locator
        MainBlock mb;
        mb.arc_flags = MHFL_SOLID;
        mb.has_locator = true;
        mb.locator_qo_offset = 12345;
        mb.locator_rr_offset = 67890;
        ok = HeaderWriter::write_main_block(out, mb);
        assert(ok);

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
        assert(ok);

        // Write 100 bytes payload
        core::byte payload[100];
        std::memset(payload, 'A', 100);
        out.write(payload, 100);

        // End block
        EndArcBlock eb;
        eb.end_flags = 0;
        ok = HeaderWriter::write_end_block(out, eb);
        assert(ok);
    }

    // 2. Read archive back and verify
    {
        io::FileStream in;
        bool ok = in.open(test_arc, io::FileMode::ReadOnly);
        assert(ok);

        // Verify signature
        assert(HeaderReader::read_signature(in));

        // Read Main block
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_MAIN);
        assert(data_size == 0);

        MainBlock read_mb;
        assert(HeaderReader::parse_main_header(body.data(), body.size(), read_mb));
        assert(read_mb.arc_flags == MHFL_SOLID);
        assert(read_mb.has_locator);
        assert(read_mb.locator_qo_offset == 12345);
        assert(read_mb.locator_rr_offset == 67890);

        // Read File block
        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_FILE);
        assert(data_size == 100);

        FileBlock read_fb;
        assert(HeaderReader::HeaderReader::parse_file_header(body.data(), body.size(), read_fb));
        assert(read_fb.file_name == "docs/readme.txt");
        assert(read_fb.unp_size == 100);
        assert(read_fb.pack_size == 100);
        assert(read_fb.has_crc32 && read_fb.data_crc32 == 0x12345678);
        assert(read_fb.has_blake2sp);
        for (size_t i = 0; i < 32; ++i) {
            assert(read_fb.blake2sp[i] == static_cast<core::byte>(i));
        }
        assert(read_fb.mtime_win == 133000000000000000ULL);
        assert(read_fb.method == 0);
        assert(read_fb.win_size == 0);

        // Skip payload
        in.seek(data_size, io::SeekOrigin::Current);

        // Read End block
        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_ENDARC);
        assert(data_size == 0);

        EndArcBlock read_eb;
        assert(HeaderReader::parse_end_header(body.data(), body.size(), read_eb));
        assert(read_eb.end_flags == 0);
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
        assert(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        assert(fb.has_blake2sp);
        for (int i = 0; i < 32; ++i) assert(fb.blake2sp[i] == static_cast<core::byte>(i));
    }

    // 2. Oversized rec_size truncates the actual record: must not read past
    //    rec_end (old code memcpy'd 32 bytes beyond the body buffer).
    {
        std::vector<core::byte> extra = vintbytes(34);
        extra.push_back(FHEXTRA_HASH);
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        assert(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        assert(!fb.has_blake2sp);
    }

    // 3. 2^64-1 record size must terminate (overflow-safe clamp), not wrap.
    {
        std::vector<core::byte> extra = vintbytes(~core::uint64(0));
        extra.push_back(0x01); // unknown record type
        std::vector<core::byte> body = craft_file_body(HFL_EXTRA, extra, {core::byte('a')});
        FileBlock fb;
        assert(HeaderReader::parse_file_header(body.data(), body.size(), fb));
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
        assert(!HeaderReader::parse_file_header(body.data(), body.size(), fb));
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
        assert(HeaderReader::parse_main_header(body.data(), body.size(), mb));
    }

    std::cout << "[PASS] Malformed header bodies are bounded and safe\n";
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
        assert(out.open(test_arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "versioned.bin";
        fb.unp_size = 1;
        fb.pack_size = 1;
        fb.method = 0;
        fb.win_size = 0;
        fb.has_file_version = true;
        fb.file_version = 7;
        assert(HeaderWriter::write_file_block(out, fb));
        const core::byte payload[1] = {'x'};
        assert(out.write(payload, 1) == 1);

        FileBlock redir;
        redir.file_name = "legacy.link";
        redir.unp_size = 0;
        redir.pack_size = -1;
        redir.method = 0;
        redir.win_size = 0;
        redir.redir_type = 1; // UNIXSYMLINK-shape legacy record
        redir.redir_target = "versioned.bin";
        assert(HeaderWriter::write_file_block(out, redir));

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }
    {
        io::FileStream in;
        assert(in.open(test_arc, io::FileMode::ReadOnly));
        assert(HeaderReader::read_signature(in));
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_MAIN);

        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_FILE);
        assert(data_size == 1);
        FileBlock fb;
        assert(HeaderReader::parse_file_header(body.data(), body.size(), fb));
        assert(fb.has_file_version && fb.file_version == 7);
        {
            core::byte skip[1];
            assert(in.read(skip, 1) == 1);
        } // consume payload

        assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
        assert(type == HEAD_FILE);
        FileBlock redir;
        assert(HeaderReader::parse_file_header(body.data(), body.size(), redir));
        assert(redir.redir_type == 1 && redir.redir_target == "versioned.bin");
        assert(!redir.has_file_version);
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
        assert(out.open(test_arc, io::FileMode::CreateAlways));
        MainBlock mb;
        mb.arc_flags = 0;
        mb.has_locator = true;
        mb.locator_qo_offset = 12345;
        mb.locator_rr_offset = 67890;
        assert(HeaderWriter::write_main_block(out, mb));
    }
    io::FileStream in;
    assert(in.open(test_arc, io::FileMode::ReadOnly));
    core::uint64 type = 0, flags = 0, data_size = 0;
    std::vector<core::byte> body;
    assert(HeaderReader::read_block_raw(in, type, flags, body, data_size) == HeaderResult::Ok);
    assert(type == HEAD_MAIN);
    assert(body.size() >= 23);
    const size_t extra_start = body.size() - 23;
    assert(body[extra_start] == static_cast<core::byte>(22));

    // The corrected record still parses back with both offsets.
    MainBlock read_mb;
    assert(HeaderReader::parse_main_header(body.data(), body.size(), read_mb));
    assert(read_mb.has_locator);
    assert(read_mb.locator_qo_offset == 12345);
    assert(read_mb.locator_rr_offset == 67890);
    std::filesystem::remove(test_arc, fs_ec);
    std::cout << "[PASS] locator record size excludes the size vint (22, not 23)\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    std::cout << "Running Clean-Room Milestone 3 Format Verification...\n";
    test_archive_roundtrip();
    test_version_redir_type_collision();
    test_locator_record_size_excludes_size_vint();
    test_malformed_headers_safe();
    std::cout << "All Milestone 3 Header Serialization & Parsing Primitives PASSED!\n";
    return 0;
}
