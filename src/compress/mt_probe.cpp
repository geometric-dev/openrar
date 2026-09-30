#include "mt_probe.hpp"

#include "compressor50.hpp"

#include <cstring>
#include <vector>

namespace openrar::compress {

// How much worse chunking may be, relative to compressing the same bytes as
// one block, before MT is declined. Amortised over several chunks, so the
// constant "one table emission instead of two" cost is small — measured 0.999
// on non-redundant input — while measured duplication runs 2x-9x.
const double kMtProbeDeclineRatio = 1.10;

namespace {

// ── Signal 1: repeated region, via CONTENT-DEFINED sample points ───────────
//
// A fixed grid only detects a repeat whose period divides the stride. That
// killed two earlier designs in turn: chunk-aligned sampling missed the
// benchmark payload's 4.25 MiB blob against 4 MiB chunks (9x size loss), and
// no practical power-of-two grid catches a period with a large odd factor.
//
// Content-defined boundaries fix it. A gear rolling hash emits a sample where
// the CONTENT matches a mask, so a copied region yields sample points at the
// same offsets in both copies whatever the period is — the trick
// deduplication tools use. One sequential pass, no compression.
//
// Not complete (a copy whose surroundings differ at the boundary can hide),
// but alignment-independent, which is the property that matters.
constexpr size_t kWindow = 64 * 1024;
constexpr size_t kMaxSamples = 512;
// Sample roughly 1 window in 4. A stricter mask (1 in 4096, as a dedup
// default with ~8 KiB chunks) yields ~0 samples over a 64 KiB-window scan of
// an 18 MB file, which is exactly the file that has to be caught.
constexpr core::uint64 kSampleMask = 3;

const std::vector<core::uint64>& gear_table() {
    static thread_local std::vector<core::uint64> gear;
    if (gear.empty()) {
        gear.resize(256);
        // Deterministic splitmix64 fill: the probe must reach the same verdict
        // for the same bytes on every run and every platform.
        core::uint64 s = 0x243F6A8885A308D3ull;
        for (auto& g : gear) {
            s += 0x9E3779B97F4A7C15ull;
            core::uint64 z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            g = z ^ (z >> 31);
        }
    }
    return gear;
}

// `read_block(offset, dst, kWindow)` must fill dst or return false. Walks the
// input in kWindow steps, sampling where the gear hash says so.
template <typename ReadBlock> bool repeated_region(core::uint64 size, ReadBlock read_block) {
    const std::vector<core::uint64>& gear = gear_table();
    std::vector<core::byte> window(kWindow);
    std::vector<core::uint64> hashes;
    hashes.reserve(kMaxSamples);
    for (core::uint64 off = 0; off + kWindow <= size; off += kWindow) {
        if (!read_block(off, window.data(), kWindow)) return false;
        // Hash THIS window only. A running hash would depend on every byte
        // before it, so two copies of a region would take different sample
        // decisions and never line up — the copies must decide from their own
        // content alone. That is what makes the sampling content-defined
        // rather than merely content-filtered.
        core::uint64 h = 0;
        for (size_t k = 0; k < kWindow; ++k) {
            h = ((h << 1) + gear[window[k]]) & 0xFFFFFFFFFFFFull;
        }
        if ((h & kSampleMask) != 0) continue;
        core::uint64 fh = 0xcbf29ce484222325ull; // FNV-1a over the window
        for (size_t k = 0; k < kWindow; ++k) {
            fh ^= window[k];
            fh *= 0x100000001b3ull;
        }
        for (core::uint64 seen : hashes) {
            if (seen == fh) return true;
        }
        if (hashes.size() >= kMaxSamples) return false;
        hashes.push_back(fh);
    }
    return false;
}

// ── Signal 2: cross-boundary redundancy, measured by compression ──────────
//
// Compress a multi-chunk region whole and again as chunks. Both sides use
// the same encoder on the same bytes, so the only thing measured is chunking.
constexpr size_t kProbeBudgetCap = 8u * 1024 * 1024;

size_t probe_budget(size_t size, size_t chunk) {
    const size_t eighth = size / 8;
    size_t budget = eighth < kProbeBudgetCap ? eighth : kProbeBudgetCap;
    budget = (budget / chunk) * chunk; // whole chunks only
    if (budget < 2 * chunk) return 0;  // too small to probe
    return budget;
}

size_t packed_size_of(const core::byte* data, size_t len, size_t win) {
    std::vector<core::byte> out;
    if (!Compressor50::compress_buffer(data, len, out, 3, win)) return 0;
    return out.size();
}

size_t packed_size_chunked(const core::byte* data, size_t len, size_t chunk) {
    size_t total = 0;
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        const size_t s = packed_size_of(data + off, n, n);
        if (s == 0) return 0;
        total += s;
    }
    return total;
}

bool region_costs_ratio(const core::byte* data, size_t len, size_t chunk) {
    if (chunk == 0 || len < 2 * chunk) return false;
    const size_t whole = packed_size_of(data, len, len);
    if (whole == 0) return false;
    const size_t chunked = packed_size_chunked(data, len, chunk);
    if (chunked == 0) return false;
    return static_cast<double>(chunked) > static_cast<double>(whole) * kMtProbeDeclineRatio;
}

} // namespace

bool mt_chunking_costs_ratio(const core::byte* data, size_t size, size_t chunk_size) {
    if (data == nullptr || chunk_size == 0) return false;
    if (repeated_region(static_cast<core::uint64>(size),
                        [&](core::uint64 off, core::byte* dst, size_t len) {
                            if (off + len > size) return false;
                            std::memcpy(dst, data + off, len);
                            return true;
                        })) {
        return true;
    }
    const size_t len = probe_budget(size, chunk_size);
    if (len == 0) return false;
    return region_costs_ratio(data, len, chunk_size);
}

bool mt_chunking_costs_ratio(io::FileStream& src, core::uint64 file_size, size_t chunk_size) {
    if (chunk_size == 0) return false;
    if (static_cast<core::uint64>(chunk_size) > file_size) return false;

    const core::uint64 saved = src.tell();
    if (src.seek(0, io::SeekOrigin::Begin)) {
        const bool repeat =
            repeated_region(file_size, [&](core::uint64 off, core::byte* dst, size_t len) {
                return src.seek(off, io::SeekOrigin::Begin) && src.read(dst, len) == len;
            });
        if (repeat) {
            src.seek(saved, io::SeekOrigin::Begin);
            return true;
        }
    }

    const size_t len = probe_budget(static_cast<size_t>(file_size), chunk_size);
    if (len == 0) {
        src.seek(saved, io::SeekOrigin::Begin);
        return false;
    }
    std::vector<core::byte> region(len);
    const bool read_ok = src.seek(0, io::SeekOrigin::Begin) && src.read(region.data(), len) == len;
    src.seek(saved, io::SeekOrigin::Begin);
    if (!read_ok) return false; // cannot judge: never decline on a guess
    return region_costs_ratio(region.data(), len, chunk_size);
}

} // namespace openrar::compress
