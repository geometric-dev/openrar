#include "../../include/openrar/openrar.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#ifdef _MSC_VER
#include <crtdbg.h>
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
    std::cout << "ALL CPP WRAPPER TESTS PASSED\n";
    return 0;
}
