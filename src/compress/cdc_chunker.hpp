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
    //
    // CDC_-prefixed deliberately: compressor50.hpp also defines MIN_CHUNK
    // (1 MiB) and MAX_CHUNK (4 MiB) as *unqualified* names, and reuses that
    // same pair for two different things in one file -- a chunk-size clamp
    // (:663-667) and a per-thread-count clamp (:672).
    // Note (S-15 correction): A fixed restart halo of CDC_MAX_CHUNK (256 KiB)
    // is insufficient for cut-set identity on constant/periodic runs because
    // cut phase is unbounded. Parallel CDC chunking uses the exact
    // candidate-then-stitch mechanism (§8.3) with a 63-byte window overlap.
    static constexpr core::uint32 CDC_MIN_CHUNK = 16u * 1024u;
    static constexpr core::uint32 CDC_MAX_CHUNK = 256u * 1024u;
    static constexpr core::uint32 CDC_AVG_CHUNK = 64u * 1024u;

    CdcChunker();

    // Chunks `data` into the out vector. Deterministic: the same bytes
    // always produce the same chunk list (fixed Gear table, no seeds).
    // Delegate of CdcStreamChunker — the cut rules live there only.
    void chunk(const core::byte* data, size_t size, std::vector<CdcChunk>& out) const;

    // Parallel content-defined chunking (v1.50 §7.8, §8.3):
    // Phase 1: parallel candidate scanning with 63-byte window overlap.
    // Phase 2: serial candidate-then-stitch replay of cut rules.
    // Guaranteed 100% byte-for-byte, offset-for-offset, hash-for-hash identical
    // to sequential chunk() (Invariant G-3, cdc_parallel_cutset_equals_sequential).
    void chunk_parallel(const core::byte* data, size_t size, std::vector<CdcChunk>& out,
                        unsigned threads = 0, struct CdcParallelDiag* diag = nullptr) const;

    // 64-bit Gear hash of an arbitrary byte range (table-driven, no seed).
    core::uint64 hash_range(const core::byte* data, size_t n) const;

private:
    friend class CdcStreamChunker;
    core::uint64 table_[256] = {};
    core::uint64 mask_strict_ = 0xFFFF800000000000ULL;
    core::uint64 mask_light_ = 0xFFFE000000000000ULL;
};

struct CdcParallelDiag {
    bool fell_back{false};
    std::string fallback_reason;
    size_t candidate_intervals{0};
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
    core::uint64 h_ = 0;      // Gear hash of the pending bytes
    size_t pending_size_ = 0; // bytes since the last cut (<= CDC_MAX_CHUNK)
    core::uint64 fed_ = 0;    // total bytes fed (chunk offsets are stream-relative)
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_CDC_CHUNKER_HPP
