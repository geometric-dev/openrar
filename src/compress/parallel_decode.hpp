#ifndef OPENRAR_COMPRESS_PARALLEL_DECODE_HPP
#define OPENRAR_COMPRESS_PARALLEL_DECODE_HPP

#include "decompressor50.hpp"

#include <cstddef>
#include <cstdint>

namespace openrar::compress {

// v1.37.0 two-phase parallel decode driver
// (docs/v1.37.0-two-phase-implementation-plan.md).
//
// Design: a header-only pre-scan builds the block/table timeline; span
// workers decode symbol spans into operation records (no window, no rep
// state); one applier consumes the spans strictly in order through the
// SAME apply engine the sequential decoder drives, so the LZ chain lives
// in exactly one place and the emission is byte- and chunk-identical.
// Workers publish finished spans through a bounded window (a worker that
// finishes more than MAX_IN_FLIGHT ahead of the applier parks before
// publishing — review R4); resident record memory is bounded by the
// worker count times the per-span record cap. The sequential decoder
// remains the default and the fallback: every gate failure, pre-scan
// anomaly, or pre-emission failure falls back to a full sequential
// decode whose verdict IS the answer (fail-safe-identical).

struct ParallelDecodeDiag {
    bool engaged{false};
    bool fell_back{false}; // a parallel-stage anomaly ran the sequential path
    unsigned spans{0};
    unsigned workers{0};
    size_t prescan_blocks{0};
};

// The single decision function (review R6): both the CLI extraction and
// the CLI verify paths consult it. WASM/ILP32 builds answer false
// unconditionally (G6). Worker count: requested_threads 0 = the D9 default
// of 2, 1 = sequential (F7), N = clamp(N, 2, 8); the
// OPENRAR_PARALLEL_DECODE_THREADS var overrides ONLY the unset case (the
// sandboxed-worker channel) and OPENRAR_NO_PARALLEL_DECODE is the kill
// switch (D9, the v1.25 --no-mmap precedent). out_workers receives the
// worker count when the function answers true.
bool should_use_parallel_decode(size_t pack_size, size_t dest_size, size_t win_size,
                                bool solid_entry, unsigned requested_threads,
                                unsigned* out_workers);

// Decodes one non-solid member. Gates, pre-scans, splits spans at block
// boundaries, runs the pipeline, and applies filters + emission through
// the applier. `flush_cb` receives post-transform bytes exactly like
// Decompressor50::decompress's callback — byte- AND chunk-identical to
// the sequential decoder's emission (pinned by the identity tests).
// `requested_threads` is the caller's -mt (0 = unset → the D9 default,
// 1 = sequential, N = N workers): decode_entry re-gates internally, so
// the count must be carried here or every caller silently gets the
// default (the v1.37.1 extraction drift).
// Failure semantics (R5): any failure after bytes were emitted fails the
// entry with the sequential taxonomy; a failure before any emission runs
// the sequential path and returns its verdict (F1/F2/F3 — the diag's
// fell_back flag makes fallback observable to tests).
bool decode_entry(const core::byte* src, size_t src_size, size_t dest_size, size_t win_size,
                  Decompressor50::OutputCallback flush_cb, ParallelDecodeDiag* diag = nullptr,
                  unsigned requested_threads = 0);

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_PARALLEL_DECODE_HPP
