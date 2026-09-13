#include "../../include/openrar/openrar.hpp"
#include <cassert>
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

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_cpp_wrapper();
    std::cout << "ALL CPP WRAPPER TESTS PASSED\n";
    return 0;
}
