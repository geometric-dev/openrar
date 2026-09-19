// Native tests for the incremental streaming encoder (Path A).
// Gates (docs/streaming-considerations.md §2, §7):
//   1. Byte-identical: StreamEncoder output (any chunking) ==
//      Compressor50::compress_buffer output of the same concatenated input.
//   2. The historical iteration-3 crash: 256 KiB fed in 4 x 64 KiB chunks.
//   3. Round-trip through Decompressor50 (decoder needs no changes).
//   4. Cancel -> feed() returns false; progress is monotonic.
//   5. Flush sink delivers exactly the same bytes as accumulation.

#include "../../src/compress/stream_encoder.hpp"
#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/decompressor50.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
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
// Unconditional failure report; CHECK(false && ...) would trip C4127.
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg);                          \
        ++fails;                                                                                   \
    } while (0)

namespace {

std::vector<uint8_t> pattern_payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        // Mix compressible runs with pseudo-random bytes.
        x = x * 1664525u + 1013904223u;
        v[i] = (i % 97 < 64) ? static_cast<uint8_t>((x >> 16) & 0x3)
                             : static_cast<uint8_t>((x >> 11) & 0xFF);
    }
    return v;
}

std::vector<uint8_t> one_shot(const std::vector<uint8_t>& src, int method, size_t win) {
    std::vector<openrar::core::byte> out;
    if (!openrar::compress::Compressor50::compress_buffer(src.data(), src.size(), out, method,
                                                          win)) {
        return {};
    }
    return std::vector<uint8_t>(out.begin(), out.end());
}

bool round_trip(const std::vector<uint8_t>& compressed, const std::vector<uint8_t>& original) {
    std::vector<openrar::core::byte> restored;
    openrar::compress::Decompressor50 dec(4 * 1024 * 1024);
    if (!dec.decompress_to_vector(compressed.data(), compressed.size(), restored)) return false;
    if (restored.size() != original.size()) return false;
    return std::memcmp(restored.data(), original.data(), original.size()) == 0;
}

void test_byte_identical_chunking(int method, size_t win, const std::vector<size_t>& chunk_sizes) {
    const std::vector<uint8_t> src =
        pattern_payload(300 * 1024, 1234u + static_cast<uint32_t>(method));
    const std::vector<uint8_t> expected = one_shot(src, method, win);
    CHECK(!expected.empty());

    for (const size_t chunk : chunk_sizes) {
        openrar::compress::StreamEncoder enc(method, win);
        std::vector<uint8_t> got;
        for (size_t off = 0; off < src.size(); off += chunk) {
            const size_t n = std::min(chunk, src.size() - off);
            if (!enc.feed(src.data() + off, n)) {
                FAIL("feed failed");
                return;
            }
        }
        if (!enc.finish(got)) {
            FAIL("finish failed");
            return;
        }
        CHECK(got.size() == expected.size());
        if (got != expected) {
            std::fprintf(stderr, "  byte-identical mismatch: method=%d win=%zu chunk=%zu\n", method,
                         win, chunk);
            return;
        }
        CHECK(round_trip(got, src));
    }
}

void test_iteration3_regression() {
    // docs/streaming-considerations.md §3: the reverted experiment crashed on
    // iteration 3 of 256 KiB fed in 4 x 64 KiB chunks (external state
    // patching). Path A owns the state internally; this must round-trip.
    std::vector<uint8_t> src(256 * 1024);
    for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);

    openrar::compress::StreamEncoder enc(3, 1 * 1024 * 1024);
    std::vector<uint8_t> got;
    for (int i = 0; i < 4; i++) {
        CHECK(enc.feed(src.data() + i * 64 * 1024, 64 * 1024));
    }
    CHECK(enc.finish(got));
    CHECK(!got.empty());
    CHECK(round_trip(got, src));
}

void test_empty_stream() {
    openrar::compress::StreamEncoder enc(3, 1024 * 1024);
    std::vector<uint8_t> got;
    CHECK(enc.finish(got));
    CHECK(got.empty());
}

void test_cancel_and_progress() {
    openrar::compress::StreamEncoder enc(3, 1024 * 1024);
    int cancel_polls = 0;
    enc.set_cancel(
        [](void* ud) {
            return ++*static_cast<int*>(ud) > 2 ? 1 : 0; // cancel on the 3rd poll
        },
        &cancel_polls);

    const std::vector<uint8_t> chunk(64 * 1024, 0xAB);
    CHECK(enc.feed(chunk.data(), chunk.size()));  // poll 1: ok
    CHECK(enc.feed(chunk.data(), chunk.size()));  // poll 2: ok
    CHECK(!enc.feed(chunk.data(), chunk.size())); // poll 3: cancel fires

    // Progress monotonicity.
    openrar::compress::StreamEncoder enc2(3, 1024 * 1024);
    struct Hook {
        uint64_t last{0};
        int calls{0};
        static void trampoline(void* ud, uint64_t done, uint64_t /*total*/) {
            auto* h = static_cast<Hook*>(ud);
            CHECK(done >= h->last); // monotonic
            h->last = done;
            h->calls++;
        }
    } hook;
    enc2.set_progress(&Hook::trampoline, &hook);
    const std::vector<uint8_t> src = pattern_payload(200 * 1024, 99);
    for (size_t off = 0; off < src.size(); off += 32 * 1024) {
        const size_t n = std::min<size_t>(32 * 1024, src.size() - off);
        CHECK(enc2.feed(src.data() + off, n));
    }
    std::vector<uint8_t> out;
    CHECK(enc2.finish(out));
    CHECK(hook.calls > 0);
    CHECK(hook.last == src.size()); // final emit lands exactly on total_in
    CHECK(enc2.total_input() == src.size());
}

void test_flush_sink() {
    const std::vector<uint8_t> src = pattern_payload(2 * 1024 * 1024, 555); // spans several blocks
    std::vector<uint8_t> sink_bytes;
    openrar::compress::StreamEncoder enc(3, 1024 * 1024);
    enc.set_flush(
        [](void* ud, const uint8_t* data, size_t n) -> int {
            auto* v = static_cast<std::vector<uint8_t>*>(ud);
            v->insert(v->end(), data, data + n);
            return 0;
        },
        &sink_bytes);

    for (size_t off = 0; off < src.size(); off += 100 * 1024) {
        const size_t n = std::min<size_t>(100 * 1024, src.size() - off);
        CHECK(enc.feed(src.data() + off, n));
    }
    std::vector<uint8_t> out;
    CHECK(enc.finish(out));
    // All bytes went through the sink; finish() output stays empty.
    CHECK(out.empty());
    CHECK(sink_bytes == one_shot(src, 3, 1024 * 1024));
    CHECK(round_trip(sink_bytes, src));
}

void test_store_passthrough() {
    const std::vector<uint8_t> src = pattern_payload(50 * 1024, 42);
    openrar::compress::StreamEncoder enc(0, 1024 * 1024);
    std::vector<uint8_t> got;
    for (size_t off = 0; off < src.size(); off += 7 * 1024) {
        const size_t n = std::min<size_t>(7 * 1024, src.size() - off);
        CHECK(enc.feed(src.data() + off, n));
    }
    CHECK(enc.finish(got));
    CHECK(got == src); // STORE: output == input
}

void test_store_flush_sink() {
    const std::vector<uint8_t> src = pattern_payload(200 * 1024, 101);
    openrar::compress::StreamEncoder enc(0, 1024 * 1024);
    std::vector<uint8_t> flushed;
    size_t calls = 0;
    enc.set_flush(
        [](void* u, const openrar::core::byte* data, size_t size) -> int {
            auto* p = static_cast<std::pair<std::vector<uint8_t>*, size_t*>*>(u);
            p->first->insert(p->first->end(), data, data + size);
            (*p->second)++;
            return 0;
        },
        new std::pair<std::vector<uint8_t>*, size_t*>(&flushed, &calls));

    const size_t chunk_size = 16 * 1024;
    for (size_t off = 0; off < src.size(); off += chunk_size) {
        const size_t n = std::min<size_t>(chunk_size, src.size() - off);
        CHECK(enc.feed(src.data() + off, n));
    }
    std::vector<uint8_t> out;
    CHECK(enc.finish(out));
    // Zero-buffer invariant: all bytes routed incrementally to flush_cb_, out is empty!
    CHECK(out.empty());
    CHECK(flushed == src);
    CHECK(calls == (src.size() + chunk_size - 1) / chunk_size);

    // Test sink abort on store mode
    openrar::compress::StreamEncoder enc_abort(0, 1024 * 1024);
    enc_abort.set_flush(
        [](void*, const openrar::core::byte*, size_t) -> int {
            return -1; // Abort sink
        },
        nullptr);
    CHECK(!enc_abort.feed(src.data(), 1024)); // feed returns false
    CHECK(enc_abort.is_finished());
    std::vector<uint8_t> out_abort;
    CHECK(!enc_abort.finish(out_abort)); // finish returns false
}

void test_stream_encoder_incremental_pull() {
    const std::vector<uint8_t> src = pattern_payload(300 * 1024, 555);
    const size_t win = 1024 * 1024;
    const std::vector<uint8_t> expected = one_shot(src, 3, win);

    openrar::compress::StreamEncoder enc(3, win);
    std::vector<uint8_t> pulled;
    for (size_t off = 0; off < src.size(); off += 32 * 1024) {
        const size_t n = std::min<size_t>(32 * 1024, src.size() - off);
        CHECK(enc.feed(src.data() + off, n));
        std::vector<openrar::core::byte> chunk;
        CHECK(enc.take_output(chunk));
        pulled.insert(pulled.end(), chunk.begin(), chunk.end());
    }
    std::vector<uint8_t> final_chunk;
    CHECK(enc.finish(final_chunk));
    pulled.insert(pulled.end(), final_chunk.begin(), final_chunk.end());
    CHECK(pulled == expected);
    CHECK(round_trip(pulled, src));

    // Also test take_output on method 0 (Store)
    openrar::compress::StreamEncoder enc_store(0, win);
    std::vector<uint8_t> pulled_store;
    for (size_t off = 0; off < src.size(); off += 20 * 1024) {
        const size_t n = std::min<size_t>(20 * 1024, src.size() - off);
        CHECK(enc_store.feed(src.data() + off, n));
        std::vector<openrar::core::byte> chunk;
        CHECK(enc_store.take_output(chunk));
        pulled_store.insert(pulled_store.end(), chunk.begin(), chunk.end());
    }
    std::vector<uint8_t> final_store;
    CHECK(enc_store.finish(final_store));
    pulled_store.insert(pulled_store.end(), final_store.begin(), final_store.end());
    CHECK(pulled_store == src);
}

void test_stream_decoder_roundtrip() {
    const std::vector<uint8_t> src = pattern_payload(120 * 1024, 777);
    for (int method : {1, 3, 5}) {
        const size_t win = 1024 * 1024;
        std::vector<uint8_t> comp = one_shot(src, method, win);
        CHECK(!comp.empty());

        for (size_t chunk : {1, 17, 512, 4096, 65536}) {
            openrar::compress::StreamDecoder dec(win, method);
            for (size_t off = 0; off < comp.size(); off += chunk) {
                size_t n = std::min(chunk, comp.size() - off);
                if (!dec.feed(comp.data() + off, n)) {
                    FAIL("decoder feed failed");
                    return;
                }
            }
            std::vector<openrar::core::byte> restored;
            if (!dec.finish(restored)) {
                FAIL("decoder finish failed");
                return;
            }
            CHECK(restored.size() == src.size());
            CHECK(std::memcmp(restored.data(), src.data(), src.size()) == 0);
        }
    }
}

void test_stream_decoder_incremental_pull() {
    const std::vector<uint8_t> src = pattern_payload(250 * 1024, 888);
    const size_t win = 1024 * 1024;
    std::vector<uint8_t> comp = one_shot(src, 3, win);
    CHECK(!comp.empty());

    openrar::compress::StreamDecoder dec(win, 3);
    std::vector<openrar::core::byte> accumulated;
    for (size_t off = 0; off < comp.size(); off += 2048) {
        size_t n = std::min<size_t>(2048, comp.size() - off);
        CHECK(dec.feed(comp.data() + off, n));
        std::vector<openrar::core::byte> chunk;
        CHECK(dec.take_output(chunk));
        accumulated.insert(accumulated.end(), chunk.begin(), chunk.end());
    }
    std::vector<openrar::core::byte> rest;
    CHECK(dec.finish(rest));
    accumulated.insert(accumulated.end(), rest.begin(), rest.end());
    CHECK(accumulated.size() == src.size());
    CHECK(std::memcmp(accumulated.data(), src.data(), src.size()) == 0);
}

void test_stream_decoder_store() {
    const std::vector<uint8_t> src = pattern_payload(40 * 1024, 333);
    openrar::compress::StreamDecoder dec(1024 * 1024, /*method=*/0);
    for (size_t off = 0; off < src.size(); off += 3000) {
        size_t n = std::min<size_t>(3000, src.size() - off);
        CHECK(dec.feed(src.data() + off, n));
    }
    std::vector<openrar::core::byte> out;
    CHECK(dec.finish(out));
    CHECK(out.size() == src.size());
    CHECK(std::memcmp(out.data(), src.data(), src.size()) == 0);
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    const std::vector<size_t> odd_chunks = {1, 7, 13, 64 * 1024, 1 << 20};
    for (const int method : {1, 3, 5}) {
        for (const size_t win : {128 * 1024, 1024 * 1024}) {
            test_byte_identical_chunking(method, win, odd_chunks);
        }
    }
    test_iteration3_regression();
    test_empty_stream();
    test_cancel_and_progress();
    test_flush_sink();
    test_store_passthrough();
    test_store_flush_sink();
    test_stream_encoder_incremental_pull();
    test_stream_decoder_roundtrip();
    test_stream_decoder_incremental_pull();
    test_stream_decoder_store();

    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::fprintf(stderr, "stream_encoder_tests OK\n");
    return 0;
}
