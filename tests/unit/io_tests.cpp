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

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "Running Clean-Room Milestone 2 I/O Verification...\n";
    test_file_stream();
    test_create_new_fails_if_exists();
    test_path_utils();
    test_ntfs_metadata();
    std::cout << "All Milestone 2 I/O & Platform Primitives PASSED!\n";
    return 0;
}
