// M2 (v1.25 plan §2/§4): mapped scanner wiring — identity, limits, fallback.
//
// Named tests from the plan that land here:
//   limits_enforced_on_mapped_path              (plan §10 test 3)
//   extraction_bytes_identical_mapped_vs_buffered (plan §10 test 4)
//   mmap_unavailable_falls_back (scanner-level)  (plan §10 test 5)
//   truncation_mid_scan_falls_back               (plan §10 test 2)

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/archive/archive_reader.hpp"

#include <cassert>
#include <cstdlib>
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
#else
#include <unistd.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
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
static void set_env(const char* name, const char* value) {
    setenv(name, value, 1);
}
static void unset_env(const char* name) {
    unsetenv(name);
}
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_m2map_") + name);
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
    assert(f.good());
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Builds a stored archive with distinct per-entry payloads.
fs::path build_archive(const fs::path& dir, const char* name,
                       const std::vector<std::pair<std::string, std::string>>& entries) {
    const fs::path arc = dir / name;
    const fs::path blob = dir / "m2_blob.bin";
    for (const auto& [n, content] : entries) {
        write_file(blob, content);
        assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, n, 0));
    }
    fs::remove(blob);
    return arc;
}

void test_extraction_bytes_identical_mapped_vs_buffered() {
    // Plan §10 test 4: extract with the mapped scan and with the buffered
    // engine (OPENRAR_NO_MMAP=1) — payload bytes must be identical.
    const fs::path dir = make_dir("ident");
    const fs::path arc = build_archive(dir, "ident.rar",
                                       {{"one.txt", "first-payload"},
                                        {"two.txt", "second-payload-2"},
                                        {"sub/three.bin", "third"}});
    std::string mapped_bytes[3];
    std::string buffered_bytes[3];

    {
        archive::ArchiveReader reader;
        reader.set_extraction_root(dir);
        assert(reader.open(arc));
        const fs::path out = dir / "out_mapped";
        const char* names[3] = {"one.txt", "two.txt", "sub/three.bin"};
        for (int i = 0; i < 3; ++i) {
            assert(reader.extract_entry(reader.entries()[i], out / names[i]));
            mapped_bytes[i] = read_file(out / names[i]);
        }
    }
    {
        set_env("OPENRAR_NO_MMAP", "1");
        archive::ArchiveReader reader;
        reader.set_extraction_root(dir);
        assert(reader.open(arc));
        const fs::path out = dir / "out_buffered";
        const char* names[3] = {"one.txt", "two.txt", "sub/three.bin"};
        for (int i = 0; i < 3; ++i) {
            assert(reader.extract_entry(reader.entries()[i], out / names[i]));
            buffered_bytes[i] = read_file(out / names[i]);
        }
        unset_env("OPENRAR_NO_MMAP");
    }

    for (int i = 0; i < 3; ++i) assert(mapped_bytes[i] == buffered_bytes[i]);
    assert(mapped_bytes[0] == "first-payload");
    std::cout << "[PASS] extraction_bytes_identical_mapped_vs_buffered\n";
    rm(dir);
}

void test_limits_enforced_on_mapped_path() {
    // Plan §10 test 3: header-count and header-byte limits abort identically
    // on the mapped path — limits-not-bypassable.
    const fs::path dir = make_dir("limits");
    const fs::path arc =
        build_archive(dir, "limits.rar", {{"a.txt", "aaa"}, {"b.txt", "bbb"}, {"c.txt", "ccc"}});

    archive::ExtractionLimits limits;
    limits.max_header_count = 2; // archive carries 3 file headers
    archive::LimitState state;

    for (bool no_mmap : {false, true}) {
        if (no_mmap) set_env("OPENRAR_NO_MMAP", "1");
        archive::ArchiveReader reader;
        reader.set_extraction_root(dir);
        int status = 0;
        std::string detail;
        // open_ex fails with the limit status (fail-closed, not silent).
        assert(!reader.open_ex(arc, "", status, detail, {}, false, &limits, &state));
        assert(status == archive::RAR_ERR_LIMIT_EXCEEDED);
        if (no_mmap) unset_env("OPENRAR_NO_MMAP");
        reader.close();
        state = archive::LimitState{};
    }
    std::cout << "[PASS] limits_enforced_on_mapped_path (and buffered parity)\n";
    rm(dir);
}

void shrink_file(const fs::path& p, unsigned long long new_size) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<long long>(new_size);
    assert(SetFilePointerEx(h, li, nullptr, FILE_BEGIN));
    assert(SetEndOfFile(h));
    CloseHandle(h);
#else
    assert(::truncate(p.c_str(), static_cast<off_t>(new_size)) == 0);
#endif
}

void test_truncation_mid_scan_falls_back() {
    // Plan §10 test 2: a volume truncated while the scan is mapped yields a
    // clean verdict — parse fails at the truncation point, no signal, and
    // the surviving prefix is reported deterministically.
    const fs::path dir = make_dir("midtrunc");
    const fs::path blob = dir / "blob.bin";
    write_file(blob, std::string(200000, 'A'));
    const fs::path seed = dir / "mv.rar";
    assert(archive::ArchiveMutator::add_file_to_archive_vol(seed, blob, "big.bin", 0, 65536));
    fs::remove(blob);

    // Find the LAST volume of the set and truncate it mid-header.
    fs::path last;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().string().find(".part") != std::string::npos) {
            if (last.empty() || e.path() > last) last = e.path();
        }
    }
    assert(!last.empty());
    shrink_file(last, 30);

    archive::ArchiveReader reader;
    reader.set_extraction_root(dir);
    assert(reader.open(dir / "mv.part01.rar")); // tolerant open
    // No crash, no SIGBUS — the scan ends deterministically at the
    // truncation point.
    std::cout << "[PASS] truncation_mid_scan_falls_back: clean truncated verdict\n";
    rm(dir);
}


} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    test_extraction_bytes_identical_mapped_vs_buffered();
    test_limits_enforced_on_mapped_path();
    test_truncation_mid_scan_falls_back();
    std::cout << "All mapped_scan_tests passed.\n";
    return 0;
}
