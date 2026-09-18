// Native test for the WASM archive C ABI surface
// (src/wasm/archive_api.cpp). Verifies the exported functions behave
// identically with a regular C++17 compiler — proves the bindings are
// portable and round-trip data losslessly.
//
// This target runs as `archive_api_tests` under ctest on every native CI
// matrix when `-DOPENRAR_WASM_ARCHIVE=ON` is passed at configure time
// (mirrors the WASM build artifact in CI).
//
// Uses malloc/free from the system allocator — these are the same
// functions Emscripten wires through `_openrar_archive_alloc/free`
// on the WASM side, so the round-trip exercises the heap semantics
// the JS wrapper relies on.

#include "../../src/wasm/archive_api.hpp"
#include "../../src/archive/buffer_archive.hpp"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

extern "C" {
int openrar_archive_version();
int openrar_archive_list(const uint8_t*, size_t, uint32_t*, void**, void**, size_t*);
void openrar_archive_list_free(void*, void*, size_t);
int openrar_archive_extract(const uint8_t*, size_t, uint32_t, uint8_t**, size_t*);
int openrar_archive_extract_all(const uint8_t*, size_t, uint8_t**, size_t*, uint64_t**, uint32_t*);
int openrar_archive_create(const uint8_t* const*, const uint8_t* const*, const size_t*, uint32_t,
                           int, uint32_t, uint8_t**, size_t*);
int openrar_archive_get_error(char*, int);
int openrar_archive_last_error_code(void);
void* openrar_archive_alloc(size_t);
void openrar_archive_free(void*);
int openrar_archive_create2(const openrar::wasm::ArchiveInputFile*, uint32_t,
                            const openrar::wasm::ArchiveCreateOpts*,
                            const openrar::wasm::ArchiveHooks*, uint8_t**, size_t*);
int openrar_archive_extract_all2(const uint8_t*, size_t, const openrar::wasm::ArchiveHooks*,
                                 uint8_t**, size_t*, uint64_t**, uint32_t*);
}

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

static std::vector<uint8_t> make_buffer(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Drive BufferArchive::create_archive directly so the test is independent of
// the C ABI we're trying to verify. (Round-trip tests below drive the C ABI.)
static std::vector<uint8_t>
make_archive(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, int method = 3,
             unsigned window_log2 = 4) {
    std::vector<uint8_t> out;
    int rc = openrar::archive::create_archive(files, out, method, window_log2);
    CHECK(rc == openrar::wasm::RAR_OK);
    return out;
}

static std::string utf8(const uint8_t* p, size_t n) {
    return std::string(reinterpret_cast<const char*>(p), n);
}

static void test_version() {
    CHECK(openrar_archive_version() == 2);
}

static void test_alloc_free() {
    void* p = openrar_archive_alloc(128);
    CHECK(p != nullptr);
    std::memset(p, 0xAB, 128);
    openrar_archive_free(p);
}

static void test_validate_path() {
    char err[256];
    // OK paths.
    CHECK(openrar::wasm::validate_archive_path_c("dir/file.txt", 12, 0, err, sizeof(err)) ==
          openrar::wasm::RAR_OK);
    CHECK(openrar::wasm::validate_archive_path_c("dir/subdir/", 11, 1, err, sizeof(err)) ==
          openrar::wasm::RAR_OK);
    // Bad paths.
    CHECK(openrar::wasm::validate_archive_path_c("/abs", 4, 0, err, sizeof(err)) ==
          openrar::wasm::RAR_ERR_INVALID_ARG);
    CHECK(openrar::wasm::validate_archive_path_c("a\\b", 3, 0, err, sizeof(err)) ==
          openrar::wasm::RAR_ERR_INVALID_ARG);
    CHECK(openrar::wasm::validate_archive_path_c("../escape", 9, 0, err, sizeof(err)) ==
          openrar::wasm::RAR_ERR_INVALID_ARG);
    CHECK(openrar::wasm::validate_archive_path_c("", 0, 0, err, sizeof(err)) ==
          openrar::wasm::RAR_ERR_INVALID_ARG);
}

static void test_list_roundtrip() {
    auto payload = make_buffer("Hello, OpenRAR! " + std::string(200, 'x'));
    auto arc = make_archive({
        {"hello.txt", payload},
        {"subdir/file.bin", make_buffer(std::string(1024, 'A'))},
        {"subdir/", {}},
    });
    CHECK(!arc.empty());

    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_size = 0;
    int rc = openrar_archive_list(arc.data(), arc.size(), &count, &entries, &paths, &paths_size);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(count == 3);
    CHECK(entries != nullptr);
    CHECK(paths != nullptr);

    auto* e = static_cast<openrar::wasm::ArchiveEntryOut*>(entries);
    // Check entries in declared order.
    auto path_at = [&](size_t i) -> std::string {
        return utf8(static_cast<uint8_t*>(paths) + e[i].path_offset, e[i].path_len);
    };
    CHECK(path_at(0) == "hello.txt");
    CHECK(e[0].is_dir == 0);
    CHECK(e[0].method == 3);
    CHECK(e[0].size == payload.size());
    CHECK(e[0].packed_size > 0);

    CHECK(path_at(1) == "subdir/file.bin");
    CHECK(e[1].is_dir == 0);
    CHECK(e[1].size == 1024);

    CHECK(path_at(2) == "subdir/");
    CHECK(e[2].is_dir == 1);
    CHECK(e[2].size == 0);
    CHECK(e[2].packed_size == 0);

    openrar_archive_list_free(entries, paths, paths_size);
}

static void test_list_bad_inputs() {
    uint32_t count = 99;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_size = 99;
    int rc = openrar_archive_list(nullptr, 0, &count, &entries, &paths, &paths_size);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);

    // Random bytes are not a RAR archive.
    auto junk = std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    rc = openrar_archive_list(junk.data(), junk.size(), &count, &entries, &paths, &paths_size);
    CHECK(rc == openrar::wasm::RAR_ERR_NOT_RAR);
}

static void test_extract_roundtrip() {
    auto payload = make_buffer("payload-1");
    auto other = make_buffer("payload-2-different-content");
    auto arc = make_archive({
        {"first.txt", payload},
        {"second.txt", other},
    });

    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_extract(arc.data(), arc.size(), 0, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(out != nullptr);
    CHECK(out_len == payload.size());
    CHECK(std::memcmp(out, payload.data(), payload.size()) == 0);
    openrar_archive_free(out);

    rc = openrar_archive_extract(arc.data(), arc.size(), 1, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(out_len == other.size());
    CHECK(std::memcmp(out, other.data(), other.size()) == 0);
    openrar_archive_free(out);
}

static void test_extract_out_of_range() {
    auto arc = make_archive({{"x", make_buffer("x")}});
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_extract(arc.data(), arc.size(), /*index*/ 99, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
}

static void test_extract_all() {
    auto a = make_buffer("alpha-alpha-alpha");
    auto b = make_buffer("beta-beta");
    auto arc = make_archive({{"a.txt", a}, {"b.txt", b}});

    uint8_t* buf = nullptr;
    size_t buf_size = 0;
    uint64_t* offsets = nullptr;
    uint32_t count = 0;
    int rc = openrar_archive_extract_all(arc.data(), arc.size(), &buf, &buf_size, &offsets, &count);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(count == 2);
    CHECK(buf_size == a.size() + b.size());
    CHECK(buf != nullptr);
    CHECK(offsets != nullptr);

    // offsets table is {offset, size} pairs (uint64 each).
    uint64_t off0 = offsets[0], len0 = offsets[1];
    uint64_t off1 = offsets[2], len1 = offsets[3];
    CHECK(off0 == 0);
    CHECK(len0 == a.size());
    CHECK(off1 == a.size());
    CHECK(len1 == b.size());
    CHECK(std::memcmp(buf + off0, a.data(), a.size()) == 0);
    CHECK(std::memcmp(buf + off1, b.data(), b.size()) == 0);

    openrar_archive_free(buf);
    openrar_archive_free(offsets);
}

static void test_create_through_abi() {
    const char* p0 = "files/a.txt";
    const char* p1 = "files/b.bin";
    const char* p2 = "files/sub/";
    const uint8_t* paths[] = {
        reinterpret_cast<const uint8_t*>(p0),
        reinterpret_cast<const uint8_t*>(p1),
        reinterpret_cast<const uint8_t*>(p2),
    };
    auto d0 = make_buffer("first-payload");
    auto d1 = make_buffer("second-payload-bigger-content");
    auto d2 = std::vector<uint8_t>{};
    const uint8_t* data[] = {d0.data(), d1.data(), d2.data()};
    const size_t sizes[] = {d0.size(), d1.size(), 0};

    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc =
        openrar_archive_create(paths, data, sizes, 3, /*method*/ 3, /*win_log2*/ 4, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(out != nullptr);
    CHECK(out_len > 0);

    // Now list + extract what we just created.
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths_buf = nullptr;
    size_t paths_size = 0;
    rc = openrar_archive_list(out, out_len, &count, &entries, &paths_buf, &paths_size);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(count == 3);
    auto* e = static_cast<openrar::wasm::ArchiveEntryOut*>(entries);
    auto path_at = [&](size_t i) -> std::string {
        return utf8(static_cast<uint8_t*>(paths_buf) + e[i].path_offset, e[i].path_len);
    };
    CHECK(path_at(0) == "files/a.txt");
    CHECK(path_at(1) == "files/b.bin");
    CHECK(path_at(2) == "files/sub/");
    CHECK(e[2].is_dir == 1);
    openrar_archive_list_free(entries, paths_buf, paths_size);

    // Extract entry 0 and byte-compare.
    uint8_t* e0 = nullptr;
    size_t e0_len = 0;
    rc = openrar_archive_extract(out, out_len, 0, &e0, &e0_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(e0_len == d0.size());
    CHECK(std::memcmp(e0, d0.data(), d0.size()) == 0);
    openrar_archive_free(e0);

    // Extract entry 1.
    uint8_t* e1 = nullptr;
    size_t e1_len = 0;
    rc = openrar_archive_extract(out, out_len, 1, &e1, &e1_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(e1_len == d1.size());
    CHECK(std::memcmp(e1, d1.data(), d1.size()) == 0);
    openrar_archive_free(e1);

    openrar_archive_free(out);
}

static void test_create_invalid_method() {
    const char* p0 = "x";
    const uint8_t* paths[] = {reinterpret_cast<const uint8_t*>(p0)};
    const uint8_t d0[] = {'x'};
    const uint8_t* data[] = {d0};
    const size_t sizes[] = {1};
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create(paths, data, sizes, 1, /*method*/ 2, 4, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
}

static void test_create_invalid_window_log2() {
    const char* p0 = "x";
    const uint8_t* paths[] = {reinterpret_cast<const uint8_t*>(p0)};
    const uint8_t d0[] = {'x'};
    const uint8_t* data[] = {d0};
    const size_t sizes[] = {1};
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create(paths, data, sizes, 1, 3, /*win_log2*/ 0, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
    rc = openrar_archive_create(paths, data, sizes, 1, 3, /*win_log2*/ 5, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
}

static void test_create_invalid_path() {
    // Path with backslash — should be rejected with INVALID_ARG.
    const char* p0 = "dir\\file.txt";
    const uint8_t* paths[] = {reinterpret_cast<const uint8_t*>(p0)};
    const uint8_t d0[] = {'x'};
    const uint8_t* data[] = {d0};
    const size_t sizes[] = {1};
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create(paths, data, sizes, 1, 3, 4, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);

    // After a failed call, get_error() must surface a message.
    char err[256] = {0};
    int n = openrar_archive_get_error(err, sizeof(err));
    CHECK(n > 0);
    CHECK(std::string(err).find("backslash") != std::string::npos);
}

static void test_get_error_after_success() {
    // After a successful call, g_last_error should still be whatever the
    // last failed call set (we don't clobber on success).
    // Just check the helper exists and doesn't crash.
    char err[256] = {0};
    int n = openrar_archive_get_error(err, sizeof(err));
    CHECK(n >= 0);
}

// ── v2 surface ───────────────────────────────────────────────────────────────

static int g_progress_calls = 0;
static uint64_t g_progress_last_done = 0;
static uint64_t g_progress_last_total = 0;
static void count_progress(uint64_t done, uint64_t total, void* /*user*/) {
    g_progress_calls++;
    g_progress_last_done = done;
    g_progress_last_total = total;
}

static int g_cancel_after = -1; // -1 = never cancel; else cancel on the Nth poll
static int g_cancel_polls = 0;
static int cancel_after_n(void* /*user*/) {
    return ++g_cancel_polls > g_cancel_after ? 1 : 0;
}

static void test_create2_with_mtime_progress() {
    auto payload = make_buffer("create2-payload-" + std::string(300, 'z'));
    std::vector<openrar::wasm::ArchiveInputFile> inputs(2);
    inputs[0].path = "v2/file.txt";
    inputs[0].data = payload.data();
    inputs[0].data_len = payload.size();
    inputs[0].mtime_unix = 1600000000; // even seconds; DOS-round
    inputs[1].path = "v2/sub/";        // dir: data null, len 0
    inputs[1].is_dir = 1;

    openrar::wasm::ArchiveCreateOpts opts{};
    opts.method = 3;
    opts.window_log2 = 4;
    g_progress_calls = 0;
    openrar::wasm::ArchiveHooks hooks{};
    hooks.progress = count_progress;
    hooks.progress_user = nullptr;

    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create2(inputs.data(), 2, &opts, &hooks, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(out != nullptr);
    CHECK(out_len > 0);
    CHECK(g_progress_calls >= 2); // one emit per entry (plus initial)
    CHECK(g_progress_last_total > 0);

    // List: mtime round-trips through DOS conversion (2s granularity).
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_size = 0;
    rc = openrar_archive_list(out, out_len, &count, &entries, &paths, &paths_size);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(count == 2);
    auto* e = static_cast<openrar::wasm::ArchiveEntryOut*>(entries);
    auto path_at = [&](size_t i) -> std::string {
        return utf8(static_cast<uint8_t*>(paths) + e[i].path_offset, e[i].path_len);
    };
    CHECK(path_at(0) == "v2/file.txt");
    const uint64_t m = e[0].mtime;
    CHECK(m + 2 >= 1600000000 && m <= 1600000000 + 2);
    CHECK(path_at(1) == "v2/sub/");
    CHECK(e[1].is_dir == 1);
    openrar_archive_list_free(entries, paths, paths_size);
    openrar_archive_free(out);
}

static void test_extract_all2_cancel() {
    auto arc = make_archive({
        {"c1.txt", make_buffer("cancel-me-1")},
        {"c2.txt", make_buffer("cancel-me-2")},
        {"c3.txt", make_buffer("cancel-me-3")},
    });

    openrar::wasm::ArchiveHooks hooks{};
    hooks.cancel = cancel_after_n;
    hooks.cancel_user = nullptr;

    g_cancel_polls = 0;
    g_cancel_after = 1; // cancel on the first per-entry poll
    uint8_t* buf = nullptr;
    size_t buf_size = 0;
    uint64_t* offsets = nullptr;
    uint32_t count = 0;
    int rc = openrar_archive_extract_all2(arc.data(), arc.size(), &hooks, &buf, &buf_size, &offsets,
                                          &count);
    CHECK(rc == openrar::wasm::RAR_ERR_ABORTED);
    CHECK(buf == nullptr);
    CHECK(offsets == nullptr);
    CHECK(openrar_archive_last_error_code() == openrar::wasm::RAR_ERR_ABORTED);

    // Never-cancel run must succeed and land the error code back at OK.
    // (cancel_after_n returns ++polls > limit, so "never" is a huge limit.)
    g_cancel_polls = 0;
    g_cancel_after = 1 << 30;
    rc = openrar_archive_extract_all2(arc.data(), arc.size(), &hooks, &buf, &buf_size, &offsets,
                                      &count);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(openrar_archive_last_error_code() != openrar::wasm::RAR_ERR_ABORTED);
    CHECK(count == 3);
    openrar_archive_free(buf);
    openrar_archive_free(offsets);
}

static void test_create2_invalid_args() {
    std::vector<openrar::wasm::ArchiveInputFile> inputs(1);
    inputs[0].path = "ok.txt";
    const uint8_t d[] = {'x'};
    inputs[0].data = d;
    inputs[0].data_len = 1;

    openrar::wasm::ArchiveCreateOpts bad_method{};
    bad_method.method = 4;
    bad_method.window_log2 = 4;
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create2(inputs.data(), 1, &bad_method, nullptr, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
    CHECK(openrar_archive_last_error_code() == openrar::wasm::RAR_ERR_INVALID_ARG);

    openrar::wasm::ArchiveCreateOpts bad_log2{};
    bad_log2.method = 3;
    bad_log2.window_log2 = 5;
    rc = openrar_archive_create2(inputs.data(), 1, &bad_log2, nullptr, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);

    // is_dir without trailing slash is rejected.
    inputs[0].is_dir = 1;
    rc = openrar_archive_create2(inputs.data(), 1, nullptr, nullptr, &out, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_INVALID_ARG);
    inputs[0].is_dir = 0;
}

static void test_handle_set_limits() {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
        {"file1.txt", std::vector<uint8_t>(100, 'A')},
        {"file2.txt", std::vector<uint8_t>(200, 'B')},
    };
    std::vector<uint8_t> rar;
    int rc = openrar::archive::create_archive(files, rar, 0); // stored
    CHECK(rc == 0);

    uint32_t handle = openrar_archive_open(rar.data(), rar.size());
    CHECK(handle != 0);

    // 1. Limit member to 50 bytes. File 0 has 100 bytes -> must fail with RAR_ERR_LIMIT_EXCEEDED (-15).
    rc = openrar_archive_handle_set_limits(handle, 50, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    CHECK(rc == openrar::wasm::RAR_OK);

    uint8_t* out_ptr = nullptr;
    size_t out_len = 0;
    rc = openrar_archive_handle_extract(handle, 0, &out_ptr, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_LIMIT_EXCEEDED);
    CHECK(openrar_archive_last_error_code() == openrar::wasm::RAR_ERR_LIMIT_EXCEEDED);
    if (out_ptr) openrar_archive_free(out_ptr);

    // 2. Raise member limit to 150, but total limit to 150.
    rc = openrar_archive_handle_set_limits(handle, 150, 150, UINT64_MAX, UINT64_MAX);
    CHECK(rc == openrar::wasm::RAR_OK);

    // File 0 has 100 bytes -> within 150 member and 150 total. Succeeds.
    out_ptr = nullptr;
    out_len = 0;
    rc = openrar_archive_handle_extract(handle, 0, &out_ptr, &out_len);
    CHECK(rc == openrar::wasm::RAR_OK);
    CHECK(out_len == 100);
    if (out_ptr) openrar_archive_free(out_ptr);

    // File 1 has 200 bytes -> cumulative 300 exceeds total limit of 150.
    out_ptr = nullptr;
    out_len = 0;
    rc = openrar_archive_handle_extract(handle, 1, &out_ptr, &out_len);
    CHECK(rc == openrar::wasm::RAR_ERR_LIMIT_EXCEEDED);
    if (out_ptr) openrar_archive_free(out_ptr);

    openrar_archive_close(handle);

    // 3. One-shot extract_all with total limit exceeded
    uint32_t h2 = openrar_archive_open(rar.data(), rar.size());
    CHECK(h2 != 0);
    rc = openrar_archive_handle_set_limits(h2, UINT64_MAX, 150, UINT64_MAX, UINT64_MAX);
    CHECK(rc == openrar::wasm::RAR_OK);
    uint8_t* buf = nullptr;
    size_t buf_size = 0;
    uint64_t* offsets = nullptr;
    uint32_t offsets_count = 0;
    rc = openrar_archive_handle_extract_all(h2, &buf, &buf_size, &offsets, &offsets_count);
    CHECK(rc == openrar::wasm::RAR_ERR_LIMIT_EXCEEDED);
    if (buf) openrar_archive_free(buf);
    if (offsets) openrar_archive_free(offsets);
    openrar_archive_close(h2);
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
    test_alloc_free();
    test_validate_path();
    test_list_roundtrip();
    test_list_bad_inputs();
    test_extract_roundtrip();
    test_extract_out_of_range();
    test_extract_all();
    test_create_through_abi();
    test_create_invalid_method();
    test_create_invalid_window_log2();
    test_create_invalid_path();
    test_get_error_after_success();
    test_create2_with_mtime_progress();
    test_extract_all2_cancel();
    test_create2_invalid_args();
    test_handle_set_limits();
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::fprintf(stderr, "archive_api_tests OK\n");
    return 0;
}
