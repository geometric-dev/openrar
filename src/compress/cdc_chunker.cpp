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

constexpr core::uint32 NORMALIZATION_BITS = [] {
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

    // Cutting mask: NORMALIZATION_BITS ones at the top of the 64-bit hash.
    mask_ = 0;
    for (core::uint32 b = 0; b < NORMALIZATION_BITS; ++b) mask_ |= (1ull << 63) >> b;
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
    size_t i = 0;
    const core::uint64 mask_strict = owner_.mask_;
    const core::uint64 mask_light = owner_.mask_ >> 1;

    while (i < n) {
        if (pending_size_ < CdcChunker::CDC_MIN_CHUNK - 1) {
            size_t skip =
                std::min(n - i, static_cast<size_t>(CdcChunker::CDC_MIN_CHUNK - 1) - pending_size_);
            for (size_t k = 0; k < skip; ++k) {
                h_ = (h_ << 1) + owner_.table_[static_cast<unsigned char>(data[i + k])];
            }
            i += skip;
            pending_size_ += skip;
            fed_ += skip;
            if (i == n) break;
        }

        if (pending_size_ < CdcChunker::CDC_AVG_CHUNK - 1) {
            size_t end = std::min(
                n, i + (static_cast<size_t>(CdcChunker::CDC_AVG_CHUNK - 1) - pending_size_));
            while (i < end) {
                h_ = (h_ << 1) + owner_.table_[static_cast<unsigned char>(data[i++])];
                ++pending_size_;
                ++fed_;
                if ((h_ & mask_strict) == 0) {
                    emit(out);
                    break;
                }
            }
            if (pending_size_ == 0 || i == n) continue;
        }

        while (i < n) {
            h_ = (h_ << 1) + owner_.table_[static_cast<unsigned char>(data[i++])];
            ++pending_size_;
            ++fed_;
            if ((h_ & mask_light) == 0 || pending_size_ == CdcChunker::CDC_MAX_CHUNK) {
                emit(out);
                break;
            }
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
