// tests/unit/allocation_tests.cpp
// Native unit tests for zero-allocation hot path and Invariants I-15 / I-16.
// Part of v2.3 Chunk 1 (§2.3, §5.4).

#include "tracking_allocator.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/filters50.hpp"
#include "../support/test_runner.hpp"
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

// ============================================================================
// Global replacement operator new / operator delete hooks
// ONLY linked into this test binary to monitor allocations (Invariant I-16, §2.3)
// ============================================================================

void* operator new(std::size_t n) {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
    return std::malloc(n);
}

void operator delete(void* p) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

void operator delete[](void* p) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
    std::free(p);
}

#if defined(__cpp_aligned_new)
void* operator new(std::size_t n, std::align_val_t al) {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
#if defined(_WIN32)
    void* p = _aligned_malloc(n, static_cast<size_t>(al));
#else
    void* p = std::aligned_alloc(static_cast<size_t>(al), n);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t al) {
    openrar::test::ScopedAllocTracker::record_alloc(n);
    if (n == 0) n = 1;
#if defined(_WIN32)
    void* p = _aligned_malloc(n, static_cast<size_t>(al));
#else
    void* p = std::aligned_alloc(static_cast<size_t>(al), n);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p, std::align_val_t) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t) noexcept {
    openrar::test::ScopedAllocTracker::record_free(0);
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete(void* p, std::size_t, std::align_val_t al) noexcept {
    operator delete(p, al);
}

void operator delete[](void* p, std::size_t, std::align_val_t al) noexcept {
    operator delete[](p, al);
}
#endif

// ============================================================================
// Test helpers & fixtures
// ============================================================================

static std::vector<uint8_t> generate_test_payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = (i % 64 < 48) ? static_cast<uint8_t>((x >> 16) & 0x7)
                             : static_cast<uint8_t>((x >> 11) & 0xFF);
    }
    return v;
}

static std::vector<openrar::core::byte> make_x86_binary(size_t n_calls) {
    std::vector<openrar::core::byte> data(n_calls * 5 + 256, 0x90);
    data[0] = 'M';
    data[1] = 'Z';
    openrar::core::write_le32(data.data() + 0x3C, 64);
    data[64] = 'P';
    data[65] = 'E';
    data[66] = 0;
    data[67] = 0;
    openrar::core::write_le16(data.data() + 68, 0x8664);
    for (size_t i = 0; i < n_calls; ++i) {
        size_t off = 128 + i * 5;
        if (off + 5 > data.size()) break;
        data[off] = 0xE8;
        openrar::core::int32 rel = static_cast<openrar::core::int32>((i * 1234567) % 0x01000000u);
        openrar::core::write_le32(data.data() + off + 1, static_cast<openrar::core::uint32>(rel));
    }
    return data;
}

// ============================================================================
// Named Tests (§5.4)
// ============================================================================

void allocation_tracker_nesting() {
    using namespace openrar::test;
    // Outside tracker, allocations do not increment any tracker
    void* p0 = ::operator new(16);
    ::operator delete(p0);

    {
        ScopedAllocTracker outer;
        void* p1 = ::operator new(64);
        CHECK_EQ(outer.alloc_count(), 1);
        CHECK(outer.bytes_allocated() >= 64);

        {
            ScopedAllocTracker inner;
            void* p2 = ::operator new(128);
            CHECK_EQ(inner.alloc_count(), 1);
            CHECK(inner.bytes_allocated() >= 128);
            CHECK_EQ(outer.alloc_count(), 1);
            ::operator delete(p2);
        }

        // Inner tracker destroyed; outer tracker resumes with its original count
        void* p3 = ::operator new(32);
        CHECK_EQ(outer.alloc_count(), 2);
        ::operator delete(p3);
        ::operator delete(p1);

        outer.reset();
        CHECK_EQ(outer.alloc_count(), 0);
        CHECK_EQ(outer.bytes_allocated(), 0);
    }
}

void allocation_zero_decompressor() {
    using namespace openrar;
    using namespace openrar::compress;
    using namespace openrar::test;

    const std::vector<uint8_t> orig = generate_test_payload(64 * 1024, 12345);
    std::vector<core::byte> comp;
    CHECK(Compressor50::compress_buffer(orig.data(), orig.size(), comp, 3, 2 * 1024 * 1024));
    CHECK(!comp.empty());

    const Decompressor50::OutputCallback sink = [](const core::byte*, size_t) {
        return true;
    };

    // Case A: VM path (window > 64 MiB, e.g. 128 MiB) - Primary proof of record
    {
        Decompressor50 dec_vm(128 * 1024 * 1024, false);
        CHECK(dec_vm.last_error() == DecompressErrorCode::Ok);

        // Pre-run once so tables are populated
        size_t written = 0;
        bool finished = false;
        CHECK(dec_vm.decompress(comp.data(), comp.size(), orig.size(), false, sink, &written,
                                &finished));
        CHECK_EQ(written, orig.size());

        // Steady-state decode inside ScopedAllocTracker
        {
            ScopedAllocTracker tracker;
            written = 0;
            finished = false;
            CHECK(dec_vm.decompress(comp.data(), comp.size(), orig.size(), false, sink, &written,
                                    &finished));
            CHECK_EQ(written, orig.size());
            CHECK_EQ(tracker.alloc_count(), 0); // Proof of record: ZERO heap allocations
        }
    }

    // Case B: Vector fallback path (window <= 64 MiB) - after prepare_window_for_test
    {
        Decompressor50 dec_vec(2 * 1024 * 1024, false);
        CHECK(dec_vec.last_error() == DecompressErrorCode::Ok);
        CHECK(dec_vec.prepare_window_for_test());

        // Pre-run once for Huffman tables
        size_t written = 0;
        bool finished = false;
        CHECK(dec_vec.decompress(comp.data(), comp.size(), orig.size(), false, sink, &written,
                                 &finished));
        CHECK_EQ(written, orig.size());

        // Steady-state decode inside ScopedAllocTracker
        {
            ScopedAllocTracker tracker;
            written = 0;
            finished = false;
            CHECK(dec_vec.decompress(comp.data(), comp.size(), orig.size(), false, sink, &written,
                                     &finished));
            CHECK_EQ(written, orig.size());
            CHECK_EQ(tracker.alloc_count(), 0); // Proof of record: ZERO heap allocations
        }
    }

    // Case C: Steady-state filtered archive after first block
    {
        const auto bin = make_x86_binary(500);
        std::vector<core::byte> comp_filt;
        CHECK(Compressor50::compress_buffer(bin.data(), bin.size(), comp_filt, 3, 4 * 1024 * 1024));
        CHECK(!comp_filt.empty());

        Decompressor50 dec_filt(4 * 1024 * 1024, false);
        CHECK(dec_filt.prepare_window_for_test());

        // First decode: allocates filter queue and scratchpad (accounted under I-16)
        size_t written = 0;
        bool finished = false;
        CHECK(dec_filt.decompress(comp_filt.data(), comp_filt.size(), bin.size(), false, sink,
                                  &written, &finished));
        CHECK_EQ(written, bin.size());

        // Steady-state decode of filtered archive: ZERO heap allocations
        {
            ScopedAllocTracker tracker;
            written = 0;
            finished = false;
            CHECK(dec_filt.decompress(comp_filt.data(), comp_filt.size(), bin.size(), false, sink,
                                      &written, &finished));
            CHECK_EQ(written, bin.size());
            CHECK_EQ(tracker.alloc_count(), 0); // Bounded ring & scratch retained across decodes
        }
    }
}

void allocation_stream_steady_state() {
    using namespace openrar;
    using namespace openrar::compress;
    using namespace openrar::test;

    const std::vector<uint8_t> orig = generate_test_payload(128 * 1024, 777123);
    const size_t win = 1024 * 1024;
    const int method = 3;
    std::vector<core::byte> comp;
    CHECK(Compressor50::compress_buffer(orig.data(), orig.size(), comp, method, win));
    CHECK(!comp.empty());

    static auto sink_fn = [](void*, const core::byte*, size_t) -> int {
        return 0;
    };

    StreamDecoder dec(win, method);
    CHECK(dec.is_valid());
    CHECK(dec.prepare_window_for_test());
    dec.set_flush(sink_fn, nullptr);
    dec.reserve_staging(comp.size());

    // Pre-run once so initial dictionary and tables are warmed
    std::vector<core::byte> scratch;
    scratch.reserve(orig.size());
    for (size_t off = 0; off < comp.size(); off += 4096) {
        size_t n = std::min<size_t>(4096, comp.size() - off);
        CHECK(dec.feed(comp.data() + off, n));
    }
    CHECK(dec.finish(scratch));

    // Reset decoder, prepare window and reserve staging again
    dec.reset();
    CHECK(dec.prepare_window_for_test());
    dec.set_flush(sink_fn, nullptr);
    dec.reserve_staging(comp.size());

    // Qualified secondary proof: steady-state streaming with flush sink and reserve_staging
    {
        ScopedAllocTracker tracker;
        for (size_t off = 0; off < comp.size(); off += 4096) {
            size_t n = std::min<size_t>(4096, comp.size() - off);
            CHECK(dec.feed(comp.data() + off, n));
        }
        CHECK(dec.finish(scratch));
        if (tracker.alloc_count() != 0) {
            std::fprintf(stderr, "StreamDecoder steady state alloc count: %zu, bytes: %zu\n",
                         tracker.alloc_count(), tracker.bytes_allocated());
        }
        CHECK_EQ(tracker.alloc_count(), 0);
    }
}

void window_hostile_declaration_peak() {
    using namespace openrar;
    using namespace openrar::compress;

    // F-1: Declared window 64 GiB on tiny payload; commits kilobytes, never 64 GiB.
    const size_t hostile_win = 64ULL * 1024 * 1024 * 1024;
    Decompressor50 dec(hostile_win);
    CHECK(dec.win_size() == hostile_win);

    // Decompress a tiny valid payload (triggers lazy allocation of address space)
    const std::vector<uint8_t> tiny = generate_test_payload(64, 42);
    std::vector<core::byte> comp;
    CHECK(Compressor50::compress_buffer(tiny.data(), tiny.size(), comp, 3, 1024 * 1024));
    CHECK(!comp.empty());

    const auto sink = [](const core::byte*, size_t) {
        return true;
    };
    size_t written = 0;
    bool finished = false;
    CHECK(dec.decompress(comp.data(), comp.size(), tiny.size(), false, sink, &written, &finished));
    CHECK_EQ(written, tiny.size());

    // Virtual memory space reserved must be >= 64 GiB
    CHECK(dec.window_buffer().reserved() >= hostile_win);

    // Initial committed pages must be <= INITIAL_WINDOW + granule (1.25 MiB)
    CHECK(dec.window_capacity() <= 256 * 1024);
    CHECK(dec.window_buffer().committed() <= 2 * 1024 * 1024);

#if defined(_WIN32)
    // VirtualQuery proof: pages beyond committed_ remain PAGE_NOACCESS / MEM_RESERVE
    MEMORY_BASIC_INFORMATION mbi{};
    const core::byte* uncommitted_addr =
        dec.window_buffer().data() + dec.window_buffer().committed() + 65536;
    if (VirtualQuery(uncommitted_addr, &mbi, sizeof(mbi))) {
        CHECK(mbi.State == MEM_RESERVE);
        CHECK((mbi.Protect == 0 || mbi.Protect == PAGE_NOACCESS));
    }
#endif
}

void filter_queue_overflow_fail_closed() {
    using namespace openrar;
    using namespace openrar::compress;

    // F-4 negative test: 65,537th pending filter must fail closed with LimitExceeded
    auto queue = std::make_unique<FixedFilterQueue<65536>>();
    FilterEntry fe{};
    fe.block_start = 100;
    fe.block_length = 50;
    fe.type = 1;
    fe.channels = 1;

    for (size_t i = 0; i < 65536; ++i) {
        fe.block_start = (i + 1) * 100;
        CHECK(queue->push(fe));
    }
    CHECK_EQ(queue->size(), 65536);
    CHECK(!queue->push(fe)); // 65,537th push MUST return false

    // Decompressor fail-closed behavior
    Decompressor50 dec(2 * 1024 * 1024);
    for (size_t i = 0; i < 65536; ++i) {
        fe.block_start = (i + 1) * 100;
        CHECK(dec.test_push_filter(fe));
    }
    // The 65,537th filter MUST fail and set last_error to LimitExceeded
    CHECK(!dec.test_push_filter(fe));
    CHECK(dec.last_error() == DecompressErrorCode::LimitExceeded);

    // F-10: Reset after fail-closed returns the decoder to a usable state
    dec.reset();

    const std::vector<uint8_t> tiny = generate_test_payload(128, 999);
    std::vector<core::byte> comp;
    CHECK(Compressor50::compress_buffer(tiny.data(), tiny.size(), comp, 3, 2 * 1024 * 1024));
    CHECK(!comp.empty());

    const auto sink = [](const core::byte*, size_t) {
        return true;
    };
    size_t written = 0;
    bool finished = false;
    CHECK(dec.decompress(comp.data(), comp.size(), tiny.size(), false, sink, &written, &finished));
    CHECK_EQ(written, tiny.size());
    CHECK(dec.last_error() == DecompressErrorCode::Ok);
}

struct StackThreadContext {
    bool success{false};
    std::vector<openrar::core::byte> comp;
    size_t expected_size{0};
};

#if defined(_WIN32)
static unsigned __stdcall small_stack_worker(void* arg) {
    auto* ctx = static_cast<StackThreadContext*>(arg);
    openrar::compress::Decompressor50 dec(1024 * 1024);
    openrar::compress::StreamDecoder sdec(1024 * 1024, 3);
    const auto sink = [](const openrar::core::byte*, size_t) {
        return true;
    };
    size_t written = 0;
    bool finished = false;
    bool ok = dec.decompress(ctx->comp.data(), ctx->comp.size(), ctx->expected_size, false, sink,
                             &written, &finished);
    if (ok && written == ctx->expected_size) {
        ctx->success = true;
    }
    return 0;
}
#else
static void* small_stack_worker(void* arg) {
    auto* ctx = static_cast<StackThreadContext*>(arg);
    openrar::compress::Decompressor50 dec(1024 * 1024);
    openrar::compress::StreamDecoder sdec(1024 * 1024, 3);
    const auto sink = [](const openrar::core::byte*, size_t) {
        return true;
    };
    size_t written = 0;
    bool finished = false;
    bool ok = dec.decompress(ctx->comp.data(), ctx->comp.size(), ctx->expected_size, false, sink,
                             &written, &finished);
    if (ok && written == ctx->expected_size) {
        ctx->success = true;
    }
    return nullptr;
}
#endif

void stack_usage_small_stack() {
    using namespace openrar::compress;

    // Static assertions (I-16 stack safety): sizeof <= 64 KiB
    static_assert(sizeof(Decompressor50) <= 64 * 1024, "Decompressor50 exceeds 64 KiB stack bound");
    static_assert(sizeof(StreamDecoder) <= 64 * 1024, "StreamDecoder exceeds 64 KiB stack bound");

    const std::vector<uint8_t> orig = generate_test_payload(1024, 555);
    StackThreadContext ctx;
    ctx.expected_size = orig.size();
    CHECK(Compressor50::compress_buffer(orig.data(), orig.size(), ctx.comp, 3, 1024 * 1024));
    CHECK(!ctx.comp.empty());

    // Execute on a 256 KiB thread stack
    const size_t kStackSize = 256 * 1024;
#if defined(_WIN32)
    uintptr_t handle = _beginthreadex(nullptr, static_cast<unsigned>(kStackSize),
                                      small_stack_worker, &ctx, 0, nullptr);
    CHECK(handle != 0);
    WaitForSingleObject(reinterpret_cast<HANDLE>(handle), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackSize);
    int rc = pthread_create(&thread, &attr, small_stack_worker, &ctx);
    CHECK_EQ(rc, 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
#endif
    CHECK(ctx.success);
}

// ============================================================================
// Main test suite entry point
// ============================================================================

int main(int argc, char* argv[]) {
    static const openrar::test::Case cases[] = {
        OPENRAR_CASE(allocation_tracker_nesting),
        OPENRAR_CASE(allocation_zero_decompressor),
        OPENRAR_CASE(allocation_stream_steady_state),
        OPENRAR_CASE(window_hostile_declaration_peak),
        OPENRAR_CASE(filter_queue_overflow_fail_closed),
        OPENRAR_CASE(stack_usage_small_stack),
    };
    return openrar::test::openrar_test_main(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
