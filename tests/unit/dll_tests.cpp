#include "openrar/openrar_dll.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

static void test_version() {
    assert(openrar_version() == OPENRAR_DLL_API_VERSION);
    assert(openrar_archive_version() == 1);
    assert(openrar_package_version_string() != nullptr);
    assert(std::string(openrar_package_version_string()) == OPENRAR_VERSION_STRING);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_PACKAGE_VERSION) != 0);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_SET_LIMITS) != 0);
    std::cout << "PASS test_version\n";
}

static void test_block_codec_roundtrip() {
    const char* msg = "Hello DLL block codec! Repeat. Hello DLL block codec! Repeat.";
    size_t len = std::strlen(msg);
    uint8_t* comp = nullptr;
    size_t comp_len = 0;
    int rc = openrar_compress2(reinterpret_cast<const uint8_t*>(msg), len, &comp, &comp_len, 3,
                               1024 * 1024);
    assert(rc == 1);
    assert(comp && comp_len > 0);
    uint8_t* decomp = nullptr;
    size_t decomp_len = 0;
    rc = openrar_decompress2(comp, comp_len, &decomp, &decomp_len, 1024 * 1024);
    assert(rc == 1);
    assert(decomp_len == len);
    assert(std::memcmp(decomp, msg, len) == 0);
    openrar_free(comp);
    openrar_free(decomp);
    std::cout << "PASS test_block_codec_roundtrip\n";
}

static void test_alloc_free_alias() {
    void* p = openrar_alloc(64);
    assert(p);
    std::memset(p, 0xAB, 64);
    openrar_free(p);
    void* q = openrar_archive_alloc(32);
    assert(q);
    openrar_archive_free(q);
    std::cout << "PASS test_alloc_free_alias\n";
}

static void test_buffer_archive_create_list_extract() {
    const char* paths[] = {"hello.txt", "dir/nested.txt"};
    const uint8_t data1[] = "hello world";
    const uint8_t data2[] = "nested content here";
    const uint8_t* datas[] = {data1, data2};
    size_t sizes[] = {sizeof(data1) - 1, sizeof(data2) - 1};
    const uint8_t* path_ptrs[] = {reinterpret_cast<const uint8_t*>(paths[0]),
                                  reinterpret_cast<const uint8_t*>(paths[1])};

    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    int rc = openrar_archive_create(path_ptrs, datas, sizes, 2, 3, 4, &rar, &rar_len);
    assert(rc == 0);
    assert(rar && rar_len > 0);

    // list
    uint32_t count = 0;
    void* entries = nullptr;
    void* pbuf = nullptr;
    size_t psize = 0;
    rc = openrar_archive_list(rar, rar_len, &count, &entries, &pbuf, &psize);
    assert(rc == 0);
    assert(count == 2);
    auto* ents = static_cast<openrar_archive_entry_t*>(entries);
    const char* pstr = static_cast<const char*>(pbuf);
    assert(ents[0].path_len == std::strlen(paths[0]));
    assert(std::string(pstr + ents[0].path_offset, ents[0].path_len) == "hello.txt");
    openrar_archive_list_free(entries, pbuf, psize);

    // extract single
    uint8_t* out = nullptr;
    size_t out_len = 0;
    rc = openrar_archive_extract(rar, rar_len, 0, &out, &out_len);
    assert(rc == 0);
    assert(out_len == sizes[0]);
    assert(std::memcmp(out, data1, out_len) == 0);
    openrar_free(out);

    // extract_all
    uint8_t* buf = nullptr;
    size_t buf_sz = 0;
    uint64_t* offs = nullptr;
    uint32_t off_cnt = 0;
    rc = openrar_archive_extract_all(rar, rar_len, &buf, &buf_sz, &offs, &off_cnt);
    assert(rc == 0);
    assert(off_cnt == 2);
    assert(buf_sz == sizes[0] + sizes[1]);
    assert(offs[1] == sizes[0]);
    assert(std::memcmp(buf, data1, sizes[0]) == 0);
    assert(std::memcmp(buf + offs[2], data2, sizes[1]) == 0);
    openrar_free(buf);
    openrar_free(offs);

    // handle API
    uint32_t h = openrar_archive_open(rar, rar_len);
    assert(h != 0);
    uint32_t hc = 0;
    void* he = nullptr;
    void* hp = nullptr;
    size_t hps = 0;
    rc = openrar_archive_handle_list(h, &hc, &he, &hp, &hps);
    assert(rc == 0 && hc == 2);
    openrar_archive_list_free(he, hp, hps);
    uint8_t* hout = nullptr;
    size_t hlen = 0;
    rc = openrar_archive_handle_extract(h, 1, &hout, &hlen);
    assert(rc == 0 && hlen == sizes[1] && std::memcmp(hout, data2, hlen) == 0);
    openrar_free(hout);
    openrar_archive_close(h);

    openrar_free(rar);
    std::cout << "PASS test_buffer_archive_create_list_extract\n";
}

static void test_error_path() {
    // truncate detection
    const char* p = "a.txt";
    const uint8_t d[] = "hi";
    const uint8_t* pp[] = {reinterpret_cast<const uint8_t*>(p)};
    const uint8_t* dp[] = {d};
    size_t ss[] = {2};
    uint8_t* rar = nullptr;
    size_t rl = 0;
    int rc = openrar_archive_create(pp, dp, ss, 1, 3, 4, &rar, &rl);
    assert(rc == 0);
    // corrupt last byte
    uint32_t count = 0;
    void* e = nullptr;
    void* pb = nullptr;
    size_t ps = 0;
    std::vector<uint8_t> trunc(rar, rar + rl - 1);
    rc = openrar_archive_list(trunc.data(), trunc.size(), &count, &e, &pb, &ps);
    assert(rc != 0);
    char buf[256] = {};
    int n = openrar_archive_get_error(buf, sizeof(buf));
    (void)n;
    openrar_free(rar);
    std::cout << "PASS test_error_path\n";
}

static void test_stream_encoder() {
    uint32_t h = openrar_stream_create(3, 4);
    assert(h != 0);
    const char* chunk1 = "Hello ";
    const char* chunk2 = "stream ";
    const char* chunk3 = "world! Hello stream world! Hello stream world!";
    int rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>(chunk1), std::strlen(chunk1));
    assert(rc == 0);
    rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>(chunk2), std::strlen(chunk2));
    assert(rc == 0);
    rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>(chunk3), std::strlen(chunk3));
    assert(rc == 0);
    uint8_t* out = nullptr;
    size_t out_len = 0;
    rc = openrar_stream_finish(h, &out, &out_len);
    assert(rc == 0);
    assert(out && out_len > 0);
    // decompress and verify
    uint8_t* dec = nullptr;
    size_t dec_len = 0;
    rc = openrar_decompress(out, out_len, &dec, &dec_len);
    // stream output is block codec; for non-empty input we expect decompress to yield original
    // Our stream_encoder uses one-shot compress_buffer internally, so output is valid block
    std::string orig = std::string(chunk1) + chunk2 + chunk3;
    assert(rc == 1);
    assert(dec_len == orig.size());
    assert(std::memcmp(dec, orig.data(), dec_len) == 0);
    openrar_free(out);
    openrar_free(dec);
    openrar_stream_free(h);
    std::cout << "PASS test_stream_encoder\n";
}

static int test_cancel_cb_fire(void*) {
    return 1;
}

static void test_stream_cancel() {
    uint32_t h = openrar_stream_create(3, 4);
    assert(h != 0);
    // A cancel callback that fires must abort feed() with RAR_ERR_ABORTED
    // (L12: the callback now runs outside the stream map mutex).
    assert(openrar_stream_set_cancel(h, test_cancel_cb_fire, nullptr) == RAR_OK);
    int rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>("x"), 1);
    assert(rc == RAR_ERR_ABORTED);
    // The aborted feed consumed nothing; clear the callback and verify the
    // stream still works end-to-end.
    assert(openrar_stream_set_cancel(h, nullptr, nullptr) == RAR_OK);
    const char* msg = "cancel-after-abort still compresses";
    rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    assert(rc == 0);
    uint8_t* out = nullptr;
    size_t out_len = 0;
    rc = openrar_stream_finish(h, &out, &out_len);
    assert(rc == 0 && out && out_len > 0);
    uint8_t* dec = nullptr;
    size_t dec_len = 0;
    rc = openrar_decompress(out, out_len, &dec, &dec_len);
    assert(rc == 1 && dec_len == std::strlen(msg));
    assert(std::memcmp(dec, msg, dec_len) == 0);
    openrar_free(out);
    openrar_free(dec);
    openrar_stream_free(h);
    std::cout << "PASS test_stream_cancel\n";
}

static uint32_t g_reent_handle = 0;
static int test_cancel_cb_reent_free(void*) {
    // Re-enter the streaming API from inside the callback (L12 regression
    // guard: pre-fix this ran while g_stream_mutex was held and deadlocked
    // the non-recursive mutex).
    openrar_stream_free(g_reent_handle);
    return 1;
}

static void test_stream_cancel_reentrant() {
    uint32_t h = openrar_stream_create(3, 4);
    assert(h != 0);
    g_reent_handle = h;
    assert(openrar_stream_set_cancel(h, test_cancel_cb_reent_free, nullptr) == RAR_OK);
    int rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>("x"), 1);
    assert(rc == RAR_ERR_ABORTED);
    // Handle already freed by the callback; nothing further to release.
    std::cout << "PASS test_stream_cancel_reentrant\n";
}

static struct {
    int calls;
    uint64_t done;
    uint64_t total;
} g_prog;
static void test_progress_cb(void*, uint64_t done, uint64_t total) {
    g_prog.calls++;
    g_prog.done = done;
    g_prog.total = total;
}

static void test_stream_progress() {
    uint32_t h = openrar_stream_create(3, 4);
    assert(h != 0);
    g_prog = {0, 0, 0};
    assert(openrar_stream_set_progress(h, test_progress_cb, nullptr) == RAR_OK);
    const char* msg = "progress callback payload";
    int rc = openrar_stream_feed(h, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    assert(rc == 0);
    assert(g_prog.calls >= 1);
    assert(g_prog.done == std::strlen(msg) && g_prog.total == std::strlen(msg));
    uint8_t* out = nullptr;
    size_t out_len = 0;
    rc = openrar_stream_finish(h, &out, &out_len);
    assert(rc == 0 && out && out_len > 0);
    openrar_free(out);
    openrar_stream_free(h);
    std::cout << "PASS test_stream_progress\n";
}

// ── Listing with progress / cancel (_ex) ─────────────────────────────────────

static void test_abi_features() {
    // Additive exports do not bump the ABI version; every feature bit is set.
    assert(openrar_version() == OPENRAR_DLL_API_VERSION);
    assert(openrar_version() == 1);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_LIST_PROGRESS) != 0);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_LIST_PASSWORD) != 0);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS) != 0);
    assert((openrar_abi_features() & OPENRAR_ABI_FEATURE_REPAIR) != 0);
    std::cout << "PASS test_abi_features\n";
}

static void test_archive_repair() {
    // Null arc_path returns RAR_ERR_INVALID_ARG
    assert(openrar_archive_repair(nullptr, nullptr, nullptr, nullptr) == RAR_ERR_INVALID_ARG);
    // Non-existent path returns RAR_ERR_IO
    assert(openrar_archive_repair("non_existent_archive_file_12345.rar", nullptr, nullptr, nullptr) == RAR_ERR_IO);
    // Cancelled callback returns RAR_ERR_ABORTED
    auto cancel_now = [](void*) -> int { return 1; };
    assert(openrar_archive_repair("non_existent_archive_file_12345.rar", nullptr, cancel_now, nullptr) == RAR_ERR_ABORTED);
    std::cout << "PASS test_archive_repair\n";
}

struct ExProgressLog {
    std::vector<uint64_t> dones;
    std::vector<uint64_t> totals;
};

static void OPENRAR_DLL_CALL ex_progress_cb(void* user, uint64_t done, uint64_t total) {
    auto* log = static_cast<ExProgressLog*>(user);
    log->dones.push_back(done);
    log->totals.push_back(total);
}

static int OPENRAR_DLL_CALL ex_cancel_after_two(void* user) {
    return ++(*static_cast<int*>(user)) >= 2 ? 1 : 0;
}

// Build a two-entry archive in memory and persist it for the file API.
static std::vector<uint8_t> make_two_entry_rar() {
    const char* paths[] = {"hello.txt", "dir/nested.txt"};
    const uint8_t data1[] = "hello world";
    const uint8_t data2[] = "nested content here";
    const uint8_t* datas[] = {data1, data2};
    size_t sizes[] = {sizeof(data1) - 1, sizeof(data2) - 1};
    const uint8_t* path_ptrs[] = {reinterpret_cast<const uint8_t*>(paths[0]),
                                  reinterpret_cast<const uint8_t*>(paths[1])};
    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    int rc = openrar_archive_create(path_ptrs, datas, sizes, 2, 3, 4, &rar, &rar_len);
    assert(rc == 0 && rar && rar_len > 0);
    std::vector<uint8_t> out(rar, rar + rar_len);
    openrar_free(rar);
    return out;
}

static void test_list_file_ex_progress() {
    auto temp_dir = std::filesystem::temp_directory_path() / "openrar_dll_test_ex";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    auto out_rar_path = (temp_dir / "ex_progress.rar").string();
    std::vector<uint8_t> rar = make_two_entry_rar();
    {
        FILE* f = std::fopen(out_rar_path.c_str(), "wb");
        assert(f);
        std::fwrite(rar.data(), 1, rar.size(), f);
        std::fclose(f);
    }

    // Null callbacks: _ex is equivalent to the non-_ex export.
    uint32_t c_ex = 0;
    void *e_ex = nullptr, *p_ex = nullptr;
    size_t ps_ex = 0;
    int rc = openrar_archive_list_file_ex(out_rar_path.c_str(), &c_ex, &e_ex, &p_ex, &ps_ex,
                                          nullptr, nullptr, nullptr);
    assert(rc == 0 && c_ex == 2);
    uint32_t c_plain = 0;
    void *e_plain = nullptr, *p_plain = nullptr;
    size_t ps_plain = 0;
    rc = openrar_archive_list_file(out_rar_path.c_str(), &c_plain, &e_plain, &p_plain, &ps_plain);
    assert(rc == 0 && c_plain == 2);
    assert(ps_ex == ps_plain);
    {
        const auto* a = static_cast<const openrar_archive_entry_t*>(e_ex);
        const auto* b = static_cast<const openrar_archive_entry_t*>(e_plain);
        for (uint32_t i = 0; i < c_ex; ++i) {
            assert(a[i].path_len == b[i].path_len && a[i].path_offset == b[i].path_offset);
            assert(std::memcmp(static_cast<const char*>(p_ex) + a[i].path_offset,
                               static_cast<const char*>(p_plain) + b[i].path_offset,
                               a[i].path_len) == 0);
        }
    }
    openrar_archive_list_free(e_ex, p_ex, ps_ex);
    openrar_archive_list_free(e_plain, p_plain, ps_plain);

    // Progress: byte-based, monotonic, one final (file_size, file_size).
    ExProgressLog log;
    uint32_t c2 = 0;
    void *e2 = nullptr, *p2 = nullptr;
    size_t ps2 = 0;
    rc = openrar_archive_list_file_ex(out_rar_path.c_str(), &c2, &e2, &p2, &ps2, ex_progress_cb,
                                      nullptr, &log);
    assert(rc == 0 && c2 == 2);
    openrar_archive_list_free(e2, p2, ps2);
    assert(log.dones.size() >= 2);
    for (size_t i = 1; i < log.dones.size(); ++i) {
        assert(log.dones[i] >= log.dones[i - 1]);
        assert(log.dones[i] <= log.totals[i]);
    }
    for (uint64_t t : log.totals) assert(t == rar.size());
    assert(log.dones.back() == rar.size() && log.totals.back() == rar.size());

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_list_file_ex_progress\n";
}

static void test_list_ex_buffer() {
    std::vector<uint8_t> rar = make_two_entry_rar();

    // Buffer variant: progress parity with the file variant.
    ExProgressLog log;
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    int rc = openrar_archive_list_ex(rar.data(), rar.size(), &count, &e, &p, &ps, ex_progress_cb,
                                     nullptr, &log);
    assert(rc == 0 && count == 2);
    openrar_archive_list_free(e, p, ps);
    assert(log.dones.back() == rar.size() && log.totals.back() == rar.size());
    for (size_t i = 1; i < log.dones.size(); ++i) assert(log.dones[i] >= log.dones[i - 1]);

    // Cancel on the second poll: RAR_ERR_ABORTED, outputs untouched.
    int polls = 0;
    count = 99;
    e = nullptr;
    p = nullptr;
    ps = 42;
    rc = openrar_archive_list_ex(rar.data(), rar.size(), &count, &e, &p, &ps, nullptr,
                                 ex_cancel_after_two, &polls);
    assert(rc == RAR_ERR_ABORTED);
    assert(count == 0 && e == nullptr && p == nullptr && ps == 0);
    std::cout << "PASS test_list_ex_buffer\n";
}

static void test_list_file_ex_cancel() {
    auto temp_dir = std::filesystem::temp_directory_path() / "openrar_dll_test_ex";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    auto out_rar_path = (temp_dir / "ex_cancel.rar").string();
    std::vector<uint8_t> rar = make_two_entry_rar();
    {
        FILE* f = std::fopen(out_rar_path.c_str(), "wb");
        assert(f);
        std::fwrite(rar.data(), 1, rar.size(), f);
        std::fclose(f);
    }

    // Cancel on the second poll: RAR_ERR_ABORTED (-11), outputs left
    // unallocated/untouched.
    int polls = 0;
    uint32_t count = 99;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 42;
    int rc = openrar_archive_list_file_ex(out_rar_path.c_str(), &count, &e, &p, &ps, nullptr,
                                          ex_cancel_after_two, &polls);
    assert(rc == RAR_ERR_ABORTED);
    assert(polls == 2);
    assert(count == 0 && e == nullptr && p == nullptr && ps == 0);
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_list_file_ex_cancel\n";
}

static void test_list_file_ex_encrypted() {
    const std::filesystem::path fixtures = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";

    // hello5_hp.rar (-hp secret): header-encrypted. The _ex listing returns
    // the dedicated early password signal; outputs stay untouched.
    auto hp = (fixtures / "hello5_hp.rar").string();
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    int rc =
        openrar_archive_list_file_ex(hp.c_str(), &count, &e, &p, &ps, nullptr, nullptr, nullptr);
    assert(rc == RAR_ERR_ENCRYPTED);
    assert(count == 0 && e == nullptr && p == nullptr && ps == 0);
    char msg[256] = {};
    openrar_archive_get_error(msg, sizeof(msg));
    assert(msg[0] != '\0');

    // Parity: the non-_ex export keeps its historical code for the same
    // archive.
    rc = openrar_archive_list_file(hp.c_str(), &count, &e, &p, &ps);
    assert(rc == RAR_ERR_UNSUPPORTED_FEATURE);

    // hello5_p.rar (-p secret): file-data encrypted, headers in clear —
    // rejected by both paths (no semantics relaxation in the _ex export).
    auto pp = (fixtures / "hello5_p.rar").string();
    rc = openrar_archive_list_file_ex(pp.c_str(), &count, &e, &p, &ps, nullptr, nullptr, nullptr);
    assert(rc == RAR_ERR_UNSUPPORTED_FEATURE);
    rc = openrar_archive_list_file(pp.c_str(), &count, &e, &p, &ps);
    assert(rc == RAR_ERR_UNSUPPORTED_FEATURE);
    std::cout << "PASS test_list_file_ex_encrypted\n";
}

static void test_list_file_pw() {
    const std::filesystem::path fixtures = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";
    auto hp = (fixtures / "hello5_hp.rar").string(); // -hp secret
    auto pp = (fixtures / "hello5_p.rar").string();  // -p secret, headers clear

    // Correct password: headers decrypt, entries reported with is_encrypted
    // (the -hp contract), byte progress ends at (file_size, file_size).
    ExProgressLog log;
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    int rc = openrar_archive_list_file_pw(hp.c_str(), "secret", &count, &e, &p, &ps, ex_progress_cb,
                                          nullptr, &log);
    assert(rc == RAR_OK && count >= 1);
    {
        const auto* ents = static_cast<const openrar_archive_entry_t*>(e);
        uint32_t encrypted_files = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (ents[i].is_dir) continue;
            assert(ents[i].is_encrypted == 1);
            ++encrypted_files;
        }
        assert(encrypted_files >= 1);
    }
    openrar_archive_list_free(e, p, ps);
    assert(log.dones.size() >= 2);
    for (size_t i = 1; i < log.dones.size(); ++i) assert(log.dones[i] >= log.dones[i - 1]);
    assert(log.dones.back() == log.totals.back());

    // Wrong password: RAR_ERR_BAD_PASSWORD, outputs untouched.
    count = 99;
    e = nullptr;
    p = nullptr;
    ps = 42;
    rc = openrar_archive_list_file_pw(hp.c_str(), "nope", &count, &e, &p, &ps, nullptr, nullptr,
                                      nullptr);
    assert(rc == RAR_ERR_BAD_PASSWORD);
    assert(count == 0 && e == nullptr && p == nullptr && ps == 0);

    // No password: the early RAR_ERR_ENCRYPTED signal (null and empty alike).
    rc = openrar_archive_list_file_pw(hp.c_str(), nullptr, &count, &e, &p, &ps, nullptr, nullptr,
                                      nullptr);
    assert(rc == RAR_ERR_ENCRYPTED && count == 0 && e == nullptr);
    rc = openrar_archive_list_file_pw(hp.c_str(), "", &count, &e, &p, &ps, nullptr, nullptr,
                                      nullptr);
    assert(rc == RAR_ERR_ENCRYPTED && count == 0 && e == nullptr);

    // -p archive (headers clear, mixed data): lists even without a password,
    // flagging the encrypted entries; the frozen list_file_ex still rejects
    // it for parity.
    rc = openrar_archive_list_file_pw(pp.c_str(), nullptr, &count, &e, &p, &ps, nullptr, nullptr,
                                      nullptr);
    assert(rc == RAR_OK && count >= 2);
    {
        const auto* ents = static_cast<const openrar_archive_entry_t*>(e);
        uint32_t encrypted = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (ents[i].is_dir) continue;
            encrypted += ents[i].is_encrypted ? 1u : 0u;
        }
        assert(encrypted >= 1);
    }
    openrar_archive_list_free(e, p, ps);
    rc = openrar_archive_list_file_ex(pp.c_str(), &count, &e, &p, &ps, nullptr, nullptr, nullptr);
    assert(rc == RAR_ERR_UNSUPPORTED_FEATURE);
    std::cout << "PASS test_list_file_pw\n";
}

static void test_open_ex() {
    std::vector<uint8_t> rar = make_two_entry_rar();

    // Progress over the open-time scan; the handle then lists normally.
    ExProgressLog log;
    uint32_t h = openrar_archive_open_ex(rar.data(), rar.size(), ex_progress_cb, nullptr, &log);
    assert(h != 0);
    assert(log.dones.size() >= 2);
    assert(log.dones.back() == log.totals.back());
    for (size_t i = 1; i < log.dones.size(); ++i) assert(log.dones[i] >= log.dones[i - 1]);
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    int rc = openrar_archive_handle_list(h, &count, &e, &p, &ps);
    assert(rc == 0 && count == 2);
    openrar_archive_list_free(e, p, ps);
    openrar_archive_close(h);

    // Null callbacks: equivalent to openrar_archive_open.
    uint32_t h0 = openrar_archive_open(rar.data(), rar.size());
    uint32_t h1 = openrar_archive_open_ex(rar.data(), rar.size(), nullptr, nullptr, nullptr);
    assert(h0 != 0 && h1 != 0);
    openrar_archive_close(h0);
    openrar_archive_close(h1);

    // Cancel on the second poll: handle is 0, error string explains.
    int polls = 0;
    h = openrar_archive_open_ex(rar.data(), rar.size(), nullptr, ex_cancel_after_two, &polls);
    assert(h == 0 && polls == 2);
    char msg[256] = {};
    openrar_archive_get_error(msg, sizeof(msg));
    assert(std::string(msg).find("aborted") != std::string::npos);
    std::cout << "PASS test_open_ex\n";
}

static void test_file_helpers() {
    // Create a temp source file and use create_from_paths
    auto temp_dir = std::filesystem::temp_directory_path() / "openrar_dll_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    auto src_path = (temp_dir / "dll_test_src.txt").string();
    auto out_rar_path = (temp_dir / "dll_test_out.rar").string();
    const char* src = src_path.c_str();
    const char* arc_name = "archived.txt";
    const char* out_rar = out_rar_path.c_str();
    {
        FILE* f = std::fopen(src, "wb");
        assert(f);
        std::fwrite("file helper content", 1, 19, f);
        std::fclose(f);
    }
    const char* srcs[] = {src};
    const char* arcs[] = {arc_name};
    uint8_t* buf = nullptr;
    size_t blen = 0;
    int rc = openrar_archive_create_from_paths(srcs, arcs, 1, 3, 4, &buf, &blen);
    assert(rc == 0 && buf && blen > 0);
    openrar_free(buf);
    rc = openrar_archive_create_to_file(srcs, arcs, 1, 3, 4, out_rar);
    assert(rc == 0);
    uint32_t count = 0;
    void* e = nullptr;
    void* p = nullptr;
    size_t ps = 0;
    rc = openrar_archive_list_file(out_rar, &count, &e, &p, &ps);
    assert(rc == 0 && count == 1);
    openrar_archive_list_free(e, p, ps);
    uint8_t* out = nullptr;
    size_t olen = 0;
    rc = openrar_archive_extract_file(out_rar, 0, &out, &olen);
    assert(rc == 0 && olen == 19 && std::memcmp(out, "file helper content", 19) == 0);
    openrar_free(out);
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_file_helpers\n";
}

static void test_b4_empty_archive_extract_all() {
    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    int rc = openrar_archive_create(nullptr, nullptr, nullptr, 0, 3, 4, &rar, &rar_len);
    assert(rc == 0);
    assert(rar && rar_len > 0);

    uint8_t* buf = reinterpret_cast<uint8_t*>(0x1234);
    size_t buf_sz = 1234;
    uint64_t* offs = reinterpret_cast<uint64_t*>(0x5678);
    uint32_t off_cnt = 5678;
    rc = openrar_archive_extract_all(rar, rar_len, &buf, &buf_sz, &offs, &off_cnt);
    assert(rc == 0);
    assert(buf == nullptr);
    assert(buf_sz == 0);
    assert(offs == nullptr);
    assert(off_cnt == 0);

    openrar_free(rar);
    std::cout << "PASS test_b4_empty_archive_extract_all\n";
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned test_get_pid() {
    return static_cast<unsigned>(GetCurrentProcessId());
}
#else
#include <unistd.h>
static unsigned test_get_pid() {
    return static_cast<unsigned>(getpid());
}
#endif

static void test_b1_durable_write_collision() {
    std::error_code ec;
    std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "openrar_test_b1_durable";
    std::filesystem::create_directories(temp_dir, ec);
    std::filesystem::path src = temp_dir / "input.txt";
    std::filesystem::path out_rar = temp_dir / "out.rar";
    std::string src_str = src.string();
    std::string out_rar_str = out_rar.string();

    {
        FILE* f = std::fopen(src_str.c_str(), "wb");
        assert(f);
        std::fwrite("durable test", 1, 12, f);
        std::fclose(f);
    }

    // Pre-create collision files for seq 0, 1, 2
    unsigned pid = test_get_pid();
    std::string suffix = ".openrar-tmp." + std::to_string(pid) + ".";
    for (int seq = 0; seq < 3; ++seq) {
        std::filesystem::path coll = out_rar_str + suffix + std::to_string(seq);
        FILE* f = std::fopen(coll.string().c_str(), "wb");
        assert(f);
        std::fwrite("collision", 1, 9, f);
        std::fclose(f);
    }

    const char* srcs[] = {src_str.c_str()};
    const char* arcs[] = {"input.txt"};
    int rc = openrar_archive_create_to_file(srcs, arcs, 1, 3, 4, out_rar_str.c_str());
    assert(rc == 0);
    assert(std::filesystem::exists(out_rar, ec));

    // Verify seq 0, 1, 2 are still there (untouched by durable_write_to)
    for (int seq = 0; seq < 3; ++seq) {
        std::filesystem::path coll = out_rar_str + suffix + std::to_string(seq);
        assert(std::filesystem::exists(coll, ec));
    }

    // Fail-fast test on non-collision error (target parent is an existing file, not directory)
    std::filesystem::path block_file = temp_dir / "block.txt";
    std::string block_file_str = block_file.string();
    {
        FILE* bf = std::fopen(block_file_str.c_str(), "wb");
        assert(bf);
        std::fwrite("block", 1, 5, bf);
        std::fclose(bf);
    }
    std::filesystem::path invalid_rar = block_file / "out.rar";
    std::string invalid_rar_str = invalid_rar.string();
    rc = openrar_archive_create_to_file(srcs, arcs, 1, 3, 4, invalid_rar_str.c_str());
    assert(rc != 0);

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASS test_b1_durable_write_collision\n";
}

static void test_archive_handle_set_limits() {
    // Invalid handle returns RAR_ERR_INVALID_ARG (-9)
    assert(openrar_archive_handle_set_limits(0, 10, 100, 10, 1000) == RAR_ERR_INVALID_ARG);
    assert(openrar_archive_handle_set_limits(999999, 10, 100, 10, 1000) == RAR_ERR_INVALID_ARG);

    const char* paths[] = {"small.txt", "large.txt"};
    const uint8_t data1[] = "12345"; // 5 bytes
    const uint8_t data2[] = "12345678901234567890"; // 20 bytes
    const uint8_t* datas[] = {data1, data2};
    size_t sizes[] = {sizeof(data1) - 1, sizeof(data2) - 1};
    const uint8_t* path_ptrs[] = {reinterpret_cast<const uint8_t*>(paths[0]),
                                  reinterpret_cast<const uint8_t*>(paths[1])};

    uint8_t* rar = nullptr;
    size_t rar_len = 0;
    int rc = openrar_archive_create(path_ptrs, datas, sizes, 2, 0, 4, &rar, &rar_len);
    assert(rc == 0 && rar && rar_len > 0);

    uint32_t h = openrar_archive_open(rar, rar_len);
    assert(h != 0);

    // Set max member bytes to 10. Member 0 (5 bytes) succeeds, member 1 (20 bytes) fails with RAR_ERR_LIMIT_EXCEEDED (-15).
    rc = openrar_archive_handle_set_limits(h, 10, 100, 10, 1000);
    assert(rc == 0);

    uint8_t* out0 = nullptr;
    size_t len0 = 0;
    rc = openrar_archive_handle_extract(h, 0, &out0, &len0);
    assert(rc == 0 && len0 == 5);
    openrar_free(out0);

    uint8_t* out1 = nullptr;
    size_t len1 = 0;
    rc = openrar_archive_handle_extract(h, 1, &out1, &len1);
    assert(rc == RAR_ERR_LIMIT_EXCEEDED);
    assert(out1 == nullptr);

    // Now raise limit to 50 bytes and retry member 1
    rc = openrar_archive_handle_set_limits(h, 50, 100, 10, 1000);
    assert(rc == 0);

    rc = openrar_archive_handle_extract(h, 1, &out1, &len1);
    assert(rc == 0 && len1 == 20);
    openrar_free(out1);

    openrar_archive_close(h);
    openrar_free(rar);
    std::cout << "PASS test_archive_handle_set_limits\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_version();
    test_block_codec_roundtrip();
    test_alloc_free_alias();
    test_buffer_archive_create_list_extract();
    test_error_path();
    test_stream_encoder();
    test_stream_cancel();
    test_stream_cancel_reentrant();
    test_stream_progress();
    test_abi_features();
    test_list_file_ex_progress();
    test_list_ex_buffer();
    test_list_file_ex_cancel();
    test_list_file_ex_encrypted();
    test_list_file_pw();
    test_open_ex();
    test_file_helpers();
    test_b4_empty_archive_extract_all();
    test_b1_durable_write_collision();
    test_archive_handle_set_limits();
    test_archive_repair();
    std::cout << "ALL DLL TESTS PASSED\n";
    return 0;
}
