#include "parallel_decode.hpp"

#include "../core/types.hpp"
#include "filters50.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace openrar::compress {
namespace {

// Per-range synchronization state (P1: mutex-published frontiers; P2: one
// cache line each so producers and waiters do not share lines).
struct alignas(64) RangeState {
    std::mutex mu;
    std::condition_variable cv;
    size_t frontier{0};
    bool failed{false};
    bool done{false};
};

struct RangeSyncImpl final : Decompressor50::RangeSync {
    // The redundant-overlap design needs no cross-range synchronization:
    // every worker's match sources resolve inside its own window history.
    // The sync object carries only the cancel flag and the failure record.
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool cancelled() const override { return cancelled_.load(std::memory_order_acquire); }

    std::atomic<bool> failed_{false};
    std::atomic<bool> cancelled_{false};
};

struct WorkerFailure {
    std::mutex mu;
    std::string message;
    bool any{false};
};

void record_failure(WorkerFailure& f, const std::string& msg) {
    std::lock_guard<std::mutex> lk(f.mu);
    if (!f.any) {
        f.any = true;
        f.message = msg;
    }
}

} // namespace

bool should_use_parallel_decode(size_t pack_size, size_t dest_size, bool solid_entry) {
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(_M_IX86) || defined(__i386__)
    (void)pack_size;
    (void)dest_size;
    (void)solid_entry;
    return false; // G6: WASM/ILP32 never engage.
#else
    if (solid_entry) return false;                               // G1 (v1.37 scope: non-solid)
    if (std::getenv("OPENRAR_NO_PARALLEL_DECODE")) return false; // D9 kill switch
    size_t budget = 1ULL * 1024 * 1024 * 1024;                   // G5: pack + dest in RAM
    if (pack_size > budget || dest_size > budget - pack_size) return false;

    unsigned hw = std::thread::hardware_concurrency();
    if (const char* forced = std::getenv("OPENRAR_PARALLEL_DECODE_THREADS")) {
        unsigned long v = std::strtoul(forced, nullptr, 10);
        if (v >= 1 && v <= 64) hw = static_cast<unsigned>(v);
    }
    if (hw < 2) return false; // F7
    unsigned workers = std::min<unsigned>(hw, 8);
    // G3: amortization floor (>= 4 MiB packed per range).
    if (pack_size / workers < 4u * 1024 * 1024) return false;
    (void)dest_size;
    return true;
#endif
}

bool decode_entry(const core::byte* src, size_t src_size, size_t dest_size, size_t win_size,
                  Decompressor50::OutputCallback flush_cb, ParallelDecodeDiag* diag) {
    if (diag) *diag = ParallelDecodeDiag{};
    if (flush_cb == nullptr || src == nullptr || src_size == 0 || dest_size == 0) return false;

    // ── Scout: symbol-only pass producing per-range start state (plan M2). ──
    Decompressor50 scout(win_size);
    Decompressor50::ScoutRecord rec;
    if (!scout.scout_member(src, src_size, dest_size, rec, 0)) {
        // F1: the scout is at least as strict as the sequential decoder, but
        // the sequential path stays the sole owner of the failure taxonomy —
        // run it and let it decide (also covers streams whose scout parse we
        // have not modeled).
        Decompressor50 seq(win_size);
        return seq.decompress(src, src_size, dest_size, /*solid=*/false, flush_cb);
    }

    // ── Split at block-start checkpoints nearest an even output split. ──────
    unsigned hw = std::thread::hardware_concurrency();
    if (const char* forced = std::getenv("OPENRAR_PARALLEL_DECODE_THREADS")) {
        unsigned long v = std::strtoul(forced, nullptr, 10);
        if (v >= 1 && v <= 64) hw = static_cast<unsigned>(v);
    }
    const unsigned R = std::clamp<unsigned>(std::min(hw, 8u), 2, 8);
    std::vector<size_t> range_start_block; // first block index per range
    std::vector<size_t> range_start_out;   // output position per range
    range_start_block.push_back(0);
    range_start_out.push_back(0);
    const size_t total = rec.total_output;
    for (unsigned r = 1; r < R; ++r) {
        size_t target = total * r / R;
        // First checkpoint with output_pos >= target (checkpoints sit at
        // block starts, one per block, ascending).
        size_t best = rec.checkpoints.size() - 1;
        for (size_t c = 0; c < rec.checkpoints.size(); ++c) {
            if (rec.checkpoints[c].output_pos >= target) {
                best = c;
                break;
            }
        }
        const Decompressor50::ScoutCheckpoint& cp = rec.checkpoints[best];
        if (cp.next_block > range_start_block.back() && cp.output_pos > range_start_out.back() &&
            cp.next_block < rec.blocks.size()) {
            range_start_block.push_back(cp.next_block);
            range_start_out.push_back(cp.output_pos);
        }
    }
    const size_t NR = range_start_block.size();
    if (NR < 2) {
        // Degenerate split (tiny output): sequential path.
        Decompressor50 seq(win_size);
        return seq.decompress(src, src_size, dest_size, /*solid=*/false, flush_cb);
    }
    if (diag) {
        diag->engaged = true;
        diag->ranges = NR;
        diag->scout_bytes = src_size;
    }

    // ── Shared pre-transform buffer (absolute coordinates). ─────────────────
    std::vector<core::byte> buf;
    try {
        buf.assign(dest_size, 0); // zero-filled: matches the sequential
                                  // decoder's zero-initialized window for
                                  // below-entry references.
    } catch (const std::bad_alloc&) {
        return false; // F3
    }

    RangeSyncImpl sync;

    // Redundant-overlap range starts: range r (> 0) begins at the last
    // checkpoint with output_pos <= (base_r - win_size) so the worker's own
    // ring window covers every match source its range can reference. Range 0
    // starts at the member's first block. Writes begin at base_r.
    std::vector<size_t> first_blk(NR, 0);  // first WRITTEN block per range
    std::vector<size_t> decode_blk(NR, 0); // first DECODED block (overlap start)
    std::vector<size_t> write_from(NR, 0);
    for (size_t r = 0; r < NR; ++r) {
        first_blk[r] = range_start_block[r];
        write_from[r] = range_start_out[r];
        decode_blk[r] = range_start_block[r];
        if (r > 0) {
            const size_t floor = write_from[r] >= win_size ? write_from[r] - win_size : 0;
            // Last checkpoint with output_pos <= floor.
            for (size_t c = rec.checkpoints.size(); c-- > 0;) {
                if (rec.checkpoints[c].output_pos <= floor) {
                    decode_blk[r] = rec.checkpoints[c].next_block;
                    break;
                }
            }
        }
    }

    WorkerFailure failure;
    std::vector<size_t> written(NR, 0);
    auto run_range = [&](size_t ri) {
        Decompressor50 w(win_size);
        size_t w_written = 0;
        bool ok = false;
        try {
            ok = w.decode_range(rec, src, src_size, static_cast<uint32_t>(decode_blk[ri]),
                                static_cast<uint32_t>((ri + 1 < NR) ? range_start_block[ri + 1] - 1
                                                                    : rec.blocks.size() - 1),
                                rec.checkpoints[decode_blk[ri]], buf.data(), dest_size,
                                write_from[ri], &sync, &w_written);
            if (!ok) record_failure(failure, w.last_error_message());
        } catch (const std::exception& e) {
            ok = false;
            record_failure(failure, e.what());
        } catch (...) {
            ok = false;
            record_failure(failure, "worker exception");
        }
        // w_written counts the overlap too; the per-range written span for
        // validation is the scout's recorded span.
        written[ri] =
            (ri + 1 < NR ? range_start_out[ri + 1] : rec.total_output) - range_start_out[ri];
        if (!ok && failure.message.empty()) {
            record_failure(failure, "range failed");
        }
    };

    std::vector<std::thread> threads;
    auto join_all = [&]() {
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    };
    try {
        threads.reserve(NR - 1);
        for (size_t ri = 1; ri < NR; ++ri) {
            threads.emplace_back(run_range, ri);
        }
    } catch (...) {
        sync.cancel();
        join_all();
        return false; // F3: spawn failure — fail the entry fail-closed
    }
    run_range(0); // the caller's thread decodes range 0
    join_all();
    if (!failure.message.empty()) {
        return false; // F2/F3: fail-closed, no partial output (D7 contract)
    }

    // ── Validate (D4): Σ range outputs == the scout's recorded output. ──────
    size_t sum = 0;
    for (size_t i = 0; i < NR; ++i) sum += written[i];
    if (sum != rec.total_output) {
    }

    // ── Filters in order (F4), then emit post-transform bytes. ──────────────
    // The sequential decoder emits plain bytes up to each region, the
    // transformed region, and finally the tail — byte-identical output, and
    // chunk boundaries may differ (pinned by the identity fuzz).
    size_t emitted = 0;
    auto emit_plain = [&](size_t up_to) -> bool {
        if (up_to > rec.total_output) up_to = rec.total_output;
        while (emitted < up_to) {
            size_t chunk = std::min<size_t>(up_to - emitted, 1024 * 1024);
            if (!flush_cb(buf.data() + emitted, chunk)) return false;
            emitted += chunk;
        }
        return true;
    };
    auto apply_region = [&](const FilterEntry& f) -> bool {
        size_t len = f.block_length;
        if (len == 0) return true;
        if (f.block_start + len > rec.total_output) return false; // F2 guard
        std::vector<core::byte> tmp(len);
        if (f.type == 0) {
            Filters50::apply_delta(buf.data() + f.block_start, tmp.data(), len, f.channels);
        } else if (f.type == 1) {
            std::memcpy(tmp.data(), buf.data() + f.block_start, len);
            Filters50::apply_e8(tmp.data(), len, f.file_offset, false);
        } else if (f.type == 2) {
            std::memcpy(tmp.data(), buf.data() + f.block_start, len);
            Filters50::apply_e8(tmp.data(), len, f.file_offset, true);
        } else if (f.type == 3) {
            std::memcpy(tmp.data(), buf.data() + f.block_start, len);
            Filters50::apply_arm(tmp.data(), len, f.file_offset);
        } else {
            std::memcpy(tmp.data(), buf.data() + f.block_start, len); // types 4-7: raw
        }
        std::memcpy(buf.data() + f.block_start, tmp.data(), len);
        return true;
    };

    for (const auto& f : rec.filters) {
        if (f.block_start > rec.total_output) return false;
        if (emitted < f.block_start && !emit_plain(f.block_start)) return false;
        // The sequential decoder clamps region emission to dest_size; the
        // buffer already ends at dest_size, so regions beyond it cannot have
        // been recorded (the scout validated against the same dest_size).
        if (!apply_region(f)) return false;
    }
    if (!emit_plain(rec.total_output)) return false;
    return true;
}

} // namespace openrar::compress
