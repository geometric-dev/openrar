#ifndef OPENRAR_COMPRESS_PARALLEL_DECODE_HPP
#define OPENRAR_COMPRESS_PARALLEL_DECODE_HPP

#include "decompressor50.hpp"

#include <cstddef>
#include <cstdint>

namespace openrar::compress {

// v1.37.0 intra-entry parallel decode driver (docs/v1.37.0-implementation-plan.md).
//
// Design: a symbol-only scout pass records per-block resynchronization state
// (rep distances, tables, filter applications, output spans); contiguous
// output ranges then decode concurrently into one shared pre-transform buffer
// in absolute coordinates, cross-range match sources waiting on the producing
// range's frontier; filters apply in order post-join; the post-transform
// stream is emitted through the caller's callback exactly as the sequential
// decoder would. Sequential decode remains the default and the fallback.

// The single decision function (review D5): both the CLI and the DLL decode
// paths consult this; WASM/ILP32 builds answer false unconditionally (G6).
// Thread count: min(hardware_concurrency, 8) with the
// OPENRAR_PARALLEL_DECODE_THREADS override for measurement and the
// OPENRAR_NO_PARALLEL_DECODE kill switch (D9, the v1.25 --no-mmap precedent).
bool should_use_parallel_decode(size_t pack_size, size_t dest_size, bool solid_entry);

struct ParallelDecodeDiag {
    bool engaged{false};
    unsigned ranges{0};
    size_t scout_bytes{0}; // packed bytes walked by the scout
};

// Decodes one non-solid member, engaging the parallel path when the gates
// allow it and the sequential Decompressor50 otherwise. `win_size` is the
// entry's dictionary size; `flush_cb` receives post-transform bytes exactly
// like Decompressor50::decompress's callback (chunk boundaries differ; bytes
// and order are identical — pinned by the MT/ST identity fuzz).
bool decode_entry(const core::byte* src, size_t src_size, size_t dest_size, size_t win_size,
                  Decompressor50::OutputCallback flush_cb, ParallelDecodeDiag* diag = nullptr);

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_PARALLEL_DECODE_HPP
