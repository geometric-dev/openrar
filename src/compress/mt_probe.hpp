#ifndef OPENRAR_COMPRESS_MT_PROBE_HPP
#define OPENRAR_COMPRESS_MT_PROBE_HPP

#include "../core/types.hpp"
#include "../io/file_stream.hpp"

namespace openrar::compress {

// Whether chunking at `chunk_size` would cost this input real ratio.
//
// MT compresses fixed-size chunks independently, so any redundancy that
// SPANS a chunk boundary is lost, and the loss is unbounded: measured on a
// 4 MiB region repeated N times, MT's archive grew by almost exactly N
// relative to sequential (8x = +698%), while MT also got ~3x SLOWER
// (sequential matches the repeats nearly free; MT re-compresses every copy).
// Chunk size does not bound this — it only changes how many boundaries there
// are. On input with no cross-chunk redundancy the cost is nil: measured
// -0.1% at ~2.2x speedup.
//
// So the fix is to DETECT the case and decline MT, not to try to reclaim it.
// Declining is a pure optimization decision: the emitted bytes are then
// produced by the sequential encoder.
//
// Method: for a couple of 2*chunk_size probes, compress the region as one
// block and as its two halves separately. If the joined form is materially
// smaller, redundancy crosses the boundary. This is a direct measurement, so
// it does not care how the repeat is aligned — an earlier fingerprint-based
// probe could only see repeats on a 64 KiB grid and silently missed a
// 708,460-byte period (a tarball holding several copies of one file), which
// is the COMMON shape. Cost is ~3 regions of 2*chunk_size per probe point.
//
// Heuristic in the other direction: an UNREADABLE probe reports "no
// redundancy" rather than "duplicated", so we never decline MT on a guess.
bool mt_chunking_costs_ratio(const core::byte* data, size_t size, size_t chunk_size);

// File-backed twin of the probe above.
bool mt_chunking_costs_ratio(io::FileStream& src, core::uint64 file_size, size_t chunk_size);

// How far below "the two halves compress as well as the whole" counts as
// duplication. 0.90 leaves headroom so ordinary variance cannot trip it.
extern const double kMtProbeDeclineRatio;

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_MT_PROBE_HPP
