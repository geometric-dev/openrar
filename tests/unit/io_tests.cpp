#include "../../src/core/types.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/motw.hpp"
#include "../../src/io/path_util.hpp"
#include "../../src/io/win32_meta.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar;

void test_file_stream() {
    std::filesystem::path test_file = "build/test_stream.tmp";
    std::filesystem::remove(test_file);

    // Write test
    {
        io::FileStream out;
        bool ok = out.open(test_file, io::FileMode::CreateAlways);
        REQUIRE(ok);

        core::byte data[256];
        for (int i = 0; i < 256; ++i) data[i] = static_cast<core::byte>(i);

        size_t written = out.write(data, 256);
        REQUIRE(written == 256);
        REQUIRE(out.size() == 256);
        REQUIRE(out.tell() == 256);

        out.seek(10, io::SeekOrigin::Begin);
        REQUIRE(out.tell() == 10);

        out.truncate(128);
        REQUIRE(out.size() == 128);
    }

    // Read test
    {
        io::FileStream in;
        bool ok = in.open(test_file, io::FileMode::ReadOnly);
        REQUIRE(ok);
        REQUIRE(in.size() == 128);

        core::byte buf[128];
        size_t read_bytes = in.read(buf, 128);
        REQUIRE(read_bytes == 128);

        for (int i = 0; i < 128; ++i) {
            REQUIRE(buf[i] == static_cast<core::byte>(i));
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
        REQUIRE(ok);
        const char* text = "original bytes";
        REQUIRE(out.write(text, std::strlen(text)) == std::strlen(text));
    }

    // Same name again: CreateNew must fail ...
    {
        io::FileStream out;
        bool ok = out.open(test_file, io::FileMode::CreateNew);
        REQUIRE(!ok);
    }

    // ... and the pre-existing content must be intact.
    {
        io::FileStream in;
        REQUIRE(in.open(test_file, io::FileMode::ReadOnly));
        core::byte buf[64] = {};
        size_t got = in.read(buf, sizeof(buf));
        REQUIRE(got == std::strlen("original bytes"));
        REQUIRE(std::memcmp(buf, "original bytes", got) == 0);
    }

    std::filesystem::remove(test_file);
    std::cout << "[PASS] FileMode::CreateNew fail-if-exists (no truncation)\n";
}

void test_path_utils() {
    // Normalization
    REQUIRE(io::normalize_separators("foo\\bar/baz\\qux") == "foo/bar/baz/qux");

    // Sanitization against path traversal
    REQUIRE(io::sanitize_archive_path("C:\\foo\\bar.txt") == "foo/bar.txt");
    REQUIRE(io::sanitize_archive_path("/etc/passwd") == "etc/passwd");
    REQUIRE(io::sanitize_archive_path("a/b/../c/./d.txt") == "a/c/d.txt");
    REQUIRE(io::sanitize_archive_path("../../../danger.txt") == "danger.txt");
    REQUIRE(io::sanitize_archive_path("..\\..\\evil.exe") == "evil.exe");
    REQUIRE(io::sanitize_archive_path("..") == "");
    REQUIRE(io::sanitize_archive_path("dir\\..\\..\\up") == "up");
    REQUIRE((io::sanitize_archive_path("\\\\server\\share\\x") == "/server/share/x" ||
             io::sanitize_archive_path("\\\\server\\share\\x") == "server/share/x"));

    // M10: Windows-specific hazards must not reach the filesystem verbatim.
    REQUIRE(io::sanitize_archive_path("NUL") == "_NUL");
    REQUIRE(io::sanitize_archive_path("con") == "_con");
    REQUIRE(io::sanitize_archive_path("aux.txt") == "_aux.txt");
    REQUIRE(io::sanitize_archive_path("COM1/data.bin") == "_COM1/data.bin");
    REQUIRE(io::sanitize_archive_path("com10.txt") == "com10.txt"); // not reserved
    REQUIRE(io::sanitize_archive_path("file.txt:stream") == "file.txt_stream");
    REQUIRE(io::sanitize_archive_path("trailing.") == "trailing");
    REQUIRE(io::sanitize_archive_path("trailing . .") == "trailing");
    REQUIRE(io::sanitize_archive_path("...") == "_");
    REQUIRE(io::sanitize_archive_path("normal_name-v1.2.txt") == "normal_name-v1.2.txt");

    // Hardened character set and device sanitization tests
    REQUIRE(io::sanitize_archive_path("foo<bar>baz:1?2*3|4.txt") == "foo_bar_baz_1_2_3_4.txt");
    REQUIRE(io::sanitize_archive_path("foo\x01\x1f.txt") == "foo__.txt");
    std::string null_str = std::string("bad.exe\0.txt", sizeof("bad.exe\0.txt") - 1);
    REQUIRE(io::sanitize_archive_path(null_str) == "bad.exe_.txt");
    REQUIRE(io::sanitize_archive_path("CON .txt") == "_CON .txt");
    REQUIRE(io::sanitize_archive_path("aux.tar.gz") == "_aux.tar.gz");
    REQUIRE(io::sanitize_archive_path("Nul . .") == "_Nul");

    // Lexical containment verification (pure algorithmic, zero disk syscalls)
    REQUIRE(io::is_lexically_contained("C:/root/sub/file.txt", "C:/root"));
    REQUIRE(io::is_lexically_contained("C:/root/file.txt", "C:/root"));
    REQUIRE(io::is_lexically_contained("C:/root", "C:/root"));
    REQUIRE(!io::is_lexically_contained("C:/root/../escape.txt", "C:/root"));
    REQUIRE(!io::is_lexically_contained("C:/root/sub/../../escape.txt", "C:/root"));
    REQUIRE(!io::is_lexically_contained("D:/other/file.txt", "C:/root"));
    REQUIRE(!io::is_lexically_contained("../escape.txt", "root"));
    REQUIRE(io::is_lexically_contained("root/sub/file.txt", "root"));

    // Format archive path switches (-ep)
    std::string full = "C:/dev/project/src/main.cpp";
    std::string base = "C:/dev/project";

    // -ep: skip whole path
    REQUIRE(io::format_archive_path(full, base, io::ExcludePathMode::SkipWholePath) == "main.cpp");

    // -ep1: base path
    REQUIRE(io::format_archive_path(full, base, io::ExcludePathMode::BasePath) == "src/main.cpp");

    // -ep2: full without drive
    REQUIRE(io::format_archive_path(full, base, io::ExcludePathMode::SaveFullPathNoDrive) ==
            "dev/project/src/main.cpp");

    // -ep3: absolute path
    REQUIRE(io::format_archive_path(full, base, io::ExcludePathMode::AbsPath) ==
            "C:/dev/project/src/main.cpp");

    // Wildcard matching
    REQUIRE(io::wildcard_match("*.txt", "readme.txt"));
    REQUIRE(io::wildcard_match("*.TXT", "readme.txt", false)); // case insensitive
    REQUIRE(!io::wildcard_match("*.TXT", "readme.txt", true)); // case sensitive
    REQUIRE(io::wildcard_match("data??.bin", "data01.bin"));
    REQUIRE(!io::wildcard_match("data??.bin", "data1.bin"));
    REQUIRE(io::wildcard_match("*", "anything.anything"));

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
    REQUIRE(wrote_stream);

    // Read back alternate data streams
    std::vector<io::StreamEntry> streams;
    bool read_ok = io::read_alternate_streams(test_file, streams);
    REQUIRE(read_ok);
    REQUIRE(!streams.empty());

    bool found_ads = false;
    for (const auto& s : streams) {
        if (s.name == ":CustomADS") {
            found_ads = true;
            REQUIRE(s.data.size() == std::strlen(ads_content));
            REQUIRE(std::memcmp(s.data.data(), ads_content, s.data.size()) == 0);
        }
    }
    REQUIRE(found_ads);

    std::filesystem::remove(test_file);
    std::cout << "[PASS] Windows NTFS Alternate Data Streams (-os)\n";
#else
    std::cout << "[SKIP] NTFS Alternate Streams (non-Windows)\n";
#endif
}

// v1.27 M4 (plan tests 9/10/12): the MotW value functions — parse/generate
// contracts and the zone-name normalizer. Platform-independent surface of
// the local-generation policy (SECURITY_ARCHITECTURE 4.3).
void test_motw_values() {
    // Zone-name normalization: any legal spelling of the zone stream.
    REQUIRE(io::is_zone_stream_name(":Zone.Identifier"));
    REQUIRE(io::is_zone_stream_name(":zone.identifier"));
    REQUIRE(io::is_zone_stream_name(":ZoNe.IdEnTiFiEr"));
    REQUIRE(io::is_zone_stream_name(":Zone.Identifier:$DATA"));
    REQUIRE(!io::is_zone_stream_name(":Zone.Identifier:$DATA:$DATA"));
    REQUIRE(!io::is_zone_stream_name(":logo.png"));
    REQUIRE(!io::is_zone_stream_name("Zone.Identifier")); // no leading colon
    REQUIRE(!io::is_zone_stream_name(""));

    // parse_zone_id: canonical content, case-insensitive key, no header.
    int zone = -1;
    const char* canon = "[ZoneTransfer]\r\nZoneId=3\r\n";
    REQUIRE(io::parse_zone_id(std::vector<core::byte>(canon, canon + strlen(canon)), zone));
    REQUIRE(zone == 3);
    const char* lower = "zoneid=2";
    REQUIRE(io::parse_zone_id(std::vector<core::byte>(lower, lower + strlen(lower)), zone));
    REQUIRE(zone == 2);
    const char* four = "[ZoneTransfer]\nZoneId=4\nHostUrl=http://x";
    REQUIRE(io::parse_zone_id(std::vector<core::byte>(four, four + strlen(four)), zone));
    REQUIRE(zone == 4);
    // Malformed / hostile: no digits, out of range, garbage, oversized.
    const char* empty_val = "ZoneId=";
    REQUIRE(!io::parse_zone_id(std::vector<core::byte>(empty_val, empty_val + 7), zone));
    const char* out_of_range = "ZoneId=9";
    REQUIRE(!io::parse_zone_id(std::vector<core::byte>(out_of_range, out_of_range + 8), zone));
    const char* overflow = "ZoneId=555555555555555555555";
    REQUIRE(
        !io::parse_zone_id(std::vector<core::byte>(overflow, overflow + strlen(overflow)), zone));
    const char* garbage = "hello world";
    REQUIRE(!io::parse_zone_id(std::vector<core::byte>(garbage, garbage + 11), zone));
    std::vector<core::byte> huge((1u << 16) + 1, 'x');
    REQUIRE(!io::parse_zone_id(huge, zone));

    // Generated content: the EXACT bytes we write — nothing else ever
    // travels (no HostUrl/ReferrerUrl; plan test motw_hosturl_never_copied).
    const auto gen = io::generate_zone_identifier_content(3);
    const char* want3 = "[ZoneTransfer]\r\nZoneId=3\r\n";
    REQUIRE(gen.size() == strlen(want3));
    REQUIRE(std::memcmp(gen.data(), want3, strlen(want3)) == 0);

    // macOS quarantine value: flag preserved, local fields present, fresh
    // UUID per call.
    const std::string q1 = io::generate_quarantine_value("0083");
    REQUIRE(!q1.empty());
    REQUIRE(q1.rfind("0083;", 0) == 0);
    REQUIRE(q1.find(";OpenRAR;") != std::string::npos);
    const std::string q2 = io::generate_quarantine_value("0083");
    REQUIRE(q1 != q2); // locally generated UUID differs

    // Unmarked file probes as unmarked.
    const std::filesystem::path probe = "build/test_motw_unmarked.tmp";
    {
        io::FileStream f;
        REQUIRE(f.open(probe, io::FileMode::CreateAlways));
        f.write("x", 1);
    }
    const io::MotwProvenance p = io::probe_archive_motw(probe);
    REQUIRE(!p.marked);
    std::filesystem::remove(probe);

    std::cout << "[PASS] motw_values: parse/generate contract + zone-name normalization\n";
}

#ifdef _WIN32
// v1.27 M4: the never-remove/never-downgrade rule on the real write path.
void test_motw_zone_write_rules() {
    const std::filesystem::path f = "build/test_motw_zone.tmp";
    std::filesystem::remove(f);
    {
        io::FileStream s;
        REQUIRE(s.open(f, io::FileMode::CreateAlways));
        s.write("body", 4);
    }
    // Fresh mark: written.
    REQUIRE(io::write_file_zone_id(f, 4));
    const auto gen4 = io::generate_zone_identifier_content(4);
    std::vector<io::StreamEntry> streams;
    REQUIRE(io::read_alternate_streams(f, streams));
    bool found = false;
    for (const auto& s : streams) {
        if (io::is_zone_stream_name(s.name)) {
            found = true;
            REQUIRE(s.data == gen4);
        }
    }
    REQUIRE(found);
    // Downgrade suppression: 4 -> 3 refused, content untouched.
    REQUIRE(!io::write_file_zone_id(f, 3));
    streams.clear();
    REQUIRE(io::read_alternate_streams(f, streams));
    for (const auto& s : streams) {
        if (io::is_zone_stream_name(s.name)) REQUIRE(s.data == gen4);
    }
    // Same zone: no rewrite needed.
    REQUIRE(!io::write_file_zone_id(f, 4));
    std::filesystem::remove(f);
    std::cout << "[PASS] motw_zone_write_rules: never removes or downgrades\n";
}
#endif

void test_reparse_point_hardening() {
#ifdef _WIN32
    // 1. Buffer too small (< 8 bytes)
    {
        core::byte tiny[4] = {0};
        io::RedirEntry redir;
        REQUIRE(!io::parse_reparse_buffer(tiny, sizeof(tiny), redir));
    }

    // 2. ReparseDataLength exceeds buffer size
    {
        core::byte bad_len[16] = {0};
        core::write_le32(bad_len, 0xA0000003);
        core::write_le16(bad_len + 4, 100); // Claims 100 bytes data length
        io::RedirEntry redir;
        REQUIRE(!io::parse_reparse_buffer(bad_len, sizeof(bad_len), redir));
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
        REQUIRE(!io::parse_reparse_buffer(oob, sizeof(oob), redir));
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
        REQUIRE(!io::parse_reparse_buffer(unaligned, sizeof(unaligned), redir));
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
        REQUIRE(io::parse_reparse_buffer(valid.data(), valid.size(), redir));
        REQUIRE(redir.type == io::RedirType::Junction);
        REQUIRE(redir.is_directory);
        REQUIRE(redir.target == "C:\\TestTarget");
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
            REQUIRE(read_ok);
            REQUIRE(read_back.type == io::RedirType::Junction);
            REQUIRE(read_back.is_directory);
            std::filesystem::remove(link_dir, ec);
        }
        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "[PASS] Reparse point parser hardening & unprivileged junction handling\n";
#else
    std::cout << "[SKIP] Reparse point hardening (non-Windows)\n";
#endif
}

#ifdef _WIN32
// A Windows scanner (Defender, Search, a backup service) holds a just-created
// file for a few milliseconds; a CreateFileW inside that window fails with
// ERROR_SHARING_VIOLATION. FileStream::open retries those two transient codes
// for a bounded time. This pins both halves of that contract: a short external
// lock must NOT fail the open, and a lock held past the retry budget must
// still fail promptly rather than retrying forever.
static void test_transient_share_lock_retry() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "openrar_io_sharelock";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path target = dir / "held.bin";

    // Pre-create so the lock can be taken on an existing file.
    {
        io::FileStream seed;
        REQUIRE(seed.open(target, io::FileMode::CreateAlways));
        REQUIRE(seed.write("seed", 4) == 4);
    }

    // Case 1: lock released well inside the retry budget -> open must succeed.
    {
        HANDLE hold = CreateFileW(target.c_str(), GENERIC_READ, 0 /* no sharing */, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(hold != INVALID_HANDLE_VALUE);
        std::thread releaser([hold]() {
            Sleep(120); // ~8x the per-attempt sleep: must be ridden out
            CloseHandle(hold);
        });
        io::FileStream f;
        const bool ok = f.open(target, io::FileMode::CreateAlways);
        releaser.join();
        REQUIRE((ok && "transient share lock should be retried, not failed"));
    }

    // Case 2: lock held well past the budget -> open must FAIL, and quickly.
    // Proves the retry is bounded and that a genuine lock still surfaces.
    {
        HANDLE hold = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(hold != INVALID_HANDLE_VALUE);
        const auto t0 = std::chrono::steady_clock::now();
        io::FileStream f;
        const bool ok = f.open(target, io::FileMode::CreateAlways);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CloseHandle(hold);
        REQUIRE((!ok && "a lock held past the retry budget must still fail"));
        REQUIRE((ms < 3000 && "retry budget must stay bounded"));
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] transient share-lock retry: brief lock ridden out, "
                 "sustained lock still fails\n";
}
#endif

static const openrar::test::Case k_cases[] = {
    {"test_file_stream", test_file_stream, false},
    {"test_create_new_fails_if_exists", test_create_new_fails_if_exists, false},
    {"test_path_utils", test_path_utils, false},
    {"test_ntfs_metadata", test_ntfs_metadata, false},
    {"test_reparse_point_hardening", test_reparse_point_hardening, false},
    {"test_motw_values", test_motw_values, false},
#ifdef _WIN32
    {"test_motw_zone_write_rules", test_motw_zone_write_rules, false},
    {"test_transient_share_lock_retry", test_transient_share_lock_retry, false},
#endif
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    if (!openrar::test::is_list_only(argc, argv) && !openrar::test::last_run_was_filtered())
        std::cout << "Running Clean-Room Milestone 2 I/O Verification...\n";
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All Milestone 2 I/O & Platform Primitives PASSED!\n";
    return rc;
}
