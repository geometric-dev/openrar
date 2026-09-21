#include "../../include/openrar/openrar.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

static void test_cpp_wrapper() {
    using namespace openrar;
    std::vector<uint8_t> data1 = {'h', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> data2 = {'w', 'o', 'r', 'l', 'd', '!'};
    std::vector<InputFile> files = {{"a.txt", data1, 0}, {"b/c.txt", data2, 0}};
    auto rar = create_archive(files, {3, 4});
    assert(!rar.empty());
    auto entries = list_archive(rar);
    assert(entries.size() == 2);
    assert(entries[0].path == "a.txt");
    assert(entries[1].path == "b/c.txt");
    auto out1 = extract_file(rar, 0);
    assert(out1 == data1);
    auto all = extract_all(rar);
    assert(all["a.txt"] == data1);
    assert(all["b/c.txt"] == data2);

    ArchiveHandle h(rar);
    auto hl = h.list();
    assert(hl.size() == 2);
    auto he = h.extract(1);
    assert(he == data2);

    std::vector<uint8_t> src = {'x', 'y', 'z', 'x', 'y', 'z', 'x', 'y', 'z'};
    auto comp = compress_block(src, 3);
    auto decomp = decompress_block(comp);
    assert(decomp == src);

    StreamEncoder se(3, 4);
    se.feed(std::vector<uint8_t>(src.begin(), src.begin() + 3));
    se.feed(std::vector<uint8_t>(src.begin() + 3, src.end()));
    auto sout = se.finish();
    assert(!sout.empty());
    auto sdec = decompress_block(sout);
    assert(sdec == src);

    assert(package_version() == OPENRAR_VERSION_STRING);

    std::cout << "PASS test_cpp_wrapper\n";
}

// ── list_archive(_file) with progress / cancel overloads ─────────────────────
namespace {
struct CbLog {
    std::vector<std::pair<uint64_t, uint64_t>> calls;
    static void OPENRAR_DLL_CALL progress(void* user, uint64_t done, uint64_t total) {
        static_cast<CbLog*>(user)->calls.emplace_back(done, total);
    }
    static int OPENRAR_DLL_CALL cancel(void* user) {
        (void)user;
        return 0;
    }
};
} // namespace

static void test_cpp_wrapper_list_callbacks() {
    using namespace openrar;
    std::vector<InputFile> files = {{"a.txt", {'h', 'i', '!'}, 0}, {"b.txt", {'o', 'k'}, 0}};
    auto rar = create_archive(files, {0, 4});

    // Buffer overload: byte progress ending at (size, size).
    CbLog log;
    auto entries = list_archive(rar, CbLog::progress, CbLog::cancel, &log);
    assert(entries.size() == 2);
    assert(!log.calls.empty());
    assert(log.calls.back().first == rar.size() && log.calls.back().second == rar.size());

    // File overload: streams from disk, same progress contract.
    auto rar_path = std::filesystem::temp_directory_path() / "openrar_dll_cpp_cb.rar";
    {
        std::ofstream f(rar_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rar.data()),
                static_cast<std::streamsize>(rar.size()));
    }
    CbLog file_log;
    auto file_entries = list_archive_file(rar_path, CbLog::progress, CbLog::cancel, &file_log);
    assert(file_entries.size() == 2);
    assert(file_entries[0].path == "a.txt");
    assert(file_log.calls.back().first == std::filesystem::file_size(rar_path));
    std::error_code ec;
    std::filesystem::remove(rar_path, ec);
    std::cout << "PASS test_cpp_wrapper_list_callbacks\n";
}

// ── Password listing + handle open with callbacks ────────────────────────────
static void test_cpp_wrapper_password_and_open() {
    using namespace openrar;
    const std::filesystem::path fixtures = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";

    // Password listing: the -hp fixture lists with is_encrypted entries.
    auto entries = list_archive_file(fixtures / "hello5_hp.rar", "secret");
    assert(!entries.empty());
    for (const auto& e : entries) {
        if (!e.is_dir) assert(e.is_encrypted);
    }

    // Wrong password throws with the RAR_ERR_BAD_PASSWORD code.
    bool threw_bad_password = false;
    try {
        list_archive_file(fixtures / "hello5_hp.rar", "nope");
    } catch (const std::runtime_error& ex) {
        threw_bad_password = std::string(ex.what()).find("code -7") != std::string::npos;
    }
    assert(threw_bad_password);

    // ArchiveHandle with callbacks over the open-time scan.
    std::vector<InputFile> files = {{"a.txt", {'h', 'i'}, 0}};
    auto rar = create_archive(files, {0, 4});
    CbLog log;
    ArchiveHandle h(rar, CbLog::progress, CbLog::cancel, &log);
    assert(h.list().size() == 1);
    assert(!log.calls.empty());
    std::cout << "PASS test_cpp_wrapper_password_and_open\n";
}

// ── File-mode handle wrapper (open_file / extract_to_path / test) ────────────
static void test_cpp_wrapper_file_handle() {
    using namespace openrar;
    const std::filesystem::path fixtures = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";
    auto scratch = fixtures / "wrapper_file_handle_scratch";
    std::filesystem::create_directories(scratch);

    // Plain fixture: open by path, extract to disk, test.
    ArchiveHandle h(fixtures / "hello5.rar");
    auto entries = h.list();
    assert(!entries.empty());
    h.test(0);
    auto dest = scratch / "hello_out.txt";
    h.extract_to_path(0, dest);
    assert(std::filesystem::file_size(dest) == entries[0].size);

    // -hp fixture: no password fails with the ENCRYPTED code, password opens.
    bool threw_encrypted = false;
    try {
        ArchiveHandle bad(fixtures / "hello5_hp.rar", nullptr);
    } catch (const std::runtime_error& ex) {
        threw_encrypted = std::string(ex.what()).find("encrypted") != std::string::npos;
    }
    assert(threw_encrypted);
    ArchiveHandle hp(fixtures / "hello5_hp.rar", "secret");
    hp.test(0);
    auto dest2 = scratch / "hello_hp_out.txt";
    hp.extract_to_path(0, dest2);
    assert(std::filesystem::file_size(dest2) > 0);

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::cout << "PASS test_cpp_wrapper_file_handle" << std::endl;
}

static void test_cpp_wrapper_create() {
    auto temp_dir = std::filesystem::temp_directory_path() / "openrar_cpp_create_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    auto src1 = temp_dir / "cpp_f1.txt";
    auto src2 = temp_dir / "cpp_f2.txt";
    {
        std::ofstream of1(src1, std::ios::binary);
        of1 << "C++ wrapper creation test file 1";
        std::ofstream of2(src2, std::ios::binary);
        of2 << "C++ wrapper creation test file 2";
    }

    auto out_rar = temp_dir / "cpp_created.rar";
    std::vector<std::pair<std::filesystem::path, std::string>> entries = {{src1, "cpp_f1.txt"},
                                                                          {src2, "sub/cpp_f2.txt"}};

    openrar::Archive::create(out_rar, entries, 3, 0);
    assert(std::filesystem::exists(out_rar));

    openrar::ArchiveHandle h(out_rar);
    auto list = h.list();
    assert(list.size() == 2);
    assert(list[0].path == "cpp_f1.txt");
    assert(list[1].path == "sub/cpp_f2.txt");

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_cpp_wrapper_create\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_cpp_wrapper();
    test_cpp_wrapper_list_callbacks();
    test_cpp_wrapper_password_and_open();
    test_cpp_wrapper_file_handle();
    test_cpp_wrapper_create();
    std::cout << "ALL CPP WRAPPER TESTS PASSED\n";
    return 0;
}
