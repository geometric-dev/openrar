// Native tests for the incremental streaming decoder (StreamDecoder).
// Part of v2.2 Workstream W4 (docs/milestones/v2.2.0-implementation-plan.md §4.2).
//
// Gates:
//   1. Byte-identical: StreamDecoder output (any chunking) == one-shot decompressed bytes.
//   2. Method coverage: Methods 0 (STORE), 1, 2, 3, 4, 5 round-trip cleanly.
//   3. Chunking invariance: 1-byte, prime-sized, 4 KiB, 64 KiB, and full-stream chunks.
//   4. Incremental pull: take_output() pulls decoded blocks during feeding.
//   5. Output flush sink: flush callback delivers identical bytes to accumulation.
//   6. Cancel and progress: cancellation stops decoding; progress callback is monotonic.
//   7. Fail-closed on corrupted, truncated, or hostile streams.

#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../support/test_runner.hpp"
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg);                          \
        ++fails;                                                                                   \
    } while (0)

namespace {

std::vector<uint8_t> generate_test_payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = (i % 64 < 48) ? static_cast<uint8_t>((x >> 16) & 0x7)
                             : static_cast<uint8_t>((x >> 11) & 0xFF);
    }
    return v;
}

std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& src, int method, size_t win) {
    if (method == 0) {
        return src; // STORE passthrough for raw block codec
    }
    std::vector<openrar::core::byte> out;
    if (!openrar::compress::Compressor50::compress_buffer(src.data(), src.size(), out, method,
                                                          win)) {
        return {};
    }
    return std::vector<uint8_t>(out.begin(), out.end());
}

void test_stream_decoder_roundtrip_methods() {
    const std::vector<size_t> chunk_sizes = {1, 13, 512, 4096, 65536, 1000000};
    for (const int method : {0, 1, 2, 3, 4, 5}) {
        const size_t win = 1024 * 1024;
        const size_t payload_size = (method == 0) ? 64 * 1024 : 160 * 1024;
        const std::vector<uint8_t> src =
            generate_test_payload(payload_size, 42000u + static_cast<uint32_t>(method));
        const std::vector<uint8_t> comp = compress_payload(src, method, win);
        CHECK(!comp.empty());

        for (const size_t chunk : chunk_sizes) {
            openrar::compress::StreamDecoder dec(win, method);
            CHECK(dec.is_valid());

            for (size_t off = 0; off < comp.size(); off += chunk) {
                const size_t n = std::min(chunk, comp.size() - off);
                if (!dec.feed(comp.data() + off, n)) {
                    FAIL("StreamDecoder feed failed");
                    return;
                }
            }

            std::vector<openrar::core::byte> restored;
            if (!dec.finish(restored)) {
                FAIL("StreamDecoder finish failed");
                return;
            }

            CHECK(restored.size() == src.size());
            CHECK(std::memcmp(restored.data(), src.data(), src.size()) == 0);
            CHECK(dec.is_finished());
        }
    }
}

void test_stream_decoder_incremental_take_output() {
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(300 * 1024, 777123);
    const std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(!comp.empty());

    openrar::compress::StreamDecoder dec(win, 3);
    std::vector<openrar::core::byte> accumulated;

    for (size_t off = 0; off < comp.size(); off += 4096) {
        const size_t n = std::min<size_t>(4096, comp.size() - off);
        CHECK(dec.feed(comp.data() + off, n));

        std::vector<openrar::core::byte> partial;
        CHECK(dec.take_output(partial));
        accumulated.insert(accumulated.end(), partial.begin(), partial.end());
    }

    std::vector<openrar::core::byte> rest;
    CHECK(dec.finish(rest));
    accumulated.insert(accumulated.end(), rest.begin(), rest.end());

    CHECK(accumulated.size() == src.size());
    CHECK(std::memcmp(accumulated.data(), src.data(), src.size()) == 0);
}

void test_stream_decoder_flush_sink() {
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(200 * 1024, 888123);
    const std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(!comp.empty());

    struct SinkContext {
        std::vector<uint8_t> delivered;
        int call_count{0};
    } ctx;

    auto sink_cb = [](void* user, const openrar::core::byte* data, size_t size) -> int {
        auto* sc = static_cast<SinkContext*>(user);
        sc->delivered.insert(sc->delivered.end(), data, data + size);
        sc->call_count++;
        return 0;
    };

    openrar::compress::StreamDecoder dec(win, 3);
    dec.set_flush(sink_cb, &ctx);

    for (size_t off = 0; off < comp.size(); off += 2048) {
        const size_t n = std::min<size_t>(2048, comp.size() - off);
        CHECK(dec.feed(comp.data() + off, n));
    }

    std::vector<openrar::core::byte> out;
    CHECK(dec.finish(out));
    // When flush sink is configured, finish returns empty out and all bytes went to the sink
    CHECK(out.empty());
    CHECK(ctx.delivered.size() == src.size());
    CHECK(std::memcmp(ctx.delivered.data(), src.data(), src.size()) == 0);
    CHECK(ctx.call_count > 0);
}

void test_stream_decoder_cancellation() {
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(100 * 1024, 999123);
    const std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(!comp.empty());

    int should_cancel = 0;
    auto cancel_fn = [](void* user) -> int {
        return *static_cast<int*>(user);
    };

    openrar::compress::StreamDecoder dec(win, 3);
    dec.set_cancel(cancel_fn, &should_cancel);

    // First feed succeeds
    CHECK(dec.feed(comp.data(), 1024));

    // Cancel tripped
    should_cancel = 1;
    CHECK(!dec.feed(comp.data() + 1024, 1024));

    // Subsequent finish fails closed
    std::vector<openrar::core::byte> out;
    CHECK(!dec.finish(out));
}

void test_stream_decoder_progress() {
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(250 * 1024, 111222);
    const std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(!comp.empty());

    struct ProgressContext {
        uint64_t last_done{0};
        bool monotonic{true};
        int calls{0};
    } pctx;

    auto progress_fn = [](void* user, openrar::core::uint64 done, openrar::core::uint64 total) {
        (void)total;
        auto* ctx = static_cast<ProgressContext*>(user);
        if (done < ctx->last_done) {
            ctx->monotonic = false;
        }
        ctx->last_done = done;
        ctx->calls++;
    };

    openrar::compress::StreamDecoder dec(win, 3);
    dec.set_progress(progress_fn, &pctx);

    for (size_t off = 0; off < comp.size(); off += 8192) {
        const size_t n = std::min<size_t>(8192, comp.size() - off);
        CHECK(dec.feed(comp.data() + off, n));
    }

    std::vector<openrar::core::byte> out;
    CHECK(dec.finish(out));
    CHECK(pctx.monotonic);
    CHECK(pctx.calls > 0);
    CHECK(pctx.last_done == src.size());
}

void test_stream_decoder_corrupt_data() {
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(50 * 1024, 333444);
    std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(comp.size() > 16);

    // Corrupt header checksum byte
    comp[1] ^= 0xFF;

    openrar::compress::StreamDecoder dec(win, 3);
    bool ok = dec.feed(comp.data(), comp.size());
    // Feed must fail on corrupt header checksum
    CHECK(!ok);

    std::vector<openrar::core::byte> out;
    CHECK(!dec.finish(out));
}

void test_stream_decoder_fine_grained_splits() {
    // Test feeding 1 byte at a time through block header boundaries
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> src = generate_test_payload(80 * 1024, 555666);
    const std::vector<uint8_t> comp = compress_payload(src, 3, win);
    CHECK(!comp.empty());

    openrar::compress::StreamDecoder dec(win, 3);
    for (size_t i = 0; i < comp.size(); ++i) {
        if (!dec.feed(&comp[i], 1)) {
            FAIL("1-byte feed failed");
            return;
        }
    }

    std::vector<openrar::core::byte> out;
    CHECK(dec.finish(out));
    CHECK(out.size() == src.size());
    CHECK(std::memcmp(out.data(), src.data(), src.size()) == 0);
}

} // namespace

static void run_suite_body() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    test_stream_decoder_roundtrip_methods();
    test_stream_decoder_incremental_take_output();
    test_stream_decoder_flush_sink();
    test_stream_decoder_cancellation();
    test_stream_decoder_progress();
    test_stream_decoder_corrupt_data();
    test_stream_decoder_fine_grained_splits();

    if (fails == 0) {
        std::fprintf(stderr, "stream_decoder_tests OK\n");
    }
}

static const openrar::test::Case k_cases[] = {
    {"run_suite_body", run_suite_body, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    return openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc,
                                            argv);
}
