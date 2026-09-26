#include "cdc_chunker.hpp"

#include <array>

namespace openrar::compress {

namespace {

// Deterministic Gear-table generation: a fixed 64-bit constant per byte
// value, derived from a splitmix64 stream seeded with a fixed constant.
// No seeds, no randomness at runtime — the same bytes always produce the
// same chunk list (plan §1).
core::uint64 splitmix64(core::uint64& state) {
    state += 0x9E3779B97F4A7C15ull;
    core::uint64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr core::uint32 kNormalizationBits = [] {
    // target avg 64 KiB → cutting mask width ≈ log2(64 KiB) = 16 bits;
    // FastCDC normalization: two levels around the target.
    core::uint32 bits = 0;
    for (core::uint32 v = 64u * 1024u; v > 1u; v >>= 1) ++bits;
    return bits;
}();

} // namespace

CdcChunker::CdcChunker() {
    core::uint64 state = 0x243F6A8885A308D3ull; // fixed seed
    for (int i = 0; i < 256; ++i) table_[i] = splitmix64(state);

    // Cutting mask: kNormalizationBits ones at the top of the 64-bit hash.
    mask_ = 0;
    for (core::uint32 b = 0; b < kNormalizationBits; ++b) mask_ |= (1ull << 63) >> b;
}

core::uint64 CdcChunker::hash_range(const core::byte* data, size_t n) const {
    core::uint64 h = 0;
    for (size_t i = 0; i < n; ++i) {
        h = (h << 1) + table_[static_cast<unsigned char>(data[i])];
    }
    return h;
}

void CdcChunker::chunk(const core::byte* data, size_t size, std::vector<CdcChunk>& out) const {
    out.clear();
    if (size == 0) return;

    size_t off = 0;
    while (off < size) {
        const size_t remaining = size - off;
        const size_t max_here = remaining < kMaxChunk ? remaining : kMaxChunk;
        if (max_here <= kMinChunk) {
            // Tail chunk (or tiny remainder): one chunk to the end.
            CdcChunk c;
            c.offset = off;
            c.length = static_cast<core::uint32>(max_here);
            c.hash = hash_range(data + off, c.length);
            out.push_back(c);
            break;
        }

        // Gear rolling hash with FastCDC normalization: below the avg-size
        // point use the stricter mask (fewer cut points), above it the
        // lighter mask — the two-level scheme reduces chunk variance.
        core::uint64 h = 0;
        size_t cut = max_here;
        const size_t avg_point = off + kAvgChunk < off + max_here ? off + kAvgChunk : max_here;
        for (size_t i = 0; i < max_here; ++i) {
            h = (h << 1) + table_[static_cast<unsigned char>(data[off + i])];
            const size_t pos = i + 1;
            if (pos < kMinChunk) continue;
            const core::uint64 active_mask = (off + pos) < avg_point ? mask_ : (mask_ >> 1);
            if ((h & active_mask) == 0) {
                cut = pos;
                break;
            }
        }
        // No natural boundary within the max window: cut at the cap.
        if (cut == max_here && max_here == kMaxChunk) cut = kMaxChunk;
        // Degenerate guard: never emit below the min unless the input ended.
        if (cut < kMinChunk && max_here > kMinChunk) cut = kMinChunk;

        CdcChunk c;
        c.offset = off;
        c.length = static_cast<core::uint32>(cut);
        c.hash = hash_range(data + off, c.length);
        out.push_back(c);
        off += cut;
    }
}

} // namespace openrar::compress
