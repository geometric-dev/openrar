#ifndef OPENRAR_COMPRESS_CDC_CHUNKER_HPP
#define OPENRAR_COMPRESS_CDC_CHUNKER_HPP

#include "../core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openrar::compress {

// ── Content-defined chunking (v1.26 plan §1, Gate-0-cleared Design A) ───────
//
// FastCDC-style Gear-hash chunker with normalized cut points. The chunker
// is a PACKER-SIDE heuristic only: its output drives file ordering for
// solid-chain packing — the emitted archive is ordinary RAR5 solid
// compression (Gate 0: no chunk-reference records exist in RAR5).
//
// Locality property (pinned by cdc_chunk_boundary_fuzz): a single byte flip
// shifts chunk boundaries by at most one chunk — rolling-hash cut points
// depend only on the bytes since the previous cut.

struct CdcChunk {
    core::uint64 offset = 0; // byte offset within the input
    core::uint32 length = 0; // chunk length (min..max clamped)
    core::uint64 hash = 0;   // 64-bit Gear hash of the chunk content
};

class CdcChunker {
public:
    // Fixed parameters (plan §1): TargetAvg 64 KiB, min 16 KiB, max 256 KiB.
    static constexpr core::uint32 kMinChunk = 16u * 1024u;
    static constexpr core::uint32 kMaxChunk = 256u * 1024u;
    static constexpr core::uint32 kAvgChunk = 64u * 1024u;

    CdcChunker();

    // Chunks `data` into the out vector. Deterministic: the same bytes
    // always produce the same chunk list (fixed Gear table, no seeds).
    void chunk(const core::byte* data, size_t size, std::vector<CdcChunk>& out) const;

    // 64-bit Gear hash of an arbitrary byte range (table-driven, no seed).
    core::uint64 hash_range(const core::byte* data, size_t n) const;

private:
    core::uint64 table_[256] = {};
    core::uint64 mask_ = 0;
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_CDC_CHUNKER_HPP
