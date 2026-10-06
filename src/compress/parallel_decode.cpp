#include "parallel_decode.hpp"

#include "../core/types.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrar::compress {
namespace {

constexpr size_t MIB = 1024 * 1024;
// Amortization floor per span (Gate 0 G3) and the span-count topology
// (review R3): spans = clamp(pack / 4 MiB, 2, 4 * workers); engagement
// requires >= 3 spans so at least two symbol workers exist.
constexpr size_t SPAN_PACKED_FLOOR = 4 * MIB;
// Per-span record cap (G5, review R2): records + literal pool together.
// Real corpora measure 2-3x span output in the 12-byte record encoding;
// 16x the span's packed bytes keeps every common shape inside the bound
// and pushes pathological streams to the sequential fallback instead of
// unbounded memory. The derivation lives in the Gate 0 record (SS1/SS2).
constexpr size_t RECORD_CAP_MIN = 16 * MIB;
constexpr size_t RECORD_CAP_PACKED_MULTIPLE = 16;
// Publish window (review R4): a worker that finishes more than this many
// spans ahead of the applier parks before publishing. Resident record
// memory is then bounded by the worker count (each worker holds at most
// its current span) plus the publish window.
constexpr size_t MAX_IN_FLIGHT_AHEAD = 2;

// Testing/measurement override (the threads-override precedent): lowers
// the G3 span floor so small generated members can engage the pipeline in
// fuzzers and identity harnesses. Never set in production.
size_t debug_span_floor() {
    if (const char* f = std::getenv("OPENRAR_PARALLEL_DECODE_SPAN_FLOOR")) {
        unsigned long long v = std::strtoull(f, nullptr, 10);
        if (v > 0 && v <= (1ULL << 30)) return static_cast<size_t>(v);
    }
    return SPAN_PACKED_FLOOR;
}

unsigned resolve_workers() {
    unsigned hw = std::thread::hardware_concurrency();
    if (const char* forced = std::getenv("OPENRAR_PARALLEL_DECODE_THREADS")) {
        unsigned long v = std::strtoul(forced, nullptr, 10);
        if (v >= 1 && v <= 64) hw = static_cast<unsigned>(v);
    }
    if (hw < 2) return 0; // F7
    return std::clamp(hw, 2u, 8u);
}

} // namespace

bool should_use_parallel_decode(size_t pack_size, size_t dest_size, size_t win_size,
                                bool solid_entry, unsigned* out_workers) {
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(_M_IX86) || defined(__i386__)
    (void)pack_size;
    (void)dest_size;
    (void)win_size;
    (void)solid_entry;
    (void)out_workers;
    return false; // G6: WASM/ILP32 never engage.
#else
    if (solid_entry) return false;                               // G1 (scope: non-solid)
    if (std::getenv("OPENRAR_NO_PARALLEL_DECODE")) return false; // D9 kill switch
    if (dest_size == 0 || pack_size == 0) return false;
    // G2 (flavor): the operation records carry u32 distances — the v0
    // table flavor (win <= 4 GiB). v1-flavor members (446 tables) stay
    // sequential this arc.
    if (win_size > (4ULL * 1024 * 1024 * 1024)) return false;
    const unsigned workers = resolve_workers();
    if (workers == 0) return false;
    // G3: the amortization floor keys on PACKED size (Gate 0 SS1: a
    // run-heavy member packs to almost nothing and has nothing to split).
    if (pack_size / workers < debug_span_floor()) return false;
    if (out_workers) *out_workers = workers;
    return true;
#endif
}

bool decode_entry(const core::byte* src, size_t src_size, size_t dest_size, size_t win_size,
                  Decompressor50::OutputCallback flush_cb, ParallelDecodeDiag* diag) {
    if (diag) *diag = ParallelDecodeDiag{};
    if (flush_cb == nullptr || src == nullptr || src_size == 0 || dest_size == 0) return false;

    auto run_sequential = [&]() {
        Decompressor50 seq(win_size);
        return seq.decompress(src, src_size, dest_size, /*solid=*/false, flush_cb);
    };

    unsigned workers = 0;
    if (!should_use_parallel_decode(src_size, dest_size, win_size, /*solid_entry=*/false,
                                    &workers)) {
        return run_sequential();
    }

    // ── Phase 0: pre-scan (fail => sequential, F1) ─────────────────────────
    Decompressor50 ps(win_size);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(src, src_size, tl)) {
        if (diag) diag->fell_back = true;
        return run_sequential();
    }

    // ── Span split (R3): block boundaries nearest an even PACKED split ─────
    size_t target_spans = std::clamp(src_size / debug_span_floor(), size_t{2}, size_t{4} * workers);
    // A member with few blocks splits no further than its block count; the
    // floor stays 3 spans (a 2-span member has one symbol worker - the
    // black-box scaling record: a 4-block member does not scale).
    target_spans = std::min(target_spans, tl.blocks.size());
    if (target_spans < 3) {
        if (diag) diag->fell_back = true;
        return run_sequential();
    }
    const size_t nspans = target_spans;
    std::vector<uint32_t> span_first(nspans, 0), span_last(nspans, 0);
    {
        // Block packed extent = header + payload; prefix-walk and cut after
        // the block that crosses each even fraction.
        std::vector<size_t> prefix(tl.blocks.size() + 1, 0);
        for (size_t i = 0; i < tl.blocks.size(); ++i) {
            const auto& b = tl.blocks[i];
            prefix[i + 1] = prefix[i] + b.header_len + b.block_size;
        }
        size_t bi = 0;
        for (size_t s = 0; s < nspans; ++s) {
            const size_t target = src_size * (s + 1) / nspans;
            span_first[s] = static_cast<uint32_t>(bi);
            while (bi + 1 < tl.blocks.size() && (s + 1 == nspans || prefix[bi + 1] < target)) {
                ++bi;
            }
            span_last[s] = static_cast<uint32_t>(bi);
            ++bi;
            if (bi > tl.blocks.size()) { // defensive: degenerate split
                if (diag) diag->fell_back = true;
                return run_sequential();
            }
        }
        // Every span must be non-empty and cover the block chain exactly.
        for (size_t s = 0; s + 1 < nspans; ++s) {
            if (span_first[s] > span_last[s] || span_last[s] + 1 != span_first[s + 1]) {
                if (diag) diag->fell_back = true;
                return run_sequential();
            }
        }
        if (span_last[nspans - 1] != tl.blocks.size() - 1) {
            if (diag) diag->fell_back = true;
            return run_sequential();
        }
    }
    if (std::getenv("OPENRAR_PD_TRACE")) {
        std::fprintf(stderr, "[pd] engaged nspans=%zu workers=%u blocks=%zu\n", nspans, workers,
                     tl.blocks.size());
    }
    if (diag) {
        diag->engaged = true;
        diag->spans = static_cast<unsigned>(nspans);
        diag->workers = workers;
        diag->prescan_blocks = tl.blocks.size();
    }

    // ── Pipeline state (R4): one mutex, publish-gated workers ──────────────
    // Events are per-span (tens per member), so a single mutex carries no
    // hot-path contention; the original P1/P2 pins (mutex-published
    // progress, notify_all on failure, padded per-range state) apply to
    // the per-range frontier scheme they were written for — this pipeline's
    // shared state is one cursor and per-span slots.
    struct Sync {
        std::mutex mu;
        std::condition_variable cv;
        size_t applier_pos{0}; // spans consumed by the applier
        size_t published{0};   // spans published by workers (in order)
        bool failed{false};
        bool cancelled{false};
        std::string failure;
    } sync;
    std::vector<std::unique_ptr<Decompressor50::SpanRecords>> span_data(nspans);

    auto set_failed = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(sync.mu);
        if (!sync.failed) {
            sync.failed = true;
            sync.failure = msg;
        }
        sync.cv.notify_all();
    };

    // ── Phase-1 workers ────────────────────────────────────────────────────
    std::vector<std::thread> threads;
    auto join_all = [&]() {
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    };
    auto worker_body = [&](unsigned w) {
        Decompressor50 wdec(win_size);
        for (size_t s = w; s < nspans; s += workers) {
            // Decode freely; publish through the bounded window.
            const uint32_t first = span_first[s], last = span_last[s];
            const size_t span_packed = tl.blocks[last].src_byte + tl.blocks[last].header_len +
                                       tl.blocks[last].block_size - tl.blocks[first].src_byte;
            const size_t rec_cap =
                std::max(RECORD_CAP_MIN, RECORD_CAP_PACKED_MULTIPLE * span_packed);
            auto sr = std::make_unique<Decompressor50::SpanRecords>();
            if (std::getenv("OPENRAR_PD_TRACE"))
                std::fprintf(stderr, "[pd] w%u span %zu start\n", w, s);
            if (!wdec.decode_span(tl, src, src_size, dest_size, first, last, rec_cap, *sr)) {
                set_failed(wdec.last_error_message());
                return;
            }
            if (std::getenv("OPENRAR_PD_TRACE"))
                std::fprintf(stderr, "[pd] w%u span %zu decoded recs=%zu\n", w, s, sr->recs.size());
            std::unique_lock<std::mutex> lk(sync.mu);
            // Turn-ordered publish: `published` is the next span to publish,
            // so the applier can never observe a later span before an
            // earlier one (span_data[s] must be non-null when it consumes).
            // The in-flight gate bounds published-but-unconsumed spans.
            sync.cv.wait(lk, [&] {
                return sync.cancelled || sync.failed ||
                       (s == sync.published && s < sync.applier_pos + MAX_IN_FLIGHT_AHEAD);
            });
            if (sync.cancelled || sync.failed) return;
            span_data[s] = std::move(sr);
            ++sync.published;
            lk.unlock();
            sync.cv.notify_all();
        }
    };

    // R decode workers + the caller as the applier: the pipeline needs the
    // symbol stage fully parallel while one thread applies in order.
    try {
        threads.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) threads.emplace_back(worker_body, w);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(sync.mu);
            sync.cancelled = true;
        }
        sync.cv.notify_all();
        join_all();
        // F3: spawn failure, nothing emitted — deterministic-safe retry.
        if (diag) diag->fell_back = true;
        return run_sequential();
    }

    // ── Phase 2: the applier on the caller's thread ────────────────────────
    Decompressor50 applier(win_size);
    bool applied_ok = applier.apply_begin();
    size_t total = 0;
    bool aborted = false;
    if (applied_ok) {
        for (size_t s = 0; s < nspans; ++s) {
            std::unique_ptr<Decompressor50::SpanRecords> sr;
            {
                std::unique_lock<std::mutex> lk(sync.mu);
                sync.cv.wait(lk,
                             [&] { return sync.cancelled || sync.failed || (s < sync.published); });
                if (sync.cancelled || sync.failed) {
                    aborted = true;
                    break;
                }
                sr = std::move(span_data[s]);
            }
            if (std::getenv("OPENRAR_PD_TRACE"))
                std::fprintf(stderr, "[pd] apply span %zu begin\n", s);
            if (!applier.apply_span_records(*sr, dest_size, s + 1 == nspans, flush_cb, &total)) {
                set_failed(applier.last_error_message());
                aborted = true;
                break;
            }
            sr.reset(); // free records as soon as the span is consumed
            {
                std::lock_guard<std::mutex> lk(sync.mu);
                sync.applier_pos = s + 1;
            }
            sync.cv.notify_all();
            // Consumer abort (F8): flush_cb returning false fails
            // apply_span_records above -> set_failed -> aborted -> join;
            // bytes already emitted are the sequential decoder's identical
            // mid-stream-failure contract [R5].
        }
    } else {
        set_failed("applier init failed");
        aborted = true;
    }

    // Join with cancel signaled so parked publishers wake.
    {
        std::lock_guard<std::mutex> lk(sync.mu);
        if (aborted && !sync.failed) sync.cancelled = true;
    }
    sync.cv.notify_all();
    join_all();

    if (aborted) {
        if (total == 0) {
            // F1/F2/F3 pre-emission failure: the sequential path owns the
            // verdict (fail-safe-identical). The diag's fell_back flag keeps
            // the fallback observable so the identity tests see any masking.
            if (diag) diag->fell_back = true;
            return run_sequential();
        }
        // R5: post-emission failure = the sequential decoder's identical
        // mid-stream failure; the consumer's containment discards.
        return false;
    }
    return true;
}

} // namespace openrar::compress
