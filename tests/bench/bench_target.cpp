// bench_target.cpp - compress / decompress hot-path throughput (MB/s).
//
// Written against the Google Benchmark API. When the build finds Google
// Benchmark (find_package(benchmark), OPENRAR_HAVE_GOOGLE_BENCHMARK) it links
// the real library; otherwise the minimal API-compatible runner at the bottom
// of this file is used, so the harness builds with no extra dependency.
//
// Matrix: block size {64 B, 4 KiB, 64 KiB, 1 MiB} x corpus {text, binary,
// random} x {compress m1/m3/m5, decompress}. Throughput is reported as
// decimal MB/s of UNCOMPRESSED bytes in both directions.
//
// Zero-allocation timing loops: every buffer (source, compressed, output)
// and every codec object (Compressor50, Decompressor50, its window, the
// output sink) is created before the timed loop and reused across
// iterations. The loop body is the codec call only. A global operator
// new/delete hook counts heap allocations made inside the timed region and
// reports them per iteration as `allocs/iter`, so "zero allocation" is
// measured, not assumed. Output size and ratio are reported as counters.
//
// Usage:  bench_target [--benchmark_filter=<substr>] [--benchmark_min_time=<sec>]
// (Both flags work with the built-in runner; the real library accepts its
// full flag set.)

#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/filters50.hpp"
#include "../../src/core/types.hpp"

#include <atomic>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if defined(OPENRAR_HAVE_GOOGLE_BENCHMARK)
#include <benchmark/benchmark.h>
#else
namespace benchmark {
class State;
}
#define OPENRAR_BENCH_SHIM 1
#endif

using namespace openrar;
using namespace openrar::compress;

// ---------------------------------------------------------------------------
// Allocation accounting (replaceable global operator new/delete)
// ---------------------------------------------------------------------------
//
// The sanitizer runtimes (ASan/TSan/MSan) define the global operator
// new/delete themselves, so a replacement in this translation unit is a
// duplicate definition at link time. Under a sanitizer the accounting is
// compiled out and the scope reports zero allocations - the legs that read
// the counter are not sanitizer legs.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__)
#define OPENRAR_BENCH_NO_ALLOC_SHIM 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define OPENRAR_BENCH_NO_ALLOC_SHIM 1
#endif
#endif

#if !defined(OPENRAR_BENCH_NO_ALLOC_SHIM)

namespace {
std::atomic<bool> g_count_allocs{false};
std::atomic<std::uint64_t> g_allocs{0};

inline void note_alloc() {
    if (g_count_allocs.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
}

// RAII scope around exactly the timed loop.
struct AllocScope {
    std::uint64_t start;
    AllocScope() : start(g_allocs.load()) { g_count_allocs.store(true); }
    ~AllocScope() { g_count_allocs.store(false); }
    std::uint64_t count() const { return g_allocs.load() - start; }
};
} // namespace

// This shim deliberately backs the global operator new/delete with
// malloc/free so the allocation counter can see every allocation. gcc's
// -Wmismatched-new-delete assumes the default library pairing and flags the
// free() inside the replacement deletes; the pairing here is intentional, so
// the diagnostic is suppressed for this block only (gcc only - clang has no
// such warning group, and an unknown group would itself be a warning).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    note_alloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    note_alloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    note_alloc();
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    note_alloc();
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
// The sized and nothrow overloads delegate to their unsized counterpart
// instead of calling free() directly: the C++14 sized delete releases a
// pointer that came from the unsized operator new, and gcc's
// -Wmismatched-new-delete rejects a direct free() on that pairing.
void operator delete(void* p, std::size_t) noexcept {
    operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    operator delete[](p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    operator delete(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    operator delete[](p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#else // OPENRAR_BENCH_NO_ALLOC_SHIM

// Under a sanitizer the global operators belong to the sanitizer runtime;
// the accounting scope stays available and reports zero.
namespace {
struct AllocScope {
    std::uint64_t count() const { return 0; }
};
} // namespace

#endif // OPENRAR_BENCH_NO_ALLOC_SHIM

#if defined(OPENRAR_BENCH_SHIM)
// ---------------------------------------------------------------------------
// Minimal Google Benchmark-compatible runner (subset used by this file)
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>

namespace benchmark {

template <class T> inline void DoNotOptimize(T const& value) {
#if defined(_MSC_VER) && !defined(__clang__)
    const volatile void* sink = &value;
    (void)sink;
    _ReadWriteBarrier();
#else
    asm volatile("" : : "r,m"(value) : "memory");
#endif
}
inline void ClobberMemory() {
#if defined(_MSC_VER) && !defined(__clang__)
    _ReadWriteBarrier();
#else
    asm volatile("" : : : "memory");
#endif
}

struct Counter {
    enum Flags { kDefaults = 0, kIsRate = 1, kAvgIterations = 2 };
    double value{0};
    int flags{kDefaults};
    Counter(double v = 0, int f = kDefaults) : value(v), flags(f) {}
};

class State {
public:
    State(std::int64_t max_iters, std::vector<std::int64_t> args)
        : max_iters_(max_iters), args_(std::move(args)) {}

    struct Iterator {
        State* s;
        std::int64_t left;
        bool operator!=(const Iterator&) const {
            if (left > 0) return true;
            s->stop();
            return false;
        }
        Iterator& operator++() {
            --left;
            return *this;
        }
        int operator*() const { return 0; }
    };
    Iterator begin() {
        start();
        return Iterator{this, max_iters_};
    }
    Iterator end() { return Iterator{this, 0}; }

    std::int64_t range(std::size_t i = 0) const { return i < args_.size() ? args_[i] : 0; }
    std::int64_t iterations() const { return max_iters_; }
    void SetBytesProcessed(std::int64_t b) { bytes_ = b; }
    void SetLabel(const std::string& l) { label_ = l; }
    void SkipWithError(const char* msg) {
        error_ = msg ? msg : "error";
        max_iters_ = 0;
    }
    void PauseTiming() { paused_at_ = clock::now(); }
    void ResumeTiming() { paused_ += clock::now() - paused_at_; }

    std::map<std::string, Counter> counters;

    // Runner access.
    double seconds() const { return std::chrono::duration<double>(elapsed_).count(); }
    std::int64_t bytes() const { return bytes_; }
    const std::string& label() const { return label_; }
    const std::string& error() const { return error_; }

private:
    using clock = std::chrono::steady_clock;
    void start() { t0_ = clock::now(); }
    void stop() {
        if (!stopped_) elapsed_ = clock::now() - t0_ - paused_;
        stopped_ = true;
    }
    std::int64_t max_iters_;
    std::vector<std::int64_t> args_;
    std::int64_t bytes_{0};
    std::string label_, error_;
    clock::time_point t0_{}, paused_at_{};
    clock::duration elapsed_{}, paused_{};
    bool stopped_{false};
};

class Benchmark {
public:
    using Fn = void (*)(State&);
    Benchmark(std::string name, Fn fn) : name_(std::move(name)), fn_(fn) {}
    Benchmark* Args(const std::vector<std::int64_t>& a) {
        arg_sets_.push_back(a);
        return this;
    }
    Benchmark* ArgsProduct(const std::vector<std::vector<std::int64_t>>& axes) {
        std::vector<std::vector<std::int64_t>> out{{}};
        for (const auto& axis : axes) {
            std::vector<std::vector<std::int64_t>> next;
            for (const auto& prefix : out)
                for (std::int64_t v : axis) {
                    auto row = prefix;
                    row.push_back(v);
                    next.push_back(std::move(row));
                }
            out = std::move(next);
        }
        arg_sets_.insert(arg_sets_.end(), out.begin(), out.end());
        return this;
    }
    Benchmark* ArgNames(const std::vector<std::string>& n) {
        arg_names_ = n;
        return this;
    }
    Benchmark* Unit(int) { return this; }
    Benchmark* MinTime(double s) {
        min_time_ = s;
        return this;
    }

    const std::string& name() const { return name_; }
    Fn fn() const { return fn_; }
    double min_time() const { return min_time_; }
    const std::vector<std::vector<std::int64_t>>& arg_sets() const { return arg_sets_; }
    std::string run_name(const std::vector<std::int64_t>& a) const {
        std::string s = name_;
        for (std::size_t i = 0; i < a.size(); ++i) {
            s += "/";
            if (i < arg_names_.size()) s += arg_names_[i] + ":";
            s += std::to_string(a[i]);
        }
        return s;
    }

private:
    std::string name_;
    Fn fn_;
    double min_time_{0};
    std::vector<std::vector<std::int64_t>> arg_sets_;
    std::vector<std::string> arg_names_;
};

enum TimeUnit { kNanosecond, kMicrosecond, kMillisecond };

inline std::vector<std::unique_ptr<Benchmark>>& registry() {
    static std::vector<std::unique_ptr<Benchmark>> r;
    return r;
}
inline Benchmark* RegisterBenchmark(const char* name, Benchmark::Fn fn) {
    registry().push_back(std::make_unique<Benchmark>(name, fn));
    return registry().back().get();
}

inline int RunAll(int argc, char** argv) {
    std::string filter;
    double min_time = 0.5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--benchmark_filter=", 0) == 0) filter = a.substr(19);
        if (a.rfind("--benchmark_min_time=", 0) == 0) min_time = std::atof(a.c_str() + 21);
    }
    std::printf("%-46s %14s %10s %11s  %s\n", "Benchmark", "Time/iter", "Iters", "MB/s",
                "Counters");
    std::printf("%s\n", std::string(110, '-').c_str());
    for (const auto& b : registry()) {
        auto sets = b->arg_sets();
        if (sets.empty()) sets.push_back({});
        for (const auto& args : sets) {
            const std::string name = b->run_name(args);
            if (!filter.empty() && name.find(filter) == std::string::npos) continue;
            const double target = b->min_time() > 0 ? b->min_time() : min_time;
            // Calibrate like the real library: grow iterations until one
            // run covers the target time, then report that run.
            std::int64_t iters = 1;
            for (;;) {
                State st(iters, args);
                b->fn()(st);
                if (!st.error().empty()) {
                    std::printf("%-46s ERROR: %s\n", name.c_str(), st.error().c_str());
                    break;
                }
                const double secs = st.seconds();
                if (secs >= target || iters >= (std::int64_t(1) << 30)) {
                    const double per_iter = secs / static_cast<double>(iters);
                    const double mbps = static_cast<double>(st.bytes()) / secs / 1e6;
                    std::string extra;
                    for (const auto& [k, c] : st.counters) {
                        double v = c.value;
                        if (c.flags & Counter::kIsRate) v /= secs;
                        if (c.flags & Counter::kAvgIterations) v /= static_cast<double>(iters);
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%s=%.4g ", k.c_str(), v);
                        extra += buf;
                    }
                    char tbuf[32];
                    if (per_iter < 1e-6)
                        std::snprintf(tbuf, sizeof(tbuf), "%.1f ns", per_iter * 1e9);
                    else if (per_iter < 1e-3)
                        std::snprintf(tbuf, sizeof(tbuf), "%.2f us", per_iter * 1e6);
                    else
                        std::snprintf(tbuf, sizeof(tbuf), "%.3f ms", per_iter * 1e3);
                    std::printf("%-46s %14s %10lld %11.1f  %s%s\n", name.c_str(), tbuf,
                                static_cast<long long>(iters), mbps, extra.c_str(),
                                st.label().c_str());
                    break;
                }
                const double grow = secs > 0 ? std::min(10.0, 1.4 * target / secs) : 10.0;
                iters = std::max<std::int64_t>(iters + 1, static_cast<std::int64_t>(iters * grow));
            }
        }
    }
    return 0;
}

} // namespace benchmark

#define OPENRAR_BENCH_CAT2(a, b) a##b
#define OPENRAR_BENCH_CAT(a, b) OPENRAR_BENCH_CAT2(a, b)
#define BENCHMARK(fn)                                                                              \
    static ::benchmark::Benchmark* OPENRAR_BENCH_CAT(bench_reg_, __LINE__) =                       \
        ::benchmark::RegisterBenchmark(#fn, fn)
#define BENCHMARK_MAIN()                                                                           \
    int main(int argc, char** argv) {                                                              \
        return ::benchmark::RunAll(argc, argv);                                                    \
    }
#endif // OPENRAR_BENCH_SHIM

// ---------------------------------------------------------------------------
// Corpora (deterministic)
// ---------------------------------------------------------------------------

namespace {

enum Corpus : int { kText = 0, kBinary = 1, kRandom = 2 };
const char* corpus_name(int c) {
    return c == kText ? "text" : c == kBinary ? "binary" : "random";
}

// Word-salad English-like text: ~3:1 under LZ, realistic match lengths.
std::vector<core::byte> make_text(std::size_t n, std::uint32_t seed) {
    static const char* words[] = {
        "the",      "archive", "header",  "block",  "window", "match",    "literal", "distance",
        "huffman",  "table",   "symbol",  "stream", "filter", "delta",    "offset",  "length",
        "of",       "and",     "to",      "in",     "is",     "with",     "for",     "that",
        "compress", "decode",  "encoder", "buffer", "volume", "recovery", "record",  "parity"};
    std::vector<core::byte> out;
    out.reserve(n + 16);
    std::uint32_t x = seed;
    while (out.size() < n) {
        x = x * 1664525u + 1013904223u;
        const char* w = words[(x >> 16) % (sizeof(words) / sizeof(words[0]))];
        out.insert(out.end(), w, w + std::strlen(w));
        out.push_back((x & 0x1F) == 0 ? '\n' : ' ');
    }
    out.resize(n);
    return out;
}

// Structured binary: 16-bit sample-like ramps with noise (delta-friendly),
// interleaved with repeated 32-byte records.
std::vector<core::byte> make_binary(std::size_t n, std::uint32_t seed) {
    std::vector<core::byte> out(n);
    std::uint32_t x = seed;
    std::uint16_t s = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        x = x * 1664525u + 1013904223u;
        s = static_cast<std::uint16_t>(s + 37 + ((x >> 24) & 7));
        out[i] = static_cast<core::byte>(s);
        out[i + 1] = static_cast<core::byte>(s >> 8);
        if ((i & 0x3FF) == 0x200) {
            for (std::size_t k = 0; k < 32 && i + k < n; ++k)
                out[i + k] = static_cast<core::byte>(0xA0 + (k & 7));
        }
    }
    if (n & 1) out[n - 1] = 0;
    return out;
}

std::vector<core::byte> make_random(std::size_t n, std::uint32_t seed) {
    std::vector<core::byte> out(n);
    std::uint64_t x = seed | 1;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        b = static_cast<core::byte>(x >> 32);
    }
    return out;
}

std::vector<core::byte> make_corpus(int c, std::size_t n) {
    return c == kText ? make_text(n, 1) : c == kBinary ? make_binary(n, 2) : make_random(n, 3);
}

// Smallest power-of-two window >= the block (the encoder's 64 KiB floor):
// keeps per-call window reset cost proportional to the block being timed.
std::size_t window_for(std::size_t block) {
    std::size_t w = 0x10000;
    while (w < block) w <<= 1;
    return w;
}

// One-shot compression through a REUSED Compressor50: the calls
// compress_buffer() makes on its unfiltered path, minus constructing a fresh
// packer (and its window-sized hash chains) per call. After the first call
// every vector inside the packer is at capacity. Filter detection and the
// pre-transform are timed separately (BM_DeltaEncode / BM_DeltaDecode), so
// this measures the LZ + entropy-coding hot path on its own.
bool compress_into(Compressor50& packer, const std::vector<core::byte>& src,
                   std::vector<core::byte>& dest, int method, std::size_t win) {
    dest.clear();
    packer.begin_archive(nullptr, method, win);
    FilterConfig no_filters;
    no_filters.mode = FilterMode::DisableAll;
    packer.set_filter_config(no_filters);
    packer.set_external_buffer(src.data(), src.size());
    packer.set_memory_dest(&dest);
    return packer.compress() >= 0 && !dest.empty();
}

// Pre-sized output sink: one memcpy per chunk into a buffer reserved
// before the timed loop.
struct PresizedSink {
    std::vector<core::byte>* out;
    std::size_t pos{0};
    bool operator()(const core::byte* d, std::size_t n) {
        if (pos + n > out->size()) return false;
        std::memcpy(out->data() + pos, d, n);
        pos += n;
        return true;
    }
};

void report(benchmark::State& state, std::size_t raw, std::size_t packed, std::uint64_t allocs,
            int corpus) {
    const std::int64_t iters = state.iterations();
    state.SetBytesProcessed(iters * static_cast<std::int64_t>(raw));
    state.counters["MB/s"] = benchmark::Counter(
        static_cast<double>(iters) * static_cast<double>(raw) / 1e6, benchmark::Counter::kIsRate);
    state.counters["ratio"] =
        benchmark::Counter(packed ? static_cast<double>(raw) / static_cast<double>(packed) : 0.0);
    state.counters["allocs/iter"] =
        benchmark::Counter(iters ? static_cast<double>(allocs) / static_cast<double>(iters) : 0.0);
    state.SetLabel(corpus_name(corpus));
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

void BM_Compress(benchmark::State& state) {
    const std::size_t block = static_cast<std::size_t>(state.range(0));
    const int corpus = static_cast<int>(state.range(1));
    const int method = static_cast<int>(state.range(2));
    const std::size_t win = window_for(block);

    // --- setup (untimed): data, packer, output capacity, warm-up ---
    const std::vector<core::byte> src = make_corpus(corpus, block);
    std::vector<core::byte> dest;
    dest.reserve(block + block / 8 + 4096);
    Compressor50 packer;
    if (!compress_into(packer, src, dest, method, win)) {
        state.SkipWithError("compression failed");
        return;
    }
    const std::size_t packed = dest.size();
    dest.reserve(packed * 2 + 4096); // headroom so the loop never regrows it

    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (auto _ : state) {
            (void)_;
            const bool ok = compress_into(packer, src, dest, method, win);
            benchmark::DoNotOptimize(ok);
            benchmark::DoNotOptimize(dest.data());
            benchmark::ClobberMemory();
        }
        allocs = scope.count();
    }
    report(state, block, packed, allocs, corpus);
}

void BM_Decompress(benchmark::State& state) {
    const std::size_t block = static_cast<std::size_t>(state.range(0));
    const int corpus = static_cast<int>(state.range(1));
    const std::size_t win = window_for(block);

    // --- setup (untimed): compressed input, decoder + window, sink ---
    const std::vector<core::byte> src = make_corpus(corpus, block);
    std::vector<core::byte> comp;
    {
        Compressor50 packer;
        comp.reserve(block + block / 8 + 4096);
        if (!compress_into(packer, src, comp, 3, win)) {
            state.SkipWithError("compression failed");
            return;
        }
    }
    std::vector<core::byte> out(block);
    Decompressor50 dec(win);
    PresizedSink sink{&out};
    // Built once: decompress() takes the callback by value, and copying a
    // std::function holding a reference_wrapper never allocates.
    const Decompressor50::OutputCallback cb = std::ref(sink);
    if (!dec.decompress(comp.data(), comp.size(), block, false, cb) || sink.pos != block ||
        out != src) {
        state.SkipWithError("warm-up round trip failed");
        return;
    }

    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (auto _ : state) {
            (void)_;
            sink.pos = 0;
            const bool ok = dec.decompress(comp.data(), comp.size(), block, false, cb);
            benchmark::DoNotOptimize(ok);
            benchmark::DoNotOptimize(out.data());
            benchmark::ClobberMemory();
        }
        allocs = scope.count();
    }
    if (sink.pos != block) {
        state.SkipWithError("timed decode produced short output");
        return;
    }
    report(state, block, comp.size(), allocs, corpus);
}

// Delta filter kernels (H-08/H-09): in place, so after the first call the
// multi-channel path reuses its per-thread scratch and never allocates.
void BM_DeltaEncode(benchmark::State& state) {
    const std::size_t block = static_cast<std::size_t>(state.range(0));
    const auto channels = static_cast<core::uint8>(state.range(1));
    std::vector<core::byte> buf = make_binary(block, 9);
    Filters50::encode_delta(buf.data(), buf.data(), block, channels); // warm-up
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (auto _ : state) {
            (void)_;
            const bool ok = Filters50::encode_delta(buf.data(), buf.data(), block, channels);
            benchmark::DoNotOptimize(ok);
            benchmark::ClobberMemory();
        }
        allocs = scope.count();
    }
    report(state, block, block, allocs, kBinary);
}

void BM_DeltaDecode(benchmark::State& state) {
    const std::size_t block = static_cast<std::size_t>(state.range(0));
    const auto channels = static_cast<core::uint8>(state.range(1));
    std::vector<core::byte> buf = make_binary(block, 9);
    Filters50::apply_delta(buf.data(), buf.data(), block, channels); // warm-up
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (auto _ : state) {
            (void)_;
            const bool ok = Filters50::apply_delta(buf.data(), buf.data(), block, channels);
            benchmark::DoNotOptimize(ok);
            benchmark::ClobberMemory();
        }
        allocs = scope.count();
    }
    report(state, block, block, allocs, kBinary);
}

const std::vector<std::int64_t> kBlocks = {64, 4 << 10, 64 << 10, 1 << 20};

} // namespace

BENCHMARK(BM_DeltaEncode)
    ->ArgsProduct({kBlocks, {1, 2, 3, 4}})
    ->ArgNames({"block", "channels"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DeltaDecode)
    ->ArgsProduct({kBlocks, {1, 2, 3, 4}})
    ->ArgNames({"block", "channels"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Compress)
    ->ArgsProduct({kBlocks, {kText, kBinary, kRandom}, {1, 3, 5}})
    ->ArgNames({"block", "corpus", "method"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Decompress)
    ->ArgsProduct({kBlocks, {kText, kBinary, kRandom}})
    ->ArgNames({"block", "corpus"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
