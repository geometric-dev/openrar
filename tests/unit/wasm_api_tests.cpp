// Native-side test for the WASM C ABI surface (src/wasm/wasm_api.cpp).
// Verifies the exported functions behave identically with a regular C++17
// compiler — proves the bindings are portable and round-trip data losslessly.
//
// This target runs as `wasm_api_tests` under ctest on native platforms so
// that the WASM ABI is regression-tested on every CI matrix even when an
// emscripten toolchain is not available.

#include "../../src/wasm/wasm_api.hpp"
#include "../../src/compress/compressor50.hpp"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

extern "C" {
int openrar_version();
void* openrar_alloc(size_t);
void openrar_free(void*);
int openrar_compress2(const uint8_t*, size_t, uint8_t**, size_t*, int, size_t);
int openrar_decompress2(const uint8_t*, size_t, uint8_t**, size_t*, size_t);
}

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

static std::vector<uint8_t> make_payload(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 31u + 7u);
    return v;
}

static void test_version() {
    CHECK(openrar_version() == 2);
}

static void test_alloc_free() {
    void* p = openrar_alloc(128);
    CHECK(p != nullptr);
    std::memset(p, 0xAA, 128);
    openrar_free(p);
}

static void test_roundtrip() {
    auto src = make_payload(8192);
    uint8_t* compressed = nullptr;
    size_t compressed_len = 0;
    int rc = openrar_compress2(src.data(), src.size(), &compressed, &compressed_len, 3, 0x200000);
    CHECK(rc == 1);
    CHECK(compressed != nullptr);
    CHECK(compressed_len > 0);
    CHECK(compressed_len < src.size()); // expect compression

    uint8_t* restored = nullptr;
    size_t restored_len = 0;
    rc = openrar_decompress2(compressed, compressed_len, &restored, &restored_len, 0x100000);
    CHECK(rc == 1);
    CHECK(restored_len == src.size());
    CHECK(std::memcmp(restored, src.data(), src.size()) == 0);

    openrar_free(compressed);
    openrar_free(restored);
}

static void test_empty() {
    std::vector<uint8_t> empty;
    uint8_t* compressed = nullptr;
    size_t compressed_len = 99;
    int rc = openrar_compress2(empty.data(), 0, &compressed, &compressed_len, 3, 0x200000);
    CHECK(rc == 1);
    CHECK(compressed == nullptr);
    CHECK(compressed_len == 0);

    rc = openrar_decompress2(compressed, 0, &compressed, &compressed_len, 0x100000);
    CHECK(rc == 1);
}

static void test_high_compress() {
    // Highly compressible payload: should shrink dramatically.
    std::vector<uint8_t> src(16384, 0xCC);
    uint8_t* compressed = nullptr;
    size_t compressed_len = 0;
    int rc = openrar_compress2(src.data(), src.size(), &compressed, &compressed_len, 5, 0x200000);
    CHECK(rc == 1);
    CHECK(compressed_len < src.size() / 4);

    uint8_t* restored = nullptr;
    size_t restored_len = 0;
    rc = openrar_decompress2(compressed, compressed_len, &restored, &restored_len, 0x100000);
    CHECK(rc == 1);
    CHECK(restored_len == src.size());
    CHECK(std::memcmp(restored, src.data(), src.size()) == 0);

    openrar_free(compressed);
    openrar_free(restored);
}

static void test_compress_buffer_raw_direct() {
    // Exercise the C++ helper behind the C ABI (driven from JS via ccall).
    auto src = make_payload(4096);
    std::vector<openrar::core::byte> out;
    CHECK(openrar::wasm::compress_buffer_raw(src.data(), src.size(), out, 3, 0x200000));
    CHECK(!out.empty());

    std::vector<openrar::core::byte> dec;
    CHECK(openrar::wasm::decompress_buffer_raw(out.data(), out.size(), dec, 0x100000));
    CHECK(dec.size() == src.size());
    CHECK(std::memcmp(dec.data(), src.data(), src.size()) == 0);
}

static void test_oversize_win_size_rejected() {
    // §3.2: refuse dictionaries larger than MAX_WIN_SIZE. The >MAX boundary
    // probe is 64-bit-only: the ABI takes size_t, and on ILP32 MAX_WIN_SIZE
    // (4 GiB) plus one is not representable — the boundary is unreachable
    // by construction there.
    auto src = make_payload(64);
    uint8_t* out = nullptr;
    size_t out_len = 0;
#if SIZE_MAX >= UINT64_MAX
    int rc = openrar_compress2(src.data(), src.size(), &out, &out_len, 3,
                               openrar::wasm::MAX_WIN_SIZE + 1);
    CHECK(rc == 0);
    CHECK(out == nullptr);
    CHECK(out_len == 0);

    rc = openrar_decompress2(src.data(), src.size(), &out, &out_len,
                             openrar::wasm::MAX_WIN_SIZE + 1);
    CHECK(rc == 0);
    CHECK(out == nullptr);
#else
    (void)src;
    (void)out;
    (void)out_len;
#endif
}

static void test_decompress_corrupt_returns_empty() {
    // §3.3: malformed bit-stream must not return partial output.
    std::vector<uint8_t> corrupt = {0x52, 0x61, 0x72, 0x21, 0x1a, 0xff, 0xff, 0xff};
    uint8_t* out = nullptr;
    size_t out_len = 999;
    int rc = openrar_decompress2(corrupt.data(), corrupt.size(), &out, &out_len, 0x100000);
    CHECK(rc == 0);
    CHECK(out == nullptr);
    CHECK(out_len == 0);
}

static void test_decompress_raw_clears_on_failure() {
    // §3.3: direct API must not leak partial output on bad input.
    std::vector<openrar::core::byte> corrupt = {0x52, 0x61, 0x72, 0x21, 0x1a, 0xff, 0xff, 0xff};
    std::vector<openrar::core::byte> out(64, 0xAB); // pre-fill: failure must
    // leave the vector exactly as it was (no partial output, no growth).
    const size_t prior = out.size();
    bool ok = openrar::wasm::decompress_buffer_raw(corrupt.data(), corrupt.size(), out, 0x100000);
    CHECK(!ok);
    CHECK(out.size() == prior);
}

static void test_stream_compress_roundtrip() {
    auto src = make_payload(64 * 1024);
    uint32_t handle = openrar_stream_create(3, 1024 * 1024);
    CHECK(handle != 0);

    for (size_t off = 0; off < src.size(); off += 8192) {
        size_t n = std::min<size_t>(8192, src.size() - off);
        int rc = openrar_stream_feed(handle, src.data() + off, n);
        CHECK(rc == 0);
    }

    uint8_t* comp_ptr = nullptr;
    size_t comp_len = 0;
    int rc = openrar_stream_finish(handle, &comp_ptr, &comp_len);
    CHECK(rc == 0);
    CHECK(comp_ptr != nullptr);
    CHECK(comp_len > 0);
    openrar_stream_free(handle);

    uint32_t dec_handle = openrar_stream_decompress_create(1024 * 1024);
    CHECK(dec_handle != 0);
    rc = openrar_stream_decompress_feed(dec_handle, comp_ptr, comp_len);
    CHECK(rc == 0);

    uint8_t* out_ptr = nullptr;
    size_t out_len = 0;
    rc = openrar_stream_decompress_finish(dec_handle, &out_ptr, &out_len);
    CHECK(rc == 0);
    CHECK(out_len == src.size());
    CHECK(std::memcmp(out_ptr, src.data(), src.size()) == 0);

    openrar_free(comp_ptr);
    openrar_free(out_ptr);
    openrar_stream_decompress_free(dec_handle);
}

static void test_stream_compress_incremental_pull() {
    auto src = make_payload(128 * 1024);
    uint32_t handle = openrar_stream_compress_new(3, 1024 * 1024);
    CHECK(handle != 0);

    std::vector<uint8_t> all_comp;
    for (size_t off = 0; off < src.size(); off += 16384) {
        size_t n = std::min<size_t>(16384, src.size() - off);
        CHECK(openrar_stream_compress_feed(handle, src.data() + off, n) == 0);
        uint8_t* chunk = nullptr;
        size_t chunk_len = 0;
        CHECK(openrar_stream_compress_pull(handle, &chunk, &chunk_len) == 0);
        if (chunk_len > 0 && chunk) {
            all_comp.insert(all_comp.end(), chunk, chunk + chunk_len);
            openrar_free(chunk);
        }
    }
    uint8_t* final_chunk = nullptr;
    size_t final_len = 0;
    CHECK(openrar_stream_compress_finish(handle, &final_chunk, &final_len) == 0);
    if (final_len > 0 && final_chunk) {
        all_comp.insert(all_comp.end(), final_chunk, final_chunk + final_len);
        openrar_free(final_chunk);
    }
    openrar_stream_compress_free(handle);
    CHECK(!all_comp.empty());

    uint8_t* restored = nullptr;
    size_t restored_len = 0;
    CHECK(openrar_decompress2(all_comp.data(), all_comp.size(), &restored, &restored_len,
                              1024 * 1024) == 1);
    CHECK(restored_len == src.size());
    CHECK(std::memcmp(restored, src.data(), src.size()) == 0);
    openrar_free(restored);
}

static void test_stream_compress_store() {
    auto src = make_payload(32 * 1024);
    uint32_t handle = openrar_stream_create(0, 1024 * 1024);
    CHECK(handle != 0);

    std::vector<uint8_t> all_chunks;
    for (size_t off = 0; off < src.size(); off += 4096) {
        size_t n = std::min<size_t>(4096, src.size() - off);
        CHECK(openrar_stream_feed(handle, src.data() + off, n) == 0);
        uint8_t* chunk = nullptr;
        size_t chunk_len = 0;
        CHECK(openrar_stream_pull(handle, &chunk, &chunk_len) == 0);
        if (chunk_len > 0 && chunk) {
            all_chunks.insert(all_chunks.end(), chunk, chunk + chunk_len);
            openrar_free(chunk);
        }
    }
    uint8_t* final_chunk = nullptr;
    size_t final_len = 0;
    CHECK(openrar_stream_finish(handle, &final_chunk, &final_len) == 0);
    if (final_len > 0 && final_chunk) {
        all_chunks.insert(all_chunks.end(), final_chunk, final_chunk + final_len);
        openrar_free(final_chunk);
    }
    openrar_stream_free(handle);
    CHECK(all_chunks == src);
}

static void test_stream_compress_filter_ex() {
    // Generate x86 CALL instructions to test E8 filter
    std::vector<uint8_t> src(64 * 1024);
    for (size_t i = 0; i + 5 <= src.size(); i += 5) {
        src[i] = 0xE8;
        src[i + 1] = static_cast<uint8_t>(i & 0xFF);
        src[i + 2] = static_cast<uint8_t>((i >> 8) & 0xFF);
        src[i + 3] = 0x00;
        src[i + 4] = 0x00;
    }

    // Test with FORCE_E8 (flags = 2)
    uint32_t handle = openrar_stream_create_ex(3, 1024 * 1024, 2);
    CHECK(handle != 0);

    for (size_t off = 0; off < src.size(); off += 4096) {
        size_t n = std::min<size_t>(4096, src.size() - off);
        int rc = openrar_stream_feed(handle, src.data() + off, n);
        CHECK(rc == 0);
    }

    uint8_t* comp_ptr = nullptr;
    size_t comp_len = 0;
    int rc = openrar_stream_finish(handle, &comp_ptr, &comp_len);
    CHECK(rc == 0);
    CHECK(comp_ptr != nullptr);
    CHECK(comp_len > 0);
    openrar_stream_free(handle);

    uint8_t* restored = nullptr;
    size_t restored_len = 0;
    CHECK(openrar_decompress2(comp_ptr, comp_len, &restored, &restored_len, 1024 * 1024) == 1);
    CHECK(restored_len == src.size());
    CHECK(std::memcmp(restored, src.data(), src.size()) == 0);
    openrar_free(restored);
    openrar_free(comp_ptr);

    // Test with DISABLE_ALL (flags = 1)
    handle = openrar_stream_create_ex(3, 1024 * 1024, 1);
    CHECK(handle != 0);
    CHECK(openrar_stream_feed(handle, src.data(), src.size()) == 0);
    comp_ptr = nullptr;
    comp_len = 0;
    CHECK(openrar_stream_finish(handle, &comp_ptr, &comp_len) == 0);
    CHECK(comp_ptr != nullptr);
    openrar_stream_free(handle);

    restored = nullptr;
    restored_len = 0;
    CHECK(openrar_decompress2(comp_ptr, comp_len, &restored, &restored_len, 1024 * 1024) == 1);
    CHECK(restored_len == src.size());
    CHECK(std::memcmp(restored, src.data(), src.size()) == 0);
    openrar_free(restored);
    openrar_free(comp_ptr);
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    test_version();
    test_alloc_free();
    test_roundtrip();
    test_empty();
    test_high_compress();
    test_compress_buffer_raw_direct();
    test_oversize_win_size_rejected();
    test_decompress_corrupt_returns_empty();
    test_decompress_raw_clears_on_failure();
    test_stream_compress_roundtrip();
    test_stream_compress_incremental_pull();
    test_stream_compress_store();
    test_stream_compress_filter_ex();
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::fprintf(stderr, "wasm_api_tests OK\n");
    return 0;
}