#include "../../include/openrar/openrar.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <doctest.h>
#include "../support/test_runner.hpp"
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
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
    REQUIRE(!rar.empty());
    auto entries = list_archive(rar);
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].path == "a.txt");
    REQUIRE(entries[1].path == "b/c.txt");
    auto out1 = extract_file(rar, 0);
    REQUIRE(out1 == data1);
    auto all = extract_all(rar);
    REQUIRE(all["a.txt"] == data1);
    REQUIRE(all["b/c.txt"] == data2);

    ArchiveHandle h(rar);
    auto hl = h.list();
    REQUIRE(hl.size() == 2);
    auto he = h.extract(1);
    REQUIRE(he == data2);

    std::vector<uint8_t> src = {'x', 'y', 'z', 'x', 'y', 'z', 'x', 'y', 'z'};
    auto comp = compress_block(src, 3);
    auto decomp = decompress_block(comp);
    REQUIRE(decomp == src);

    StreamEncoder se(3, 4);
    se.feed(std::vector<uint8_t>(src.begin(), src.begin() + 3));
    se.feed(std::vector<uint8_t>(src.begin() + 3, src.end()));
    auto sout = se.finish();
    REQUIRE(!sout.empty());
    auto sdec = decompress_block(sout);
    REQUIRE(sdec == src);

    REQUIRE(package_version() == OPENRAR_VERSION_STRING);

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
    REQUIRE(entries.size() == 2);
    REQUIRE(!log.calls.empty());
    REQUIRE((log.calls.back().first == rar.size() && log.calls.back().second == rar.size()));

    // File overload: streams from disk, same progress contract.
    auto rar_path = std::filesystem::temp_directory_path() / "openrar_dll_cpp_cb.rar";
    {
        std::ofstream f(rar_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rar.data()),
                static_cast<std::streamsize>(rar.size()));
    }
    CbLog file_log;
    auto file_entries = list_archive_file(rar_path, CbLog::progress, CbLog::cancel, &file_log);
    REQUIRE(file_entries.size() == 2);
    REQUIRE(file_entries[0].path == "a.txt");
    REQUIRE(file_log.calls.back().first == std::filesystem::file_size(rar_path));
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
    REQUIRE(!entries.empty());
    for (const auto& e : entries) {
        if (!e.is_dir) REQUIRE(e.is_encrypted);
    }

    // Wrong password throws with the RAR_ERR_BAD_PASSWORD code.
    bool threw_bad_password = false;
    try {
        list_archive_file(fixtures / "hello5_hp.rar", "nope");
    } catch (const std::runtime_error& ex) {
        threw_bad_password = std::string(ex.what()).find("code -7") != std::string::npos;
    }
    REQUIRE(threw_bad_password);

    // ArchiveHandle with callbacks over the open-time scan.
    std::vector<InputFile> files = {{"a.txt", {'h', 'i'}, 0}};
    auto rar = create_archive(files, {0, 4});
    CbLog log;
    ArchiveHandle h(rar, CbLog::progress, CbLog::cancel, &log);
    REQUIRE(h.list().size() == 1);
    REQUIRE(!log.calls.empty());
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
    REQUIRE(!entries.empty());
    h.test(0);
    auto dest = scratch / "hello_out.txt";
    h.extract_to_path(0, dest);
    REQUIRE(std::filesystem::file_size(dest) == entries[0].size);

    // -hp fixture: no password fails with the ENCRYPTED code, password opens.
    bool threw_encrypted = false;
    try {
        ArchiveHandle bad(fixtures / "hello5_hp.rar", nullptr);
    } catch (const std::runtime_error& ex) {
        threw_encrypted = std::string(ex.what()).find("encrypted") != std::string::npos;
    }
    REQUIRE(threw_encrypted);
    ArchiveHandle hp(fixtures / "hello5_hp.rar", "secret");
    hp.test(0);
    auto dest2 = scratch / "hello_hp_out.txt";
    hp.extract_to_path(0, dest2);
    REQUIRE(std::filesystem::file_size(dest2) > 0);

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::cout << "PASS test_cpp_wrapper_file_handle" << std::endl;
}

// v1.25 (OPENRAR_ABI_FEATURE_MMAP): random-read region export over a
// file-mode handle — stored entry, range checks, checksum contract.
static void test_cpp_wrapper_read_entry_region() {
    using namespace openrar;
    // Feature bit 16 advertised by the native library.
    REQUIRE((openrar_abi_features() & OPENRAR_ABI_FEATURE_MMAP) != 0);

    const std::filesystem::path fixtures = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";
    auto scratch = fixtures / "wrapper_mmap_scratch";
    std::filesystem::create_directories(scratch);

    // Stored fixture with a known payload.
    std::vector<uint8_t> payload = {'h', 'i', '.', 'b', 'y', 'e', '!', 0x00, 0x01, 0x02};
    const char* names[] = {"stored.bin"};
    const uint8_t* datas[] = {payload.data()};
    size_t sizes[] = {payload.size()};
    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    REQUIRE(openrar_archive_create(reinterpret_cast<const uint8_t* const*>(names),
                                   reinterpret_cast<const uint8_t* const*>(datas), sizes, 1, 0, 4,
                                   &rar, &rar_len) == 0);
    std::vector<uint8_t> rar_bytes(rar, rar + rar_len);
    openrar_free(rar);
    const std::filesystem::path arc = scratch / "stored.rar";
    {
        std::ofstream f(arc, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rar_bytes.data()),
                static_cast<std::streamsize>(rar_bytes.size()));
    }

    uint32_t h =
        openrar_archive_open_file(arc.u8string().c_str(), nullptr, nullptr, nullptr, nullptr);
    REQUIRE(h != 0);

    // Full-range read: CRC-verified, byte-identical.
    std::vector<uint8_t> full(payload.size());
    size_t written = 0;
    int rc = openrar_archive_handle_read_entry_region(
        h, 0, 0, static_cast<uint32_t>(payload.size()), full.data(), full.size(), &written);
    REQUIRE(rc == RAR_OK);
    REQUIRE(written == payload.size());
    REQUIRE(full == payload);

    // Partial range: first 3 bytes at offset 2.
    std::vector<uint8_t> part(3);
    rc = openrar_archive_handle_read_entry_region(h, 0, 2, 3, part.data(), part.size(), &written);
    REQUIRE((rc == RAR_OK && written == 3));
    REQUIRE((part[0] == payload[2] && part[1] == payload[3] && part[2] == payload[4]));

    // Out-of-range: offset beyond payload / range exceeding payload.
    REQUIRE(openrar_archive_handle_read_entry_region(h, 0, payload.size(), 1, full.data(),
                                                     full.size(), &written) == RAR_ERR_INVALID_ARG);
    REQUIRE(openrar_archive_handle_read_entry_region(h, 0, 0, 0, full.data(), full.size(),
                                                     &written) == RAR_ERR_INVALID_ARG);

    openrar_archive_close(h);
    std::error_code rm_ec;
    std::filesystem::remove_all(scratch, rm_ec);
    std::cout << "PASS test_cpp_wrapper_read_entry_region" << std::endl;
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
    REQUIRE(std::filesystem::exists(out_rar));

    openrar::ArchiveHandle h(out_rar);
    auto list = h.list();
    REQUIRE(list.size() == 2);
    REQUIRE(list[0].path == "cpp_f1.txt");
    REQUIRE(list[1].path == "sub/cpp_f2.txt");

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_cpp_wrapper_create\n";
}

static void test_cpp_wrapper_busy_reentry() {
    using namespace openrar;

    struct StreamCtx {
        uint32_t handle{0};
        int reentry_rc{0};
        bool called{false};
    };
    StreamCtx sctx;
    sctx.handle = openrar_stream_create(3, 5);
    REQUIRE(sctx.handle != 0);
    auto stream_cancel = [](void* user) -> int {
        auto* c = static_cast<StreamCtx*>(user);
        c->called = true;
        c->reentry_rc = openrar_stream_set_cancel(c->handle, nullptr, nullptr);
        return 0;
    };
    REQUIRE(openrar_stream_set_cancel(sctx.handle, stream_cancel, &sctx) == RAR_OK);
    std::vector<uint8_t> payload(4096, 0x90);
    REQUIRE(openrar_stream_feed(sctx.handle, payload.data(), payload.size()) == RAR_OK);
    REQUIRE(sctx.called);
    REQUIRE(sctx.reentry_rc == RAR_ERR_BUSY);
    uint8_t* sout = nullptr;
    size_t sout_len = 0;
    REQUIRE(openrar_stream_finish(sctx.handle, &sout, &sout_len) == RAR_OK);
    if (sout) openrar_free(sout);
    openrar_stream_free(sctx.handle);

    struct HandleCtx {
        uint32_t handle{0};
        int reentry_rc{0};
        bool called{false};
    };
    std::vector<InputFile> files = {{"a.bin", std::vector<uint8_t>(4096, 0x41), 0}};
    auto rar = create_archive(files, {0, 4});
    HandleCtx hctx;
    hctx.handle = openrar_archive_open(rar.data(), rar.size());
    REQUIRE(hctx.handle != 0);
    auto handle_progress = [](void* user, uint64_t, uint64_t) {
        auto* c = static_cast<HandleCtx*>(user);
        if (c->called) return;
        c->called = true;
        uint32_t count = 0;
        void* entries = nullptr;
        void* paths = nullptr;
        size_t paths_size = 0;
        c->reentry_rc =
            openrar_archive_handle_list(c->handle, &count, &entries, &paths, &paths_size);
    };
    const auto dest = std::filesystem::temp_directory_path() / "openrar_busy_reentry.bin";
    const std::string dest_u8 = dest.u8string();
    REQUIRE(openrar_archive_handle_extract_to_path(hctx.handle, 0, dest_u8.c_str(), handle_progress,
                                                   nullptr, &hctx) == RAR_OK);
    REQUIRE(hctx.called);
    REQUIRE(hctx.reentry_rc == RAR_ERR_BUSY);
    openrar_archive_close(hctx.handle);
    std::error_code ec;
    std::filesystem::remove(dest, ec);

    std::cout << "PASS test_cpp_wrapper_busy_reentry\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_cpp_wrapper", test_cpp_wrapper, false},
    {"test_cpp_wrapper_list_callbacks", test_cpp_wrapper_list_callbacks, false},
    {"test_cpp_wrapper_password_and_open", test_cpp_wrapper_password_and_open, false},
    {"test_cpp_wrapper_file_handle", test_cpp_wrapper_file_handle, false},
    {"test_cpp_wrapper_create", test_cpp_wrapper_create, false},
    {"test_cpp_wrapper_read_entry_region", test_cpp_wrapper_read_entry_region, false},
    {"test_cpp_wrapper_busy_reentry", test_cpp_wrapper_busy_reentry, false}};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "ALL CPP WRAPPER TESTS PASSED\n";
    return rc;
}
