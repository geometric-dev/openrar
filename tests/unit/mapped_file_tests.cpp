// M1 (v1.25 plan §1/§4): io::MappedFile fault-injection gate.
//
// Named tests from the plan that land here:
//   truncation_after_map_read_fails_gracefully (plan §10 test 1)
//
// The mapped read engine must NEVER crash on a file that shrinks after
// mapping: Windows catches the fault in the SEH leaf, POSIX clamps to the
// current file size via the truncation-aware re-check. Both produce a
// short/empty read the caller can treat as a clean failure.

#include "../../src/io/mapped_file.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <doctest.h>
#include "../support/test_runner.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_m1map_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void rm(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(f.good());
}

// Returns false when the OS refuses the truncation (Windows pins the size
// of a file with a mapped view: ERROR_USER_MAPPED_FILE).
bool shrink_file(const fs::path& p, unsigned long long new_size) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<long long>(new_size);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }
    const bool ok = SetEndOfFile(h) != FALSE;
    CloseHandle(h);
    return ok;
#else
    return ::truncate(p.c_str(), static_cast<off_t>(new_size)) == 0;
#endif
}

void test_map_read_basics() {
    const fs::path dir = make_dir("basics");
    const fs::path file = dir / "data.bin";
    const std::string payload(1 << 20, 'x'); // 1 MiB
    write_file(file, payload);

    io::MappedFile mf;
    REQUIRE(mf.open(file));
    REQUIRE(mf.is_open());
    REQUIRE(mf.size() == payload.size());
    REQUIRE(mf.tell() == 0);

    // Sequential ReadSource reads.
    char buf[1000];
    REQUIRE(mf.read(buf, sizeof(buf)) == sizeof(buf));
    REQUIRE(mf.tell() == sizeof(buf));
    REQUIRE(mf.seek(0, io::SeekOrigin::Begin));
    REQUIRE(mf.read(buf, 100) == 100);
    // End-relative seek is pinned to the open-time size.
    REQUIRE(mf.seek(-4, io::SeekOrigin::End));
    REQUIRE(mf.tell() == payload.size() - 4);
    REQUIRE(mf.seek(1, io::SeekOrigin::End) == false); // past the end refused
    REQUIRE(mf.tell() == payload.size() - 4);

    // Absolute-offset reads.
    char out8[8] = {};
    REQUIRE(mf.read_at(42, out8, 8) == 8);
    REQUIRE(out8[0] == 'x');
    REQUIRE(mf.read_at(payload.size(), out8, 1) == 0);     // past EOF
    REQUIRE(mf.read_at(payload.size() - 2, out8, 8) == 2); // clamped at EOF

    mf.close();
    REQUIRE(!mf.is_open());
    std::cout << "[PASS] mapped read basics (cursor, bounds, EOF clamp)\n";
    rm(dir);
}

void test_truncation_after_map_read_fails_gracefully() {
    // Plan §10 test 1: map, then shrink the file; reads past the new EOF
    // return short/empty — a clean failure, never a crash.
    const fs::path dir = make_dir("trunc");
    const fs::path file = dir / "victim.bin";
    write_file(file, std::string(1 << 20, 'A')); // 1 MiB

    io::MappedFile mf;
    REQUIRE(mf.open(file));
    REQUIRE(mf.size() == 1 << 20);

    char buf[100];
#ifdef _WIN32
    // Windows pins the size of a file with a mapped view: the OS refuses the
    // truncation outright (ERROR_USER_MAPPED_FILE), so the shrink race is
    // structurally absent there. The mapping stays fully readable and the
    // SEH leaf still guards the copy.
    {
        HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        LARGE_INTEGER li;
        li.QuadPart = 4096;
        REQUIRE(SetFilePointerEx(h, li, nullptr, FILE_BEGIN));
        REQUIRE(!SetEndOfFile(h));
        REQUIRE(GetLastError() == ERROR_USER_MAPPED_FILE);
        CloseHandle(h);
    }
    REQUIRE(mf.read_at(0, buf, sizeof(buf)) == sizeof(buf));   // mapping intact
    REQUIRE(mf.read_at((1 << 20) - 4, buf, sizeof(buf)) == 4); // tail intact
    REQUIRE(mf.read_at(1 << 20, buf, sizeof(buf)) == 0);       // past EOF clean
    mf.close();
#else
    // POSIX: shrink to 4096 bytes AFTER the map — the truncation-aware
    // pre-flight turns reads past the new EOF into short/empty results.
    REQUIRE(shrink_file(file, 4096));

    REQUIRE(mf.read_at(0, buf, sizeof(buf)) == sizeof(buf)); // surviving prefix
    REQUIRE(mf.read_at(4000, buf, sizeof(buf)) == 96);       // spanning new EOF
    REQUIRE(mf.read_at(8192, buf, sizeof(buf)) == 0);        // beyond new EOF
    REQUIRE(mf.read_at(1 << 20, buf, sizeof(buf)) == 0);

    REQUIRE(mf.seek(0, io::SeekOrigin::Begin));
    REQUIRE(mf.read(buf, sizeof(buf)) == sizeof(buf));
    mf.close();
#endif
    std::cout << "[PASS] truncation_after_map_read_fails_gracefully\n";
    rm(dir);
}

void test_unmappable_and_empty_files() {
    const fs::path dir = make_dir("failopen");
    // Missing file: open fails (buffered fallback territory).
    io::MappedFile mf;
    REQUIRE(!mf.open(dir / "does_not_exist.bin"));
    // Empty file: mapping is pointless — open fails, callers fall back.
    { std::ofstream f(dir / "empty.bin", std::ios::binary); }
    REQUIRE(!mf.open(dir / "empty.bin"));
    std::cout << "[PASS] mmap_unavailable_falls_back: unmappable/empty open fails clean\n";
    rm(dir);
}

} // namespace

static const openrar::test::Case k_cases[] = {
    {"test_map_read_basics", test_map_read_basics, false},
    {"test_truncation_after_map_read_fails_gracefully",
     test_truncation_after_map_read_fails_gracefully, false},
    {"test_unmappable_and_empty_files", test_unmappable_and_empty_files, false}};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All mapped_file_tests passed.\n";
    return rc;
}
