#include "cdc_chunker.hpp"

#include "../core/thread_pool.hpp"

#include <algorithm>
#include <array>
#include <thread>

namespace openrar::compress {

namespace {

enum MatchFlag : core::uint8 { MATCH_NONE = 0, MATCH_LIGHT = 1, MATCH_STRICT = 2, MATCH_BOTH = 3 };

struct CandidateInterval {
    size_t start_off = 0;
    size_t count = 0;
    core::uint8 flags = 0;
};

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

} // namespace

CdcChunker::CdcChunker() {
    core::uint64 state = 0x243F6A8885A308D3ull; // fixed seed
    for (int i = 0; i < 256; ++i) table_[i] = splitmix64(state);

    // FastCDC distinct-popcount masks (OPT-K02):
    // Strict (17 bits, 2^-17): applied between MIN (16 KiB) and AVG (64 KiB)
    mask_strict_ = 0xFFFF800000000000ULL;
    // Light (15 bits, 2^-15): applied between AVG (64 KiB) and MAX (256 KiB)
    mask_light_ = 0xFFFE000000000000ULL;
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
    const core::uint64 mask_strict = owner_.mask_strict_;
    const core::uint64 mask_light = owner_.mask_light_;

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

void CdcChunker::chunk_parallel(const core::byte* data, size_t size, std::vector<CdcChunk>& out,
                                unsigned threads, CdcParallelDiag* diag) const {
    out.clear();
    if (!data || size == 0) return;

    if (size < 2 * CDC_MAX_CHUNK || threads == 1) {
        chunk(data, size, out);
        return;
    }

    if (threads == 0) {
        threads = std::min<unsigned>(std::thread::hardware_concurrency(), 8u);
    }
    if (threads <= 1) {
        chunk(data, size, out);
        return;
    }

    size_t num_slices = std::min<size_t>(threads, size / (2 * CDC_MAX_CHUNK));
    if (num_slices <= 1) {
        chunk(data, size, out);
        return;
    }

    size_t slice_size = size / num_slices;
    std::vector<std::vector<CandidateInterval>> slice_candidates(num_slices);
    std::vector<bool> slice_overflow(num_slices, false);

    // Phase 1: Parallel scanning with 63-byte window overlap
    {
        core::ThreadPool pool(static_cast<unsigned>(num_slices));
        for (size_t s = 0; s < num_slices; ++s) {
            size_t s_start = s * slice_size;
            size_t s_end = (s + 1 == num_slices) ? size : (s + 1) * slice_size;
            size_t s_len = s_end - s_start;
            size_t max_intervals = std::max<size_t>(s_len / 1024, 64);

            pool.submit(
                [this, data, s, s_start, s_end, max_intervals, &slice_candidates, &slice_overflow] {
                    auto& intervals = slice_candidates[s];
                    core::uint64 h = 0;
                    size_t warmup_start = (s == 0 ? 0 : s_start - 63);
                    for (size_t i = warmup_start; i < s_start; ++i) {
                        h = (h << 1) + table_[static_cast<unsigned char>(data[i])];
                    }
                    const core::uint64 mask_strict = mask_strict_;
                    const core::uint64 mask_light = mask_light_;

                    for (size_t i = s_start; i < s_end; ++i) {
                        h = (h << 1) + table_[static_cast<unsigned char>(data[i])];
                        bool is_strict = ((h & mask_strict) == 0);
                        bool is_light = ((h & mask_light) == 0);
                        if (is_strict || is_light) {
                            core::uint8 flags =
                                (is_strict ? MATCH_STRICT : 0) | (is_light ? MATCH_LIGHT : 0);
                            if (!intervals.empty() &&
                                intervals.back().start_off + intervals.back().count == i &&
                                intervals.back().flags == flags) {
                                ++intervals.back().count;
                            } else {
                                if (intervals.size() >= max_intervals) {
                                    slice_overflow[s] = true;
                                    intervals.clear();
                                    break;
                                }
                                intervals.push_back({i, 1, flags});
                            }
                        }
                    }
                });
        }
    }

    bool any_overflow = false;
    for (bool ov : slice_overflow) {
        if (ov) {
            any_overflow = true;
            break;
        }
    }
    if (any_overflow) {
        if (diag) {
            diag->fell_back = true;
            diag->fallback_reason = "candidate list overflow (exceeded range_len/1024)";
        }
        chunk(data, size, out);
        return;
    }

    // Phase 2: Serial candidate-then-stitch replay
    std::vector<CandidateInterval> all_intervals;
    size_t total_intervals = 0;
    for (const auto& sc : slice_candidates) total_intervals += sc.size();
    all_intervals.reserve(total_intervals);
    for (auto& sc : slice_candidates) {
        all_intervals.insert(all_intervals.end(), sc.begin(), sc.end());
    }
    if (diag) {
        diag->candidate_intervals = all_intervals.size();
    }

    size_t last_cut = 0;
    size_t cand_idx = 0;
    const size_t n_cand = all_intervals.size();

    while (last_cut < size) {
        if (size - last_cut <= CDC_MIN_CHUNK) {
            break;
        }

        size_t strict_start = last_cut + CDC_MIN_CHUNK - 1;
        size_t avg_boundary = std::min(size, last_cut + CDC_AVG_CHUNK - 1);
        size_t max_boundary = std::min(size, last_cut + CDC_MAX_CHUNK);

        while (cand_idx < n_cand &&
               all_intervals[cand_idx].start_off + all_intervals[cand_idx].count <= strict_start) {
            ++cand_idx;
        }

        size_t cut_at = SIZE_MAX;
        size_t search_idx = cand_idx;

        while (search_idx < n_cand && all_intervals[search_idx].start_off < avg_boundary) {
            const auto& ci = all_intervals[search_idx];
            if (ci.flags & MATCH_STRICT) {
                size_t cand_off = std::max(ci.start_off, strict_start);
                if (cand_off < avg_boundary) {
                    cut_at = cand_off;
                    break;
                }
            }
            ++search_idx;
        }

        if (cut_at == SIZE_MAX) {
            while (search_idx < n_cand &&
                   all_intervals[search_idx].start_off + all_intervals[search_idx].count <=
                       avg_boundary) {
                ++search_idx;
            }
            while (search_idx < n_cand && all_intervals[search_idx].start_off < max_boundary) {
                const auto& ci = all_intervals[search_idx];
                if (ci.flags & MATCH_LIGHT) {
                    size_t cand_off = std::max(ci.start_off, avg_boundary);
                    if (cand_off < max_boundary) {
                        cut_at = cand_off;
                        break;
                    }
                }
                ++search_idx;
            }
        }

        if (cut_at != SIZE_MAX) {
            size_t chunk_len = (cut_at + 1) - last_cut;
            CdcChunk c;
            c.offset = last_cut;
            c.length = static_cast<core::uint32>(chunk_len);
            c.hash = hash_range(data + cut_at + 1 - 64, 64);
            out.push_back(c);
            last_cut = cut_at + 1;
        } else {
            if (max_boundary == size) {
                break;
            }
            size_t chunk_len = CDC_MAX_CHUNK;
            CdcChunk c;
            c.offset = last_cut;
            c.length = static_cast<core::uint32>(chunk_len);
            c.hash = hash_range(data + last_cut + CDC_MAX_CHUNK - 64, 64);
            out.push_back(c);
            last_cut += CDC_MAX_CHUNK;
        }
    }

    if (last_cut < size) {
        size_t tail_len = size - last_cut;
        CdcChunk c;
        c.offset = last_cut;
        c.length = static_cast<core::uint32>(tail_len);
        if (tail_len >= 64) {
            c.hash = hash_range(data + size - 64, 64);
        } else {
            c.hash = hash_range(data + last_cut, tail_len);
        }
        out.push_back(c);
    }
}

} // namespace openrar::compress
