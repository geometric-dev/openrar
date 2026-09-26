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
    // Delegate of CdcStreamChunker — the cut rules live there only.
    void chunk(const core::byte* data, size_t size, std::vector<CdcChunk>& out) const;

    // 64-bit Gear hash of an arbitrary byte range (table-driven, no seed).
    core::uint64 hash_range(const core::byte* data, size_t n) const;

private:
    friend class CdcStreamChunker;
    core::uint64 table_[256] = {};
    core::uint64 mask_ = 0;
};

// Stateful streaming front-end over the same cut rules as CdcChunker::chunk.
// Cut decisions are causal — the rolling hash and both normalization levels
// depend only on the bytes since the last cut — so feed()/finish() over any
// block partition yields the exact chunk sequence (offsets, lengths, hashes)
// of chunk() over the whole buffer. The v1.26 fingerprint pass uses this to
// index files of unbounded size in bounded memory; chunk() delegates here so
// the cut logic has exactly one copy (the CLOSED-P1 drift lesson).
class CdcStreamChunker {
public:
    explicit CdcStreamChunker(const CdcChunker& owner) : owner_(owner) {}

    // Feed the next block; completed chunks are appended to `out`.
    void feed(const core::byte* data, size_t n, std::vector<CdcChunk>& out);

    // Emit the pending tail chunk at EOF (if any bytes are pending).
    void finish(std::vector<CdcChunk>& out);

private:
    void emit(std::vector<CdcChunk>& out);

    const CdcChunker& owner_;
    core::uint64 h_ = 0; // Gear hash of the pending bytes
    core::byte pending_[CdcChunker::kMaxChunk] = {};
    size_t pending_size_ = 0; // bytes since the last cut (<= kMaxChunk)
    size_t fed_ = 0;          // total bytes fed (chunk offsets are stream-relative)
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_CDC_CHUNKER_HPP
