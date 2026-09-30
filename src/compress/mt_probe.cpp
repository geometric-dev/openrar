#include "mt_probe.hpp"

#include "compressor50.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace openrar::compress {

// How much worse chunking may be, relative to compressing the same bytes as
// one block, before MT is declined. Amortised over several chunks, so the
// constant "one table emission instead of two" cost is small — measured 0.999
// on non-redundant input — while measured duplication runs 2x-8x.
const double kMtProbeDeclineRatio = 1.10;

namespace {

// Bytes of input the compression probe actually compresses, and the rule for
// when it is worth running at all.
//
// The probe costs ~2x its budget in compression (once whole, once chunked).
// At a 16 MiB budget that was ~100% overhead on a 25 MB file — the probe
// alone dominated and MT measured SLOWER than sequential on clean input. So
// cap the budget at 8 MiB and only run when that is a small fraction of the
// file: budget = min(8 MiB, size/8), which needs size >= 16 * chunk to fire
// (budget must hold at least two whole chunks). Worst case is ~25% overhead
// on the smallest file that probes; well under 1% on large ones. Files too
// small to probe keep MT, where the absolute downside is correspondingly
// small — the repeated-chunk signal below still covers them.
constexpr size_t kProbeBudgetCap = 8u * 1024 * 1024;

// Leading bytes of each chunk fingerprinted by the repeat test.
constexpr size_t kFingerprintWindow = 64 * 1024;

// Chunks fingerprinted. Bounded so the scan is O(1) in file size.
constexpr size_t kMaxFingerprintChunks = 512;

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

// Compress `len` bytes the way MT would: as independent `chunk`-sized pieces.
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

// Signal 2: does splitting a multi-chunk region cost real ratio? Both sides
// compress the SAME bytes with the SAME encoder, so the only thing measured
// is chunking itself.
bool region_costs_ratio(const core::byte* data, size_t len, size_t chunk) {
    if (chunk == 0 || len < 2 * chunk) return false;
    const size_t whole = packed_size_of(data, len, len);
    if (whole == 0) return false;
    const size_t chunked = packed_size_chunked(data, len, chunk);
    if (chunked == 0) return false;
    const double ratio = static_cast<double>(chunked) / static_cast<double>(whole);
    return ratio > kMtProbeDeclineRatio;
}

// Signal 1: are any two chunks byte-identical? If so, MT re-compresses the
// same data N times while sequential matches it for free. This catches a file
// built by repeating a region — which a head-only redundancy probe is BLIND
// to, because the first chunks are perfectly self-consistent and the copies
// only collide across the file.
bool chunks_repeat(const core::byte* data, size_t size, size_t chunk) {
    std::vector<core::uint64> hashes;
    hashes.reserve(kMaxFingerprintChunks);
    const size_t win = chunk < kFingerprintWindow ? chunk : kFingerprintWindow;
    std::vector<core::byte> window(win);
    for (size_t i = 0; i < kMaxFingerprintChunks; ++i) {
        const size_t off = i * chunk;
        if (off + win > size) break;
        std::memcpy(window.data(), data + off, win);
        core::uint64 h = 0xcbf29ce484222325ull; // FNV-1a 64
        for (size_t k = 0; k < win; ++k) {
            h ^= window[k];
            h *= 0x100000001b3ull;
        }
        for (core::uint64 seen : hashes) {
            if (seen == h) return true;
        }
        hashes.push_back(h);
    }
    return false;
}

} // namespace

bool mt_chunking_costs_ratio(const core::byte* data, size_t size, size_t chunk_size) {
    if (data == nullptr || chunk_size == 0) return false;
    if (chunks_repeat(data, size, chunk_size)) return true;
    const size_t len = probe_budget(size, chunk_size);
    if (len == 0) return false;
    return region_costs_ratio(data, len, chunk_size);
}

bool mt_chunking_costs_ratio(io::FileStream& src, core::uint64 file_size, size_t chunk_size) {
    if (chunk_size == 0) return false;
    if (static_cast<core::uint64>(chunk_size) > file_size) return false;

    // Fingerprint the chunk heads first: it needs only a small read per chunk
    // and catches the repeated-region case outright.
    const core::uint64 saved = src.tell();
    const size_t win = chunk_size < kFingerprintWindow ? chunk_size : kFingerprintWindow;
    std::vector<core::byte> window(win);
    std::vector<core::uint64> hashes;
    hashes.reserve(kMaxFingerprintChunks);
    bool repeat = false;
    for (size_t i = 0; i < kMaxFingerprintChunks && !repeat; ++i) {
        const core::uint64 off = static_cast<core::uint64>(i) * chunk_size;
        if (off + win > file_size) break;
        if (!src.seek(off, io::SeekOrigin::Begin) || src.read(window.data(), win) != win) break;
        core::uint64 h = 0xcbf29ce484222325ull;
        for (size_t k = 0; k < win; ++k) {
            h ^= window[k];
            h *= 0x100000001b3ull;
        }
        for (core::uint64 seen : hashes) {
            if (seen == h) {
                repeat = true;
                break;
            }
        }
        hashes.push_back(h);
    }
    if (repeat) {
        src.seek(saved, io::SeekOrigin::Begin);
        return true;
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
