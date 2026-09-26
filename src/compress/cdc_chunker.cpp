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
    CdcStreamChunker stream(*this);
    stream.feed(data, size, out);
    stream.finish(out);
}

void CdcStreamChunker::feed(const core::byte* data, size_t n, std::vector<CdcChunk>& out) {
    for (size_t i = 0; i < n; ++i) {
        h_ = (h_ << 1) + owner_.table_[static_cast<unsigned char>(data[i])];
        pending_[pending_size_++] = data[i];
        ++fed_;
        const size_t pos = pending_size_;
        if (pos < CdcChunker::kMinChunk) continue;
        // FastCDC two-level normalization: the strict mask below the avg
        // point, the lighter one above it. (At a buffer end within kAvgChunk
        // of the chunk start, chunk() applied the strict mask to every
        // scanned position and the light mask at most at the final byte,
        // where "cut" and "tail to EOF" coincide — the streaming decisions
        // are byte-identical without knowing where EOF is.)
        const core::uint64 active_mask =
            pos < CdcChunker::kAvgChunk ? owner_.mask_ : (owner_.mask_ >> 1);
        if ((h_ & active_mask) == 0) {
            emit(out); // natural boundary
        } else if (pos == CdcChunker::kMaxChunk) {
            emit(out); // no boundary within the max window: cut at the cap
        }
    }
}

void CdcStreamChunker::finish(std::vector<CdcChunk>& out) {
    if (pending_size_ > 0) emit(out); // tail chunk to the end of input
}

void CdcStreamChunker::emit(std::vector<CdcChunk>& out) {
    CdcChunk c;
    c.offset = fed_ - pending_size_; // offset within the whole fed stream
    c.length = static_cast<core::uint32>(pending_size_);
    c.hash = h_; // h_ is exactly hash_range() over the pending bytes
    out.push_back(c);
    h_ = 0;
    pending_size_ = 0;
}

} // namespace openrar::compress
