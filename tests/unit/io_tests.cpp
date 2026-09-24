#include "../../src/core/types.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/path_util.hpp"
#include "../../src/io/win32_meta.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;

void test_file_stream() {
    std::filesystem::path test_file = "build/test_stream.tmp";
    std::filesystem::remove(test_file);

    // Write test
    {
        io::FileStream out;
        bool ok = out.open(test_file, io::FileMode::CreateAlways);
        assert(ok);

        core::byte data[256];
        for (int i = 0; i < 256; ++i) data[i] = static_cast<core::byte>(i);

        size_t written = out.write(data, 256);
        assert(written == 256);
        assert(out.size() == 256);
        assert(out.tell() == 256);

        out.seek(10, io::SeekOrigin::Begin);
        assert(out.tell() == 10);

        out.truncate(128);
        assert(out.size() == 128);
    }

    // Read test
    {
        io::FileStream in;
        bool ok = in.open(test_file, io::FileMode::ReadOnly);
        assert(ok);
        assert(in.size() == 128);

        core::byte buf[128];
        size_t read_bytes = in.read(buf, 128);
        assert(read_bytes == 128);

        for (int i = 0; i < 128; ++i) {
            assert(buf[i] == static_cast<core::byte>(i));
        }
    }

    std::filesystem::remove(test_file);
    std::cout << "[PASS] FileStream 64-bit Read/Write/Seek/Truncate\n";
}

void test_create_new_fails_if_exists() {
    // L8: FileMode::CreateNew must refuse an existing name (pre-planted file or
    // symlink) and must NOT truncate what is there.
    std::filesystem::path test_file = "build/test_createnew.tmp";
    std::filesystem::remove(test_file);

    // Fresh name: CreateNew succeeds.
    {
        io::FileStream out;
        bool ok = out.open(test_file, io::FileMode::CreateNew);
        assert(ok);
        const char* text = "original bytes";
        assert(out.write(text, std::strlen(text)) == std::strlen(text));
    }

    // Same name again: CreateNew must fail ...
    {
        io::FileStream out;
        bool ok = out.open(test_file, io::FileMode::CreateNew);
        assert(!ok);
    }

    // ... and the pre-existing content must be intact.
    {
        io::FileStream in;
        assert(in.open(test_file, io::FileMode::ReadOnly));
        core::byte buf[64] = {};
        size_t got = in.read(buf, sizeof(buf));
        assert(got == std::strlen("original bytes"));
        assert(std::memcmp(buf, "original bytes", got) == 0);
    }

    std::filesystem::remove(test_file);
    std::cout << "[PASS] FileMode::CreateNew fail-if-exists (no truncation)\n";
}

void test_path_utils() {
    // Normalization
    assert(io::normalize_separators("foo\\bar/baz\\qux") == "foo/bar/baz/qux");

    // Sanitization against path traversal
    assert(io::sanitize_archive_path("C:\\foo\\bar.txt") == "foo/bar.txt");
    assert(io::sanitize_archive_path("/etc/passwd") == "etc/passwd");
    assert(io::sanitize_archive_path("a/b/../c/./d.txt") == "a/c/d.txt");
    assert(io::sanitize_archive_path("../../../danger.txt") == "danger.txt");
    assert(io::sanitize_archive_path("..\\..\\evil.exe") == "evil.exe");
    assert(io::sanitize_archive_path("..") == "");
    assert(io::sanitize_archive_path("dir\\..\\..\\up") == "up");
    assert(io::sanitize_archive_path("\\\\server\\share\\x") == "/server/share/x" ||
           io::sanitize_archive_path("\\\\server\\share\\x") == "server/share/x");

    // M10: Windows-specific hazards must not reach the filesystem verbatim.
    assert(io::sanitize_archive_path("NUL") == "_NUL");
    assert(io::sanitize_archive_path("con") == "_con");
    assert(io::sanitize_archive_path("aux.txt") == "_aux.txt");
    assert(io::sanitize_archive_path("COM1/data.bin") == "_COM1/data.bin");
    assert(io::sanitize_archive_path("com10.txt") == "com10.txt"); // not reserved
    assert(io::sanitize_archive_path("file.txt:stream") == "file.txt_stream");
    assert(io::sanitize_archive_path("trailing.") == "trailing");
    assert(io::sanitize_archive_path("trailing . .") == "trailing");
    assert(io::sanitize_archive_path("...") == "_");
    assert(io::sanitize_archive_path("normal_name-v1.2.txt") == "normal_name-v1.2.txt");

    // Hardened character set and device sanitization tests
    assert(io::sanitize_archive_path("foo<bar>baz:1?2*3|4.txt") == "foo_bar_baz_1_2_3_4.txt");
    assert(io::sanitize_archive_path("foo\x01\x1f.txt") == "foo__.txt");
    std::string null_str = std::string("bad.exe\0.txt", sizeof("bad.exe\0.txt") - 1);
    assert(io::sanitize_archive_path(null_str) == "bad.exe_.txt");
    assert(io::sanitize_archive_path("CON .txt") == "_CON .txt");
    assert(io::sanitize_archive_path("aux.tar.gz") == "_aux.tar.gz");
    assert(io::sanitize_archive_path("Nul . .") == "_Nul");

    // Lexical containment verification (pure algorithmic, zero disk syscalls)
    assert(io::is_lexically_contained("C:/root/sub/file.txt", "C:/root"));
    assert(io::is_lexically_contained("C:/root/file.txt", "C:/root"));
    assert(io::is_lexically_contained("C:/root", "C:/root"));
    assert(!io::is_lexically_contained("C:/root/../escape.txt", "C:/root"));
    assert(!io::is_lexically_contained("C:/root/sub/../../escape.txt", "C:/root"));
    assert(!io::is_lexically_contained("D:/other/file.txt", "C:/root"));
    assert(!io::is_lexically_contained("../escape.txt", "root"));
    assert(io::is_lexically_contained("root/sub/file.txt", "root"));

    // Format archive path switches (-ep)
    std::string full = "C:/dev/project/src/main.cpp";
    std::string base = "C:/dev/project";

    // -ep: skip whole path
    assert(io::format_archive_path(full, base, io::ExcludePathMode::SkipWholePath) == "main.cpp");

    // -ep1: base path
    assert(io::format_archive_path(full, base, io::ExcludePathMode::BasePath) == "src/main.cpp");

    // -ep2: full without drive
    assert(io::format_archive_path(full, base, io::ExcludePathMode::SaveFullPathNoDrive) ==
           "dev/project/src/main.cpp");

    // -ep3: absolute path
    assert(io::format_archive_path(full, base, io::ExcludePathMode::AbsPath) ==
           "C:/dev/project/src/main.cpp");

    // Wildcard matching
    assert(io::wildcard_match("*.txt", "readme.txt"));
    assert(io::wildcard_match("*.TXT", "readme.txt", false)); // case insensitive
    assert(!io::wildcard_match("*.TXT", "readme.txt", true)); // case sensitive
    assert(io::wildcard_match("data??.bin", "data01.bin"));
    assert(!io::wildcard_match("data??.bin", "data1.bin"));
    assert(io::wildcard_match("*", "anything.anything"));

    std::cout << "[PASS] Path Utilities & Path Traversal Sanitization\n";
}

void test_ntfs_metadata() {
#ifdef _WIN32
    std::filesystem::path test_file = "build/test_ads.tmp";
    std::filesystem::remove(test_file);

    // Create host file
    {
        io::FileStream f;
        f.open(test_file, io::FileMode::CreateAlways);
        const char* text = "host data payload";
        f.write(text, std::strlen(text));
    }

    // Write alternate data stream
    const char* ads_content = "custom stream payload 12345";
    bool wrote_stream =
        io::write_alternate_stream(test_file, ":CustomADS", ads_content, std::strlen(ads_content));
    assert(wrote_stream);

    // Read back alternate data streams
    std::vector<io::StreamEntry> streams;
    bool read_ok = io::read_alternate_streams(test_file, streams);
    assert(read_ok);
    assert(!streams.empty());

    bool found_ads = false;
    for (const auto& s : streams) {
        if (s.name == ":CustomADS") {
            found_ads = true;
            assert(s.data.size() == std::strlen(ads_content));
            assert(std::memcmp(s.data.data(), ads_content, s.data.size()) == 0);
        }
    }
    assert(found_ads);

    std::filesystem::remove(test_file);
    std::cout << "[PASS] Windows NTFS Alternate Data Streams (-os)\n";
#else
    std::cout << "[SKIP] NTFS Alternate Streams (non-Windows)\n";
#endif
}

void test_reparse_point_hardening() {
#ifdef _WIN32
    // 1. Buffer too small (< 8 bytes)
    {
        core::byte tiny[4] = {0};
        io::RedirEntry redir;
        assert(!io::parse_reparse_buffer(tiny, sizeof(tiny), redir));
    }

    // 2. ReparseDataLength exceeds buffer size
    {
        core::byte bad_len[16] = {0};
        core::write_le32(bad_len, 0xA0000003);
        core::write_le16(bad_len + 4, 100); // Claims 100 bytes data length
        io::RedirEntry redir;
        assert(!io::parse_reparse_buffer(bad_len, sizeof(bad_len), redir));
    }

    // 3. Out-of-bounds PrintNameOffset or SubstituteNameOffset
    {
        core::byte oob[32] = {0};
        core::write_le32(oob, 0xA0000003);
        core::write_le16(oob + 4, 24); // ReparseDataLength = 24
        core::write_le16(oob + 8, 0);
        core::write_le16(oob + 10, 4);
        core::write_le16(oob + 12, 100);
        core::write_le16(oob + 14, 4);
        io::RedirEntry redir;
        assert(!io::parse_reparse_buffer(oob, sizeof(oob), redir));
    }

    // 4. Odd-aligned offsets or lengths (must be 2-byte aligned for UTF-16 WCHAR)
    {
        core::byte unaligned[32] = {0};
        core::write_le32(unaligned, 0xA0000003);
        core::write_le16(unaligned + 4, 24);
        core::write_le16(unaligned + 8, 1); // unaligned offset 1!
        core::write_le16(unaligned + 10, 4);
        core::write_le16(unaligned + 12, 6);
        core::write_le16(unaligned + 14, 4);
        io::RedirEntry redir;
        assert(!io::parse_reparse_buffer(unaligned, sizeof(unaligned), redir));
    }

    // 5. Valid MountPoint buffer with \??\ NT prefix stripping
    {
        std::wstring nt_target = L"\\??\\C:\\TestTarget";
        std::wstring print_target = L"C:\\TestTarget";
        size_t sub_b = nt_target.size() * sizeof(wchar_t);
        size_t prn_b = print_target.size() * sizeof(wchar_t);

        std::vector<core::byte> valid(16 + sub_b + prn_b, 0);
        core::write_le32(valid.data(), 0xA0000003); // IO_REPARSE_TAG_MOUNT_POINT
        core::write_le16(valid.data() + 4, static_cast<core::uint16>(8 + sub_b + prn_b));
        core::write_le16(valid.data() + 6, 0);

        // SubstituteName
        core::write_le16(valid.data() + 8, 0);
        core::write_le16(valid.data() + 10, static_cast<core::uint16>(sub_b));
        // PrintName
        core::write_le16(valid.data() + 12, static_cast<core::uint16>(sub_b));
        core::write_le16(valid.data() + 14, static_cast<core::uint16>(prn_b));

        std::memcpy(valid.data() + 16, nt_target.data(), sub_b);
        std::memcpy(valid.data() + 16 + sub_b, print_target.data(), prn_b);

        io::RedirEntry redir;
        assert(io::parse_reparse_buffer(valid.data(), valid.size(), redir));
        assert(redir.type == io::RedirType::Junction);
        assert(redir.is_directory);
        assert(redir.target == "C:\\TestTarget");
    }

    // 6. Test unprivileged junction creation and read_reparse_info roundtrip
    {
        std::filesystem::path test_dir = "build/test_junc_dir";
        std::filesystem::path link_dir = "build/test_junc_link";
        std::error_code ec;
        std::filesystem::remove(link_dir, ec);
        std::filesystem::remove_all(test_dir, ec);
        std::filesystem::create_directories(test_dir, ec);

        std::filesystem::path abs_target = std::filesystem::absolute(test_dir);
        io::RedirEntry junc_entry;
        junc_entry.type = io::RedirType::Junction;
        junc_entry.target = abs_target.string();
        junc_entry.is_directory = true;

        bool created = io::create_reparse_link(link_dir, junc_entry);
        if (created) {
            io::RedirEntry read_back;
            bool read_ok = io::read_reparse_info(link_dir, read_back);
            assert(read_ok);
            assert(read_back.type == io::RedirType::Junction);
            assert(read_back.is_directory);
            std::filesystem::remove(link_dir, ec);
        }
        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "[PASS] Reparse point parser hardening & unprivileged junction handling\n";
#else
    std::cout << "[SKIP] Reparse point hardening (non-Windows)\n";
#endif
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
    std::cout << "Running Clean-Room Milestone 2 I/O Verification...\n";
    test_file_stream();
    test_create_new_fails_if_exists();
    test_path_utils();
    test_ntfs_metadata();
    test_reparse_point_hardening();
    std::cout << "All Milestone 2 I/O & Platform Primitives PASSED!\n";
    return 0;
}
