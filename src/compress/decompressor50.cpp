#include "decompressor50.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstdint>

// WindowBuffer's reserve/commit path.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__) && !defined(__wasm__)
#include <sys/mman.h>
#endif

namespace openrar::compress {

// ------------------------------------------------------------------
// BitReader : bit_pos based, MSB first, zero padded beyond end
// ------------------------------------------------------------------
BitReader::BitReader(const core::byte* data, size_t size)
    : p_(data), end_(data), acc_(0), n_(0), consumed_(0), size_(data ? size : 0),
      fetched_(data ? size : 0) {
    if (data) end_ = data + size;
}

BitReader::BitReader(InputCallback cb, core::uint64 size)
    : cb_(cb), buf_(65536), p_(buf_.data()), end_(buf_.data()), acc_(0), n_(0), consumed_(0),
      size_(size), fetched_(0) {}

// Pull the next callback chunk into buf_. Only valid when p_ >= end_ (every
// staged byte already moved into acc_). Returns false at the true end of the
// stream or when the callback is starved.
bool BitReader::fetch_chunk() {
    if (!cb_ || fetched_ >= size_) return false;
    size_t to_read = static_cast<size_t>(std::min<core::uint64>(buf_.size(), size_ - fetched_));
    size_t n = cb_(buf_.data(), to_read);
    // InputCallback is a PUBLIC std::function API owned by the embedding
    // (DLL / wasm / host), so its return value cannot be trusted to fit the
    // buffer it was offered. Unclamped, an over-reporting callback parks end_
    // past the staging vector and every later memcpy reads out of bounds
    // (v1.41.1).
    n = std::min(n, to_read);
    p_ = buf_.data();
    end_ = p_ + n;
    fetched_ += n;
    return n != 0;
}

void BitReader::refill() {
    // Fill the accumulator to at least 57 bits, fetching successive input
    // chunks as needed, so a peek/consume is served short ONLY at the true
    // end of the stream (zero padding is EOF semantics).
    if (n_ > 56) return;
    if (end_ - p_ >= 8) {
        core::uint64 v = 0;
        std::memcpy(&v, p_, 8);
#ifndef OPENRAR_BIG_ENDIAN
#if defined(_MSC_VER)
        v = _byteswap_uint64(v);
#else
        v = __builtin_bswap64(v);
#endif
#endif
        unsigned int bytes = (64 - n_) >> 3;
        p_ += bytes;
        if (bytes == 8) {
            acc_ = v;
        } else {
            acc_ = (acc_ << (bytes * 8)) | (v >> (64 - bytes * 8));
        }
        n_ += bytes * 8;
        return;
    }
    for (;;) {
        if (p_ >= end_ && !fetch_chunk()) return; // true end of stream / starved
        unsigned int max_bytes = (64 - n_) / 8;
        unsigned int k = std::min<unsigned int>(max_bytes, static_cast<unsigned int>(end_ - p_));
        if (k == 0) return; // accumulator holds >= 57 bits
        core::uint64 v = 0;
        std::memcpy(&v, p_, k);
#ifndef OPENRAR_BIG_ENDIAN
#if defined(_MSC_VER)
        v = _byteswap_uint64(v);
#else
        v = __builtin_bswap64(v);
#endif
#endif
        v >>= (64 - 8 * k);
        if (k == 8) {
            acc_ = v; // k == 8 implies n_ == 0, so no unconsumed bits are lost
        } else {
            acc_ = (acc_ << (8 * k)) | v;
        }
        n_ += 8 * k;
        p_ += k;
        if (n_ >= 57) return;
    }
}

core::uint64 BitReader::bits_remaining() const {
    core::uint64 rem_bytes = static_cast<core::uint64>(end_ - p_) + (size_ - fetched_);
    return rem_bytes * 8ULL + n_;
}

core::uint32 BitReader::peek_bits(unsigned int count) {
    if (count == 0) return 0;
    if (count > 32) count = 32; // mirror get_bits64's clamp for external callers
    if (n_ < count) refill();
    unsigned int take = std::min(count, n_);
    // Early return when the accumulator already holds the whole request:
    // this both avoids the shift below and is the only correct form at
    // `count == 32`, where `val << (count - take)` would be a shift by the
    // type's own width (UB) whenever a starved callback left take == 0
    // (v1.41.1).
    if (take == count) return static_cast<core::uint32>(acc_ >> (n_ - count));
    if (take == 0) return 0; // starved accumulator: zero-pad without a shift-by-32 UB
    return static_cast<core::uint32>(acc_ >> (n_ - take)) << (count - take);
}

core::uint64 BitReader::peek_bits64(unsigned int count) {
    if (count == 0) return 0;
    if (count > 64) count = 64;
    if (n_ < count) refill();
    unsigned int take = std::min(count, n_);
    if (take == count) return acc_ >> (n_ - count);
    // H-05: refill() works in whole bytes and stops once n_ > 56, so a
    // 58..64-bit peek can come up 1..7 bits short with input still unread.
    // Those bits live in the next unread byte: splice them in without
    // consuming it. (n_ in 57..63 here means acc_ holds exactly n_ valid
    // low bits, so the shift below cannot lose data.)
    if (n_ >= 57) {
        if (p_ >= end_) fetch_chunk(); // all staged bytes are already in acc_
        if (p_ < end_) {
            const unsigned int extra = count - n_; // 1..7
            const core::uint64 next = *p_;
            return (acc_ << extra) | (next >> (8 - extra));
        }
    }
    if (take == 0) return 0; // starved accumulator: zero-pad without a shift-by-64 UB
    return (acc_ >> (n_ - take)) << (count - take);
}

core::uint32 BitReader::get_bits(unsigned int count) {
    if (count == 0) return 0;
    if (count > 32) count = 32;
    if (n_ < count) refill();
    if (n_ >= count) {
        n_ -= count;
        consumed_ += count;
        core::uint32 val = static_cast<core::uint32>(acc_ >> n_);
        acc_ &= (n_ ? (core::uint64(1) << n_) - 1 : 0);
        return val;
    }
    unsigned int take = n_;
    core::uint32 val = take ? (static_cast<core::uint32>(acc_) << (count - take)) : 0;
    consumed_ += take;
    n_ = 0;
    acc_ = 0;
    truncated_ = true; // H-07: zero-padded past the end of input
    return val;
}

core::uint64 BitReader::get_bits64(unsigned int count) {
    if (count == 0) return 0;
    if (count > 64) count = 64;
    if (n_ < count) refill();
    if (n_ >= count) {
        core::uint64 val =
            (count == 64) ? acc_ : ((acc_ >> (n_ - count)) & ((core::uint64(1) << count) - 1));
        consume_bits(count);
        return val;
    }
    if (count > 32) {
        unsigned int high_count = count - 32;
        core::uint64 high = get_bits64(high_count);
        core::uint64 low = get_bits64(32);
        return (high << 32) | low;
    }
    core::uint64 val = peek_bits64(count);
    consume_bits(count);
    return val;
}

void BitReader::consume_bits(unsigned int count) {
    if (count == 0) return;
    if (n_ < count) refill();
    unsigned int take = std::min(count, n_);
    if (take < count) truncated_ = true; // H-07: consumed padding, not input
    consumed_ += take;
    n_ -= take;
    acc_ &= (n_ ? (core::uint64(1) << n_) - 1 : 0);
}

void BitReader::align_byte() {
    unsigned int r = consumed_ & 7;
    if (r) consume_bits(8 - r);
}

// ------------------------------------------------------------------
// HuffmanDecoder
// ------------------------------------------------------------------
bool HuffmanDecoder::build(const core::byte* bit_lengths, size_t count) {
    if (count > MAX_SYMBOLS) return false;
    for (size_t i = 0; i < count; ++i)
        if (bit_lengths[i] > MAX_CODE_BITS) return false;

    std::memset(quick_table_, 0, sizeof(quick_table_));
    std::memset(decode_len_, 0, sizeof(decode_len_));
    std::memset(decode_pos_, 0, sizeof(decode_pos_));
    std::memset(decode_num_, 0, sizeof(decode_num_));
    max_num_ = count;
    quick_bits_ = QUICK_BITS;

    // Count
    core::uint32 len_cnt[16] = {0};
    for (size_t i = 0; i < count; ++i) len_cnt[bit_lengths[i] & 0xF]++;
    len_cnt[0] = 0;

    // Build decode_len / decode_pos
    decode_pos_[0] = 0;
    decode_len_[0] = 0;
    core::uint32 upper = 0;
    for (unsigned int i = 1; i < 16; ++i) {
        upper += len_cnt[i];
        // Kraft inequality on the PARTIAL sum: upper is S_i * 2^i where
        // S_i = sum_{j<=i} len_cnt[j]/2^j. For any prefix code S_i <= 1, so
        // upper <= (1u << i) always — including the sparsely-populated tables
        // RAR permits (which only make S_i smaller) and all-zero tables.
        // Rejecting the over-complete case here keeps decode_len_ monotone and
        // turns a table that would otherwise decode to arbitrary garbage into
        // a clean failure (v1.41.1).
        if (upper > (1u << i)) return false;
        decode_len_[i] = upper << (16 - i);
        upper <<= 1;
        decode_pos_[i] = decode_pos_[i - 1] + len_cnt[i - 1];
    }

    // Build decode_num
    core::uint32 copy_pos[16];
    std::memcpy(copy_pos, decode_pos_, sizeof(copy_pos));
    for (size_t i = 0; i < count; ++i) {
        core::uint8 l = bit_lengths[i] & 0xF;
        if (l != 0) {
            decode_num_[copy_pos[l]++] = static_cast<core::uint16>(i);
        }
    }

    // Kraft check: upper should be 1<<16 for complete code, but allow incomplete for RAR (sparse)
    // No failure.

    // Build quick table (single pass)
    // For each code 0..QUICK_SIZE-1 left aligned
    unsigned int cur_len = 1;
    for (size_t code = 0; code < QUICK_SIZE; ++code) {
        core::uint32 bitfield = static_cast<core::uint32>(code) << (16 - QUICK_BITS);
        while (cur_len < 16 && bitfield >= decode_len_[cur_len]) ++cur_len;
        if (cur_len > QUICK_BITS) {
            quick_table_[code] = 0; // Forces slow path
        } else {
            unsigned int len_cap = cur_len >= 16 ? 15 : cur_len;
            core::uint32 symbol = 0;
            core::uint32 dist = bitfield - decode_len_[len_cap - 1];
            dist >>= (16 - len_cap);
            core::uint32 pos = decode_pos_[len_cap] + dist;
            if (pos < max_num_) symbol = decode_num_[pos];
            quick_table_[code] = (static_cast<core::uint32>(len_cap) << 16) | symbol;
        }
    }

    return true;
}

core::uint32 HuffmanDecoder::decode(BitReader& reader) const {
    core::uint32 peek = reader.peek_bits(QUICK_BITS);
    core::uint32 e = quick_table_[peek];
    core::uint32 len = e >> 16;
    if (len != 0) {
        reader.consume_bits(len);
        return e & 0xFFFF;
    }
    // Slow path: left aligned 15 bits
    core::uint32 bitfield = reader.peek_bits(15) << 1; // left align 15 in 16
    bitfield &= 0xFFFE;
    unsigned int bits = 15;
    for (unsigned int i = QUICK_BITS + 1; i < 15; ++i) {
        if (bitfield < decode_len_[i]) {
            bits = i;
            break;
        }
    }
    reader.consume_bits(bits);
    core::uint32 dist = bitfield - decode_len_[bits - 1];
    dist >>= (16 - bits);
    core::uint32 pos = decode_pos_[bits] + dist;
    if (pos >= max_num_) pos = 0;
    return decode_num_[pos];
}

// ------------------------------------------------------------------
// Decompressor50
// ------------------------------------------------------------------
Decompressor50::Decompressor50(size_t win_size, bool extra_dist, size_t alloc_win) {
    declared_win_ = win_size ? win_size : DEFAULT_WIN_SIZE;
    if (alloc_win == 0) alloc_win = declared_win_;
    engine_.init(alloc_win ? alloc_win : DEFAULT_WIN_SIZE);
    last_error_ = DecompressErrorCode::Ok;
    last_error_str_.clear();

    // Two failure modes for the sliding dictionary:
    //   (a) declared size exceeds this build's ALLOC_LIMIT (64 GiB on 64-bit,
    //       1 GiB on 32-bit/WASM). Checked on declared_win_ so hostile/corrupt
    //       declarations cannot slip past by clamping alloc_win.
    //   (b) the OS refuses memory while the window grows with the decoded
    //       output (ApplyEngine::grow -> AllocationFailed). The declared size
    //       is a ceiling, not an up-front commitment.
    if (declared_win_ > ALLOC_LIMIT) {
        last_error_ = DecompressErrorCode::DictionaryTooLarge;
        last_error_str_ = "dictionary too large";
    }
    std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
    use_extra_dist_ = extra_dist;
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
}

void Decompressor50::set_extra_dist(bool extra_dist) {
    if (extra_dist == use_extra_dist_) return;
    use_extra_dist_ = extra_dist;
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
    // Code lengths of the other flavor cannot be reused: force the member
    // to transmit its own tables.
    tables_ready_ = false;
    std::fill(std::begin(table_), std::end(table_), static_cast<core::byte>(0));
}

// ── ApplyEngine (v1.37.0 plan M0, review R1): the single apply path ─────────
//
// Driven by decompress_internal's token loop below AND by the two-phase
// record applier (parallel_decode driver). Extracted verbatim from the
// sequential decoder; the refactor is byte-identity-neutral and frozen by
// the full gate.

void Decompressor50::ApplyEngine::init(size_t win_size) {
    win_size_ = std::max<size_t>(win_size, 0x10000);
    win_mask_ = win_size_ - 1;
    win_pow2_ = (win_size_ & (win_size_ - 1)) == 0;
}

// ── WindowBuffer ───────────────────────────────────────────────────────
namespace {
#if SIZE_MAX > 0xFFFFFFFFu && !defined(__EMSCRIPTEN__) && !defined(__wasm__)
constexpr bool WINDOW_VM = true;
#else
constexpr bool WINDOW_VM = false;
#endif
// Below this, a vector (and its occasional copy) is cheaper than a mapping.
constexpr size_t WINDOW_VM_THRESHOLD = 64u << 20;
constexpr size_t WINDOW_COMMIT_GRANULE = 1u << 20;
} // namespace

bool WindowBuffer::init(size_t max_bytes) {
    release();
    if (WINDOW_VM && max_bytes > WINDOW_VM_THRESHOLD) {
#if defined(_WIN32)
        void* p = VirtualAlloc(nullptr, max_bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (p == nullptr) return false;
#elif SIZE_MAX > 0xFFFFFFFFu && !defined(__EMSCRIPTEN__) && !defined(__wasm__)
        void* p =
            mmap(nullptr, max_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return false;
#else
        void* p = nullptr;
        return false;
#endif
        base_ = static_cast<core::byte*>(p);
        reserved_ = max_bytes;
        vm_ = true;
    }
    return true;
}

bool WindowBuffer::grow_to(size_t n) {
    if (n <= size_) return true;
    if (!vm_) {
        try {
            vec_.resize(n);
        } catch (...) {
            return false; // strong guarantee: vec_ unchanged
        }
        base_ = vec_.data();
        size_ = vec_.size();
        return true;
    }
    if (n > reserved_) return false;
    if (n > committed_) {
        // Commit whole granules; committed pages read as zero.
        size_t target =
            (n + WINDOW_COMMIT_GRANULE - 1) / WINDOW_COMMIT_GRANULE * WINDOW_COMMIT_GRANULE;
        if (target > reserved_) target = reserved_;
        const size_t delta = target - committed_;
#if defined(_WIN32)
        if (VirtualAlloc(base_ + committed_, delta, MEM_COMMIT, PAGE_READWRITE) == nullptr)
            return false;
#elif SIZE_MAX > 0xFFFFFFFFu && !defined(__EMSCRIPTEN__) && !defined(__wasm__)
        if (mprotect(base_ + committed_, delta, PROT_READ | PROT_WRITE) != 0) return false;
#else
        (void)delta;
        return false;
#endif
        committed_ = target;
    }
    size_ = n;
    return true;
}

void WindowBuffer::release() {
    if (vm_ && base_ != nullptr) {
#if defined(_WIN32)
        VirtualFree(base_, 0, MEM_RELEASE);
#elif SIZE_MAX > 0xFFFFFFFFu && !defined(__EMSCRIPTEN__) && !defined(__wasm__)
        munmap(base_, reserved_);
#endif
    }
    vec_.clear();
    vec_.shrink_to_fit();
    base_ = nullptr;
    size_ = reserved_ = committed_ = 0;
    vm_ = false;
}

bool Decompressor50::ApplyEngine::ensure_window_alloc() {
    if (window_ready_ || win_size_ == 0) return true;
    if (!window_.init(win_size_) || !window_.grow_to(std::min(win_size_, INITIAL_WINDOW))) {
        window_.release();
        cap_ = 0;
        return false;
    }
    cap_ = window_.size();
    window_ready_ = true;
    return true;
}

bool Decompressor50::ApplyEngine::grow(size_t n) {
    // Only reachable while cap_ < win_size_, i.e. before the first wrap:
    // the data is the linear prefix [0, win_pos_), and both storage kinds
    // keep it in place. Reaching win_size_ switches to the full ring for
    // good.
    if (cap_ == win_size_) return true;
    const size_t need =
        (n >= win_size_ - win_pos_) ? win_size_ : win_pos_ + n; // clamp at the ring end
    if (need <= cap_) return true;
    // Geometric (x2) growth amortises the vector's copies; the VM path grows
    // by the same steps, which also bounds the commit-call count.
    size_t next = cap_ > win_size_ / 2 ? win_size_ : std::max(cap_ * 2, need);
    if (next > win_size_) next = win_size_;
    if (!window_.grow_to(next)) return false;
    cap_ = window_.size();
    return true;
}

void Decompressor50::ApplyEngine::reset_window() {
    win_pos_ = 0;
    first_win_done_ = false;
}

bool Decompressor50::ApplyEngine::copy_match(size_t distance, size_t length,
                                             [[maybe_unused]] size_t total_written) {
#ifdef OPENRAR_CROSS_VALIDATE
    if (val_src_ != nullptr) {
        // Filter-awareness: the window holds the TRANSFORMED stream inside
        // decoded filter regions, so a match whose dictionary range
        // intersects a region legitimately differs from the raw source.
        bool range_transformed = false;
        const size_t win_lo = total_written >= distance ? total_written - distance : 0;
        const size_t win_hi = win_lo + length; // exclusive
        for (const auto& region : val_regions_) {
            if (region.first >= win_hi) break; // regions are ascending
            if (region.first + region.second > win_lo) {
                range_transformed = true;
                break;
            }
        }
        // Suppresses only the VALIDATION below - the copy itself always runs
        // (an early return here would corrupt the decoded output).
        if (!range_transformed && total_written >= distance &&
            total_written + length <= val_size_) {
            for (size_t i = 0; i < length; ++i) {
                if (val_src_[total_written - distance + i] != val_src_[total_written + i]) {
                    // Match failed cross-validation: the byte we are about to copy
                    // from the dictionary does not equal the byte in the original source stream.
                    std::fprintf(
                        stderr,
                        "Cross-validation failed at pos %zu (dist %zu, len %zu, offset %zu)\n",
                        total_written, distance, length, i);
                    std::fflush(stderr);
                    std::abort();
                }
            }
        }
    }
#endif

    if (distance == 0 || distance > win_size_) return true; // callers validate first
    // Lazy window: every path below writes [win_pos_, win_pos_ + length)
    // (wrapping only once the full ring exists), so reserve that span.
    if (!reserve(length)) return false;
    size_t src = wrap_up(win_pos_ + win_size_ - distance);
    if (distance > win_pos_ && !first_win_done_) {
        // INTENDED (RAR5 spec): references past written data on the first
        // window pass decode as zeros. Do not "fix" this into an error.
        size_t rem_zero = length;
        while (rem_zero > 0) {
            const size_t chunk = std::min(rem_zero, win_size_ - win_pos_);
            std::memset(&window_[win_pos_], 0, chunk);
            win_pos_ = wrap_up(win_pos_ + chunk);
            if (win_pos_ == 0) first_win_done_ = true;
            rem_zero -= chunk;
        }
        return true;
    }

    if (src == win_pos_) {
        // distance == win_size_: the circular reference is this very slot, so
        // a byte-exact copy rewrites each position with its own old content.
        // Advancing without writing is the same result and cannot be expressed
        // as memcpy (dst == src) or a chunked loop with gap 0.
        win_pos_ = wrap_up(win_pos_ + length);
        if (win_pos_ == 0) first_win_done_ = true;
        return true;
    }

    if (distance == 1 && win_pos_ + length <= win_size_) {
        std::memset(&window_[win_pos_], window_[src], length);
        win_pos_ = wrap_up(win_pos_ + length);
        if (win_pos_ == 0) first_win_done_ = true;
        return true;
    }

    // Linear (non-circular) distance between source and destination regions in
    // the window buffer. memcpy is only valid when the regions do not overlap,
    // i.e. when this gap is >= the copy length. Note the circular `distance`
    // is NOT the right guard: after a wrap (win_pos_ < distance) the source
    // sits AHEAD of the destination and the true gap is win_size_ - distance.
    size_t gap = (src < win_pos_) ? (win_pos_ - src) : (src - win_pos_);

    if (src + length <= win_size_ && win_pos_ + length <= win_size_ && gap >= length) {
        std::memcpy(&window_[win_pos_], &window_[src], length);
        win_pos_ = wrap_up(win_pos_ + length);
        if (win_pos_ == 0) first_win_done_ = true;
        return true;
    }

    while (length > 0) {
        size_t chunk = length;
        if (chunk > win_size_ - win_pos_) chunk = win_size_ - win_pos_;
        if (chunk > win_size_ - src) chunk = win_size_ - src;
        size_t gap_now = (src < win_pos_) ? (win_pos_ - src) : (src - win_pos_);
        if (chunk > gap_now) chunk = gap_now;

        std::memcpy(&window_[win_pos_], &window_[src], chunk);

        if (chunk < length && chunk >= distance) {
            size_t done = chunk;
            size_t remaining = length - done;
            while (remaining > 0) {
                size_t step = std::min(done, remaining);
                // Clamp against the window end (wrap handled by the outer loop)
                if (step > win_size_ - (win_pos_ + done)) step = win_size_ - (win_pos_ + done);
                if (step == 0) break; // window edge; outer loop continues
                std::memmove(&window_[win_pos_ + done], &window_[win_pos_], step);
                done += step;
                remaining -= step;
            }
            chunk = done;
        }
        win_pos_ = wrap_up(win_pos_ + chunk);
        src = wrap_up(src + chunk);
        length -= chunk;
        if (win_pos_ == 0) first_win_done_ = true;
    }
    return true;
}

bool Decompressor50::ApplyEngine::flush_plain_up_to(size_t target, size_t total_written,
                                                    size_t dest_size, OutputCallback cb) {
    if (last_flushed_ > total_written) return false;
    if (total_written - last_flushed_ > win_size_) return false;
    if (target > dest_size) target = dest_size;
    if (target > total_written) target = total_written;
    while (last_flushed_ < target) {
        size_t chunk = target - last_flushed_;
        size_t back = (total_written - last_flushed_) % win_size_;
        size_t circ_start = (win_pos_ + win_size_ - back) % win_size_;
        size_t max_contig = win_size_ - circ_start;
        if (chunk > max_contig) chunk = max_contig;

        if (chunk == 0) {
            return false;
        }
        if (cb && !cb(&window_[circ_start], chunk)) {
            return false;
        }
        last_flushed_ += chunk;
    }
    return true;
}

bool Decompressor50::ApplyEngine::flush_pending(bool flush_all, core::uint64 base_at_entry,
                                                size_t total_written, size_t dest_size,
                                                OutputCallback cb) {
    auto abs_pos = [&]() -> core::uint64 {
        return base_at_entry + static_cast<core::uint64>(total_written);
    };
    auto local_region_start = [&](const FilterEntry& f) -> size_t {
        return f.block_start > base_at_entry ? static_cast<size_t>(f.block_start - base_at_entry)
                                             : 0;
    };
    while (filters_head_ < filters_.size()) {
        const auto& f = filters_[filters_head_];
        const bool region_complete =
            f.block_start <= abs_pos() &&
            static_cast<core::uint64>(f.block_length) <= abs_pos() - f.block_start;
        if (region_complete) {
            // 1. Flush any plain data preceding this filter:
            size_t region_local = local_region_start(f);
            if (last_flushed_ < region_local) {
                if (!flush_plain_up_to(region_local, total_written, dest_size, cb)) return false;
            }

            // 2. Extract filter region from circular window WITHOUT modifying window_:
            size_t len = f.block_length;
            const core::uint64 prior64 =
                f.block_start < base_at_entry ? base_at_entry - f.block_start : 0;
            if (prior64 > f.block_length) return false;
            const size_t prior = static_cast<size_t>(prior64);
            if (len > 0) {
                if (abs_pos() - f.block_start >= win_size_) {
                    return false;
                }
                size_t back = static_cast<core::uint64>(abs_pos() - f.block_start) % win_size_;
                size_t circ_start = (win_pos_ + win_size_ - back) % win_size_;
                const size_t head = std::min(len, win_size_ - circ_start);

                const core::byte* emit_ptr = nullptr;
                if (f.type == 0) {
                    filter_scratch_.resize(len);
                    if (head == len) {
                        Filters50::apply_delta(&window_[circ_start], filter_scratch_.data(), len,
                                               f.channels);
                        emit_ptr = filter_scratch_.data();
                    } else {
                        std::memcpy(filter_scratch_.data(), &window_[circ_start], head);
                        std::memcpy(filter_scratch_.data() + head, &window_[0], len - head);
                        delta_scratch_.resize(len);
                        Filters50::apply_delta(filter_scratch_.data(), delta_scratch_.data(), len,
                                               f.channels);
                        emit_ptr = delta_scratch_.data();
                    }
                } else {
                    filter_scratch_.resize(len);
                    std::memcpy(filter_scratch_.data(), &window_[circ_start], head);
                    if (head < len) {
                        std::memcpy(filter_scratch_.data() + head, &window_[0], len - head);
                    }
                    if (f.type == 1) {
                        Filters50::apply_e8(filter_scratch_.data(), len, f.file_offset, false);
                    } else if (f.type == 2) {
                        Filters50::apply_e8(filter_scratch_.data(), len, f.file_offset, true);
                    } else if (f.type == 3) {
                        Filters50::apply_arm(filter_scratch_.data(), len, f.file_offset);
                    }
                    // Undefined filter types 4-7: emit the raw region (spec).
                    emit_ptr = filter_scratch_.data();
                }

                // 3. Emit filtered bytes directly to flush_cb (never touching window_!)
                if (dest_size != SIZE_MAX) {
                    if (prior != 0) return false;
                    if (last_flushed_ > dest_size || len > dest_size - last_flushed_) return false;
                }
                if (cb && !cb(emit_ptr, len)) return false;
                last_flushed_ += len - prior;
            }
            ++filters_head_;
        } else {
            if (flush_all) {
                if (f.block_length != 0) return false;
                ++filters_head_;
                continue;
            } else {
                break;
            }
        }
    }

    // Honor RAM ceiling invariant (docs/invariants.md §2): cap retained scratch at 1 MiB.
    constexpr size_t MAX_RETAINED_FILTER_SCRATCH = 1024 * 1024;
    if (filter_scratch_.capacity() > MAX_RETAINED_FILTER_SCRATCH) {
        filter_scratch_.clear();
        filter_scratch_.shrink_to_fit();
    }
    if (delta_scratch_.capacity() > MAX_RETAINED_FILTER_SCRATCH) {
        delta_scratch_.clear();
        delta_scratch_.shrink_to_fit();
    }

    // v1.38.0 M2: compact the consumed prefix once per flush call (each
    // region used to pay an O(pending) erase shift). The push-side overlap
    // check compares against the last PUSHED region; compacting here keeps
    // filters_.empty()/.back() semantics identical to the erased queue.
    if (filters_head_) {
        filters_.erase(filters_.begin(), filters_.begin() + filters_head_);
        filters_head_ = 0;
    }

    // Flush any plain bytes up to the safe limit (before the next pending filter):
    size_t safe_limit = total_written;
    if (!flush_all) {
        for (const auto& f : filters_) {
            size_t region_local = local_region_start(f);
            if (region_local < safe_limit) {
                safe_limit = region_local;
            }
        }
    }

    return flush_plain_up_to(safe_limit, total_written, dest_size, cb);
}

namespace {
// C-02: shared by decompress_internal() and decode_span() so the two decoders
// cannot drift. The distance is accumulated in 64 bits regardless of the
// target's size_t: with the RAR7 ExtraDist table d_bits reaches 38, and even
// the 64-slot table produces > 4 GiB distances (slots 64/65) that would wrap
// into [1, win_size_] on ILP32/WASM32 and slip past the window check.
// Returns 0 on truncated input (0 is never a valid 1-based distance).
core::uint64 decode_match_distance(BitReader& reader, const HuffmanDecoder& ldd,
                                   core::uint32 dist_slot) {
    if (dist_slot < 4) return 1u + static_cast<core::uint64>(dist_slot);
    const core::uint32 d_bits = dist_slot / 2 - 1; // 1..38 (slot < 80)
    core::uint64 distance = 1u + (static_cast<core::uint64>(2u | (dist_slot & 1u)) << d_bits);
    if (d_bits >= 4) {
        if (d_bits > 4) {
            const core::uint32 n = d_bits - 4;
            if (reader.bits_remaining() < n) return 0;
            const core::uint64 extra = (d_bits > 36)
                                           ? reader.get_bits64(n)
                                           : static_cast<core::uint64>(reader.get_bits(n));
            distance += extra << 4;
        }
        if (reader.bits_remaining() < 1) return 0;
        distance += ldd.decode(reader);
    } else {
        if (reader.bits_remaining() < d_bits) return 0;
        distance += reader.get_bits(d_bits);
    }
    return distance;
}
} // namespace

core::uint32 Decompressor50::slot_to_length(BitReader& reader, core::uint32 slot) {
    core::uint32 length = 2;
    core::uint32 l_bits = 0;
    if (slot < 8) {
        length += slot;
    } else {
        l_bits = slot / 4 - 1;
        length += (4 | (slot & 3)) << l_bits;
    }
    if (l_bits > 0) {
        length += reader.get_bits(l_bits);
    }
    return length;
}

bool Decompressor50::read_block_header(BitReader& reader, BlockHeader& header) {
    header.header_size = 0;
    if (reader.bits_remaining() < 16) return false;
    reader.align_byte();
    if (reader.bits_remaining() < 8) return false;
    core::uint32 flags = reader.get_bits(8);
    core::uint32 byte_cnt = ((flags >> 3) & 3) + 1;
    if (byte_cnt == 4) return false;
    header.header_size = 2 + static_cast<int>(byte_cnt);
    header.block_bit_size = static_cast<int>((flags & 7) + 1);
    if (reader.bits_remaining() < 8) return false;
    core::uint32 saved = reader.get_bits(8);
    core::uint32 block_size = 0;
    for (core::uint32 i = 0; i < byte_cnt; ++i) {
        if (reader.bits_remaining() < 8) return false;
        block_size |= reader.get_bits(8) << (i * 8);
    }
    header.block_size = static_cast<int>(block_size);
    core::uint32 chk = (0x5A ^ flags ^ block_size ^ (block_size >> 8) ^ (block_size >> 16)) & 0xFF;
    if (chk != saved) return false;
    header.block_start = static_cast<core::int64>(reader.byte_pos()); // after header
    header.last_block_in_file = (flags & 0x40) != 0;
    header.table_present = (flags & 0x80) != 0;
    // block_size==0 is valid per spec (empty last block with LastBlock flag); caller must handle.
    return true;
}

bool Decompressor50::read_tables(BitReader& reader, BlockHeader& header) {
    if (!header.table_present) return true;
    if (header.block_size == 0) return true; // Tolerate empty blocks with table_present flag
    // NOTE: there used to be an up-front `bits_remaining() < 80` guard here,
    // rejecting any stream with fewer than 20*4 bits left. That assumed the
    // bit-length prefix always costs 20 raw nibbles, but it is RLE-coded: a
    // `15` nibble followed by a non-zero count encodes a run of zero lengths.
    // For highly repetitive payloads most of the 20 entries are zero, so a
    // perfectly valid table can occupy far fewer than 80 bits. The guard made
    // us reject valid compressed output for small compressible files
    // -- e.g. any 12-byte packed stream, which has only ~64 bits left after
    // the block header. Every read below is individually bounds-checked at
    // 1/3/4/7-bit granularity, so truncation is still caught; the bulk
    // precheck was both redundant and wrong.

    core::byte bitlen[20];
    for (unsigned int i = 0; i < 20;) {
        if (reader.bits_remaining() < 4) return false;
        core::uint32 len = reader.get_bits(4);
        if (len == 15) {
            if (reader.bits_remaining() < 4) return false;
            core::uint32 zc = reader.get_bits(4);
            if (zc == 0) {
                bitlen[i++] = 15;
            } else {
                zc += 2;
                while (zc-- > 0 && i < 20) bitlen[i++] = 0;
            }
        } else {
            bitlen[i++] = static_cast<core::byte>(len);
        }
        assert(i <= 20 && "BD table length exceeds 20");
    }
    if (!bd_decoder_.build(bitlen, 20)) return false;

    const size_t table_size = cur_table_size_; // 430 RAR5 or 446 RAR7 ExtraDist
    const size_t cur_dc = use_extra_dist_ ? 80 : 64;
    size_t pos = 0;
    while (pos < table_size) {
        if (reader.bits_remaining() < 1) return false;
        core::uint32 num = bd_decoder_.decode(reader);
        if (num < 16) {
            table_[pos] = static_cast<core::byte>(num);
            pos++;
        } else if (num < 18) {
            core::uint32 n;
            if (num == 16) {
                if (reader.bits_remaining() < 3) return false;
                n = reader.get_bits(3) + 3;
            } else {
                if (reader.bits_remaining() < 7) return false;
                n = reader.get_bits(7) + 11;
            }
            if (pos == 0) return false;
            while (n-- > 0 && pos < table_size) {
                table_[pos] = table_[pos - 1];
                ++pos;
            }
        } else {
            core::uint32 n;
            if (num == 18) {
                if (reader.bits_remaining() < 3) return false;
                n = reader.get_bits(3) + 3;
            } else {
                if (reader.bits_remaining() < 7) return false;
                n = reader.get_bits(7) + 11;
            }
            while (n-- > 0 && pos < table_size) table_[pos++] = 0;
        }
        assert(pos <= table_size && "Table size overrun");
    }

    if (!ld_decoder_.build(table_, 306)) return false;
    if (!dd_decoder_.build(table_ + 306, cur_dc)) return false;
    if (!ldd_decoder_.build(table_ + 306 + cur_dc, 16)) return false;
    if (!rd_decoder_.build(table_ + 306 + cur_dc + 16, 44)) return false;
    tables_ready_ = true;
    return true;
}

bool Decompressor50::decompress(const core::byte* src, size_t src_size, size_t dest_size,
                                bool solid, OutputCallback flush_cb, size_t* out_written,
                                bool* out_finished) {
    if (src_size == 0 || dest_size == 0) return true;
    if (src == nullptr) return false;
    BitReader reader(src, src_size);
    return decompress_internal(reader, dest_size, solid, flush_cb, out_written, out_finished);
}

bool Decompressor50::decompress(BitReader::InputCallback src_cb, size_t src_size, size_t dest_size,
                                bool solid, OutputCallback flush_cb, size_t* out_written,
                                bool* out_finished) {
    if (src_size == 0 || dest_size == 0) return true;
    BitReader reader(src_cb, src_size);
    return decompress_internal(reader, dest_size, solid, flush_cb, out_written, out_finished);
}

bool Decompressor50::decompress_block(const core::byte* src, size_t src_size,
                                      OutputCallback flush_cb, size_t* out_written,
                                      bool* out_finished, bool solid) {
    if (src_size == 0) return true;
    if (src == nullptr) return false;
    BitReader reader(src, src_size);
    return decompress_internal(reader, static_cast<size_t>(-1), solid, flush_cb, out_written,
                               out_finished, /*single_block=*/true);
}

bool Decompressor50::decompress_internal(BitReader& reader, size_t dest_size, bool solid,
                                         OutputCallback flush_cb, size_t* out_written,
                                         bool* out_finished, bool single_block) {
    if (out_written) *out_written = 0;
    if (out_finished) *out_finished = false;

    if (last_error_ != DecompressErrorCode::Ok) return false;

    if (!engine_.ensure_window_alloc()) {
        last_error_ = DecompressErrorCode::AllocationFailed;
        last_error_str_ = "dictionary too large: allocation failed";
        return false;
    }

    // Filter queue lifecycle: a filter region never spans a file boundary, so
    // whole-stream calls (per-file) always start with an empty queue. In
    // per-block mode (single_block) regions may span block boundaries — the
    // queue carries across blocks of the SAME file (solid=true) and is applied
    // when each region's data completes. StreamDecoder resets by constructing
    // a fresh Decompressor50 per file, so no region leaks across files.
    if (!single_block || !solid) {
        engine_.filters_.clear();
        engine_.filters_head_ = 0;
    }
    core::uint64 base_at_entry = solid ? file_base_ : 0;
    // A solid chain accumulates the unpacked bytes of every PRIOR member, so
    // this is an output-position base that grows without bound. Filter regions
    // are derived from it below through a size_t, which wraps silently at 4 GiB
    // on ILP32/WASM — after which every transform is scheduled at a bogus
    // offset and the member decodes to wrong bytes with no error raised. Refuse
    // the address range instead of wrapping into it (v1.41.1).
    if (base_at_entry > static_cast<core::uint64>(SIZE_MAX)) {
        last_error_ = DecompressErrorCode::InvalidInput;
        last_error_str_ = "solid member base exceeds the addressable range";
        return false;
    }
    if (!solid) {
        file_base_ = 0;
        member_start_ = 0;
        base_at_entry = 0;
        engine_.reset_window();
        std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
        std::fill(std::begin(table_), std::end(table_), static_cast<core::byte>(0));
        last_length_ = 0;
        tables_ready_ = false;
    } else if (!single_block) {
        // A whole-stream solid call is a member start: the transform base
        // (see member_start_) resets even though the window carries.
        member_start_ = base_at_entry;
    }
    if (dest_size == 0) return true;

    BlockHeader header;
    header.block_size = -1;
    // Need first header
    if (!read_block_header(reader, header)) return false;
    if (!read_tables(reader, header)) return false;
    if (!header.table_present && !tables_ready_) return false;

    auto block_end_bit = [&](const BlockHeader& h) -> core::uint64 {
        if (h.block_size <= 0) return static_cast<core::uint64>(h.block_start) * 8;
        // 64-bit throughout: the previous form added two ints before casting,
        // so it overflowed at 2 GiB even when block_start was positive.
        return (static_cast<core::uint64>(h.block_start + h.block_size - 1)) * 8 +
               static_cast<core::uint64>(h.block_bit_size);
    };
    core::uint64 end_bit = block_end_bit(header);
    auto cur_bit = [&]() -> core::uint64 {
        return reader.bit_pos();
    };

    size_t total_written = 0;
    engine_.begin_call(); // call-local flush cursor (was a local last_flushed)
    const size_t flush_cadence = engine_.flush_cadence();
    auto cadence = [&]() -> bool {
        if (total_written - engine_.last_flushed_ < flush_cadence &&
            !engine_.has_completed_filter(base_at_entry, total_written))
            return true;
        return engine_.flush_pending(false, base_at_entry, total_written, dest_size, flush_cb);
    };
    // Aggregate filter budget for this decode (report M7).
    size_t filters_total_len = 0;
    // Pending regions are recorded in ABSOLUTE file coordinates (block_start
    // includes base_at_entry) so a queue carried across per-block calls keeps
    // a stable frame. abs_pos() is this call's current absolute output
    // position; local_region_start() converts a region into this call's local
    // frame (regions carried from an earlier block clamp to 0 — their leading
    // plain bytes were already flushed before this call started).
    // Flush machinery lives in ApplyEngine (plan M0/R1): the token loop and
    // the two-phase record applier drive the SAME implementation, so flush
    // triggers, filter application, and chunk boundaries are identical by
    // construction. base_at_entry + total_written reproduce the lambda's
    // abs_pos()/local_region_start() captures exactly.

    // H-06: a token (or table description) that ends past the block's
    // declared bit size is malformed. Failing here keeps the sequential
    // decoder's verdict identical to decode_span()/prescan_member(), which
    // restart every block at its exact byte boundary instead of aligning
    // from wherever the over-read left the reader.
    auto overran_block = [&](core::uint64 bit) {
        return header.block_size > 0 && bit > end_bit;
    };
    if (overran_block(cur_bit())) return false;

    while (total_written < dest_size) {
        core::uint64 cb = cur_bit();
        if (cb >= end_bit) {
            if (overran_block(cb)) return false;
            if (single_block) break;
            if (header.last_block_in_file) break;
            if (!read_block_header(reader, header)) break;
            if (!read_tables(reader, header)) return false;
            if (!header.table_present && !tables_ready_) return false;
            end_bit = block_end_bit(header);
            continue;
        }
        if (reader.bits_remaining() < 1) {
            if (single_block) break;
            if (header.last_block_in_file) break;
            if (!read_block_header(reader, header)) break;
            if (!read_tables(reader, header)) return false;
            end_bit = block_end_bit(header);
            continue;
        }

        core::uint32 slot = ld_decoder_.decode(reader);
        // H-07: the symbol was assembled from zero padding past the end of a
        // truncated stream - fail instead of emitting a fabricated token.
        if (reader.truncated()) return false;
        assert(slot < 306 && "Main LD table slot out of bounds");
        if (slot < 256) {
            if (!engine_.write_literal(slot)) return window_alloc_failed();
            ++total_written;
            if (!cadence()) return false;
            continue;
        }
        if (slot == 256) {
            auto read_filter_data = [&](bool& ok) -> core::uint32 {
                if (reader.bits_remaining() < 2) {
                    ok = false;
                    return 0;
                }
                core::uint32 byte_cnt = reader.get_bits(2) + 1;
                core::uint32 val = 0;
                for (core::uint32 i = 0; i < byte_cnt; ++i) {
                    if (reader.bits_remaining() < 8) {
                        ok = false;
                        return 0;
                    }
                    val |= (reader.get_bits(8) << (i * 8));
                }
                return val;
            };
            bool ok = true;
            core::uint32 f_start = read_filter_data(ok);
            core::uint32 f_len = read_filter_data(ok);
            if (!ok) return false;
            if (f_len > 0x400000) f_len = 0;
            if (reader.bits_remaining() < 3) return false;
            core::uint32 f_type = reader.get_bits(3);
            core::uint32 f_ch = 1;
            if (f_type == 0) {
                if (reader.bits_remaining() < 5) return false;
                f_ch = reader.get_bits(5) + 1;
            }
            FilterEntry fe;
            fe.type = static_cast<core::uint8>(f_type);
            fe.channels = static_cast<core::uint8>(f_ch);
            const core::uint64 rel_start = static_cast<core::uint64>(total_written) + f_start;
            if (rel_start < f_start || base_at_entry > UINT64_MAX - rel_start) return false;
            // ABSOLUTE file coordinates: carried across per-block calls without
            // frame drift (see abs_pos()/local_region_start() above).
            fe.block_start = base_at_entry + rel_start;
            if (fe.block_start > static_cast<core::uint64>(SIZE_MAX)) return false;
            // E8/E8E9/ARM transform base is the offset WITHIN the member: the
            // format resets it per member even on solid chains
            // (verified in v1.36.x — a carried base corrupted every
            // non-first solid member with E8-filtered content).
            if (base_at_entry < member_start_) return false;
            fe.file_offset = fe.block_start - member_start_;
            fe.block_length = f_len;
            // A filter region larger than the window can never be applied
            // intact: by the time the region ends, its start has been
            // overwritten in the circular buffer (report M5).
            if (static_cast<size_t>(f_len) > engine_.win_size_) {
                return false;
            }
            // Aggregate budget: prevents crafted streams from scheduling
            // unbounded transform work at flush time (report M7).
            // When dest_size == SIZE_MAX (streaming via decompress_to_vector),
            // the addition dest_size + win_size_ wraps modulo 2^64, so we
            // skip the check to avoid a false-positive abort.
            if (filters_total_len > SIZE_MAX - static_cast<size_t>(f_len)) return false;
            filters_total_len += static_cast<size_t>(f_len);
            if (dest_size != SIZE_MAX && dest_size <= SIZE_MAX - engine_.win_size_ &&
                filters_total_len > dest_size + engine_.win_size_) {
                return false;
            }
            // Legitimate encoders emit disjoint, ordered regions. Overlapping
            // or backward regions would let a crafted stream emit transform
            // output beyond the region data and out of frame order.
            if (!engine_.filters_.empty()) {
                const FilterEntry& prev = engine_.filters_.back();
                if (fe.block_start < prev.block_start ||
                    fe.block_start - prev.block_start < prev.block_length) {
                    return false;
                }
            }
            // Queue bound: with disjoint regions enforced, per-region work is
            // bounded by the region length itself, so this only bounds queue
            // memory (65536 entries ~= 2 MiB) — large filtered files emit one
            // region per ~1 MiB and legitimately exceed the old 8192 cap.
            if (engine_.filters_.size() - engine_.filters_head_ >= 65536) return false;
#ifdef OPENRAR_CROSS_VALIDATE
            if (engine_.val_src_ != nullptr) {
                engine_.val_regions_.emplace_back(static_cast<size_t>(fe.block_start),
                                                  fe.block_length);
            }
#endif
            engine_.filters_.push_back(fe);
            continue;
        }
        if (slot == 257) {
            if (last_length_ != 0) {
                size_t distance = old_dist_[0];
                // Repeat distances are attacker-controlled state: the
                // sentinel (never set) or an out-of-window value means the
                // stream is corrupt (report M6).
                if (distance == static_cast<size_t>(-1) || distance == 0 ||
                    distance > engine_.win_size_) {
                    return false;
                }
                core::uint32 len = static_cast<core::uint32>(last_length_);
                if (len > dest_size - total_written)
                    len = static_cast<core::uint32>(dest_size - total_written);
                if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
                total_written += len;
                if (!cadence()) return false;
            }
            continue;
        }
        if (slot >= 262) {
            core::uint32 len = slot_to_length(reader, slot - 262);
            if (reader.bits_remaining() < 1) return false;
            core::uint32 dist_slot = dd_decoder_.decode(reader);
            // Release-safe replacement for the old assert.
            if (dist_slot >= (use_extra_dist_ ? 80u : 64u)) return false;
            // C-02: decoded and validated in 64 bits BEFORE it is stored in
            // old_dist_ or narrowed to size_t. A distance beyond the window
            // cannot be copied; fail the stream instead of desyncing the
            // output accounting (report M6).
            const core::uint64 d64 = decode_match_distance(reader, ldd_decoder_, dist_slot);
            if (d64 == 0 || d64 > static_cast<core::uint64>(engine_.win_size_)) return false;
            if (reader.truncated()) return false; // H-07: length/distance bits were padding
            const size_t distance = static_cast<size_t>(d64);
            if (distance > 0x100) {
                ++len;
                if (distance > 0x2000) {
                    ++len;
                    if (distance > 0x40000) ++len;
                }
            }
            old_dist_[3] = old_dist_[2];
            old_dist_[2] = old_dist_[1];
            old_dist_[1] = old_dist_[0];
            old_dist_[0] = distance;
            last_length_ = len;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
            total_written += len;
            if (!cadence()) return false;
            continue;
        }
        if (slot < 262) {
            core::uint32 dist_num = slot - 258;
            size_t distance = old_dist_[dist_num];
            // Same corrupt-stream check as slot 257 (report M6).
            if (distance == static_cast<size_t>(-1) || distance == 0 ||
                distance > engine_.win_size_)
                return false;
            for (core::uint32 i = dist_num; i > 0; --i) old_dist_[i] = old_dist_[i - 1];
            old_dist_[0] = distance;
            if (reader.bits_remaining() < 1) return false;
            core::uint32 len_slot = rd_decoder_.decode(reader);
            core::uint32 len = slot_to_length(reader, len_slot);
            if (reader.truncated()) return false; // H-07
            last_length_ = len;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
            total_written += len;
            if (!cadence()) return false;
            continue;
        }
    }
    if (overran_block(cur_bit())) return false; // H-06: final token overran

    // End-of-call flush. Whole-stream calls and the final block of a
    // per-block stream flush strictly: incomplete regions mean a corrupt
    // stream. Non-final per-block calls flush only completed regions and
    // leave regions spanning into the next block queued (they are applied
    // there once their data has fully arrived).
    if (!single_block || header.last_block_in_file) {
        if (!engine_.flush_pending(true, base_at_entry, total_written, dest_size, flush_cb))
            return false;
    } else if (!engine_.flush_pending(false, base_at_entry, total_written, dest_size, flush_cb)) {
        return false;
    }

    // Truncation check: if the stream ended without a LastBlock flag before
    // producing dest_size bytes, the payload is incomplete.
    if (!single_block && total_written < dest_size && !header.last_block_in_file) return false;

    if (out_written) *out_written = total_written;
    if (out_finished) *out_finished = header.last_block_in_file;
    file_base_ = base_at_entry + static_cast<core::uint64>(total_written);
    return true;
}

bool Decompressor50::prescan_member(const core::byte* src, size_t src_size, PrescanTimeline& out) {
    out = PrescanTimeline{};
    if (src == nullptr || src_size == 0) return false;
    if (last_error_ != DecompressErrorCode::Ok) return false;

    // Table flavor is this instance's configured one (constructor /
    // set_extra_dist), the same flavor the worker and applier instances are
    // built with.
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
    tables_ready_ = false;
    std::fill(std::begin(table_), std::end(table_), static_cast<core::byte>(0));

    // Headers are byte-aligned and contiguous: the next header sits exactly
    // one payload past this header's end (block_size counts payload bytes,
    // the last of which carries block_bit_size valid bits). A fresh BitReader
    // per block at that byte offset is therefore equivalent to the sequential
    // decoder's continuously-advancing reader at block boundaries.
    size_t pos = 0;
    uint32_t cur_desc = 0;
    bool first = true;
    // H-12: the encoder flushes a block every ~32 KiB of tokens, so packed
    // bytes / 32 KiB is a good first guess; capped so a hostile stream
    // cannot make the reservation itself the amplification.
    {
        const size_t guess = std::min<size_t>(src_size / 32768 + 1, 4096);
        out.blocks.reserve(guess);
        out.descriptions.reserve(guess);
    }
    while (true) {
        if (out.blocks.size() >= PRESCAN_MAX_BLOCKS) return false; // R2 cap
        if (pos >= src_size) return false; // ran off the end without LastBlock
        BitReader reader(src + pos, src_size - pos);
        BlockHeader header;
        if (!read_block_header(reader, header)) return false;
        const size_t payload_start = pos + static_cast<size_t>(header.header_size);
        const size_t payload = header.block_size > 0 ? static_cast<size_t>(header.block_size) : 0;
        if (payload_start > src_size || payload > src_size - payload_start) return false;
        if (!read_tables(reader, header)) return false;
        if (header.table_present && header.block_size != 0) {
            PrescanDescription d;
            d.extra_dist = use_extra_dist_;
            d.lengths.assign(table_, table_ + cur_table_size_);
            out.descriptions.push_back(std::move(d));
            cur_desc = static_cast<uint32_t>(out.descriptions.size() - 1);
        }
        if (first) {
            // Non-solid member start: the sequential decoder requires tables
            // in force at the first token; the scan fails closed earlier.
            if (!tables_ready_) return false;
            first = false;
        }
        PrescanBlock b;
        b.src_byte = pos;
        b.header_len = static_cast<uint32_t>(header.header_size);
        b.block_size = static_cast<uint32_t>(payload);
        b.desc_id = cur_desc;
        b.token_bits = static_cast<uint32_t>(reader.bit_pos());
        b.table_present = header.table_present;
        b.last_block = header.last_block_in_file;
        b.block_bit_size = header.block_bit_size;
        out.blocks.push_back(b);
        pos = payload_start + payload;
        if (header.last_block_in_file) {
            out.saw_last_block = true;
            break;
        }
    }
    out.packed_extent = pos;
    out.ok = true;
    return true;
}

bool Decompressor50::initialize_from_description(const PrescanDescription& desc) {
    // A description must match the flavor this instance was configured for;
    // > 4 GiB distances are still rejected per token (64-bit decode, checked
    // against win_size_), so 32-bit targets need no blanket refusal.
    if (desc.extra_dist != use_extra_dist_) {
        last_error_ = DecompressErrorCode::InvalidInput;
        return false;
    }
    use_extra_dist_ = desc.extra_dist;
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
    if (desc.lengths.size() != cur_table_size_) {
        last_error_ = DecompressErrorCode::InvalidInput;
        last_error_str_ = "recorded description length mismatch";
        return false;
    }
    std::memcpy(table_, desc.lengths.data(), cur_table_size_);
    const size_t cur_dc = use_extra_dist_ ? 80 : 64;
    if (!ld_decoder_.build(table_, 306)) return false;
    if (!dd_decoder_.build(table_ + 306, cur_dc)) return false;
    if (!ldd_decoder_.build(table_ + 306 + cur_dc, 16)) return false;
    if (!rd_decoder_.build(table_ + 306 + cur_dc + 16, 44)) return false;
    tables_ready_ = true;
    return true;
}

bool Decompressor50::decode_span(const PrescanTimeline& tl, const core::byte* src, size_t src_size,
                                 size_t dest_size, uint32_t first_block, uint32_t last_block,
                                 size_t rec_cap, SpanRecords& out) {
    // Capacity-preserving reset: callers that reuse a SpanRecords across
    // spans (the pipeline workers, the census probe) keep the record/pool
    // buffers warm instead of re-allocating per span.
    out.recs.clear();
    out.lit_pool.clear();
    out.out_lower_bound = 0;
    out.crossed_hint = false;
    out.saw_last_block = false;
    out.ok = false;
    if (src == nullptr || tl.blocks.empty() || first_block >= tl.blocks.size() ||
        last_block >= tl.blocks.size() || first_block > last_block) {
        last_error_ = DecompressErrorCode::InvalidInput;
        last_error_str_ = "span block range out of bounds";
        return false;
    }
    // Slot-257's emitted length and rep distances are APPLIER state; the
    // worker tracks none of it (Gate 0 section 0: the symbol parse is
    // stateless). Only bit positions, tables, and the record buffers live
    // here. total_local is the span-relative output LOWER bound (257
    // contributes 0 - its length is unknowable without prior tokens) and
    // feeds only the crossed_hint, never any semantic decision [R8].
    tables_ready_ = false;
    size_t total_local = 0;
    size_t run_start = static_cast<size_t>(-1); // lit_pool offset of the open literal run
    core::uint32 run_count = 0;
    core::byte run_buf[255]; // open literal run; appended to lit_pool on close
    // v1.38.0 M1a: reserve once from the span's packed size instead of
    // growing through the token loop (the G5 cap still bounds everything;
    // estimates stay well under it so resident memory does not scale with
    // the worker count). A failed reserve fails the span the same way a
    // failed mid-loop growth would (F1/F3), just earlier and cleanly.
    {
        size_t span_packed = 0;
        for (uint32_t bi = first_block; bi <= last_block; ++bi)
            span_packed += tl.blocks[bi].header_len + tl.blocks[bi].block_size;
        const size_t est_pool = std::min(rec_cap, std::max<size_t>(64 * 1024, span_packed));
        const size_t est_recs =
            std::min(rec_cap / sizeof(OpRecord), std::max<size_t>(4096, span_packed / 2));
        try {
            out.lit_pool.reserve(est_pool);
            out.recs.reserve(est_recs);
        } catch (...) {
            last_error_ = DecompressErrorCode::AllocationFailed;
            last_error_str_ = "span record pool reserve failed (G5)";
            return false;
        }
    }
    // v1.38.0 M1a: running record-pool byte counter, updated exactly where
    // recs/lit_pool grow, so the G5 check at the loop top fails on the same
    // token the rec_bytes() lambda failed on - representation only [D8].
    size_t rec_bytes_now = 0;
    auto close_run = [&]() {
        if (run_count == 0) return;
        out.lit_pool.insert(out.lit_pool.end(), run_buf, run_buf + run_count);
        rec_bytes_now += run_count;
        OpRecord r;
        r.tag = static_cast<core::uint8>(OpRecord::Tag::Lit);
        r.aux = static_cast<core::uint8>(run_count);
        r.a = 0;
        r.b = static_cast<core::uint32>(run_start);
        out.recs.push_back(r);
        rec_bytes_now += sizeof(OpRecord);
        run_count = 0;
        run_start = static_cast<size_t>(-1);
    };
    auto push_lit = [&](core::uint32 slot) {
        if (run_count == 0) run_start = out.lit_pool.size();
        run_buf[run_count] = static_cast<core::byte>(slot);
        ++run_count;
        ++total_local;
        if (run_count == 255) close_run();
    };

    for (uint32_t bi = first_block; bi <= last_block; ++bi) {
        const PrescanBlock& expect = tl.blocks[bi];
        if (expect.src_byte >= src_size) return false;
        BitReader reader(src + expect.src_byte, src_size - expect.src_byte);
        BlockHeader header;
        if (!read_block_header(reader, header)) return false;
        // F6 detector: the wire header must match the pre-scan record.
        if (static_cast<size_t>(header.header_size) != expect.header_len ||
            (header.block_size > 0 ? static_cast<uint32_t>(header.block_size) : 0u) !=
                expect.block_size ||
            header.table_present != expect.table_present ||
            header.last_block_in_file != expect.last_block ||
            header.block_bit_size != expect.block_bit_size) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "span header diverged from pre-scan timeline";
            return false;
        }
        if (header.table_present && header.block_size != 0) {
            // R10: build from the pre-scanned description; no bit re-parse.
            // The description's bits still occupy the stream, so skip them:
            // token_bits is where this block's tokens start.
            if (expect.desc_id >= tl.descriptions.size()) return false;
            if (!initialize_from_description(tl.descriptions[expect.desc_id])) return false;
        } else if (!tables_ready_) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "span starts on reuse block without tables";
            return false;
        }
        // Skip header + (already-built) description bits down to the first
        // token. consume_bits caps at the accumulator width (~57 bits), so
        // long descriptions skip in chunks.
        while (reader.bit_pos() < expect.token_bits) {
            // Forward-progress guard. consume_bits() takes ZERO bits once the
            // accumulator is empty and the staging callback is starved, so
            // without this the loop spins forever on a truncated stream.
            // prescan_member records token_bits only after read_tables parsed
            // that description successfully, so the bits exist today — but a
            // skip loop that can spin is one refactor away from a fuzz hang
            // (v1.41.1).
            if (reader.bits_remaining() == 0) {
                last_error_ = DecompressErrorCode::InvalidInput;
                last_error_str_ = "span token-bit skip hit end of input";
                return false;
            }
            core::uint64 remain = expect.token_bits - reader.bit_pos();
            reader.consume_bits(static_cast<unsigned>(remain > 57 ? 57 : remain));
        }
        if (reader.bit_pos() != expect.token_bits) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "span token-bit skip mismatch";
            return false;
        }

        auto block_end_bit = [&](const BlockHeader& h) -> core::uint64 {
            if (h.block_size <= 0) return static_cast<core::uint64>(h.block_start) * 8;
            return (static_cast<core::uint64>(h.block_start + h.block_size - 1)) * 8 +
                   static_cast<core::uint64>(h.block_bit_size);
        };
        const core::uint64 end_bit = block_end_bit(header);

        while (reader.bit_pos() < end_bit && reader.bits_remaining() >= 1) {
            if (rec_bytes_now > rec_cap) {
                last_error_ = DecompressErrorCode::AllocationFailed;
                last_error_str_ = "span record pool cap exceeded (G5)";
                return false;
            }
            core::uint32 slot = ld_decoder_.decode(reader);
            if (reader.truncated()) return false; // H-07: padded symbol
            if (slot < 256) {
                push_lit(slot);
                continue;
            }
            if (slot == 256) {
                auto read_filter_data = [&](bool& ok) -> core::uint32 {
                    if (reader.bits_remaining() < 2) {
                        ok = false;
                        return 0;
                    }
                    core::uint32 byte_cnt = reader.get_bits(2) + 1;
                    core::uint32 val = 0;
                    for (core::uint32 i = 0; i < byte_cnt; ++i) {
                        if (reader.bits_remaining() < 8) {
                            ok = false;
                            return 0;
                        }
                        val |= (reader.get_bits(8) << (i * 8));
                    }
                    return val;
                };
                bool ok = true;
                core::uint32 f_start = read_filter_data(ok);
                core::uint32 f_len = ok ? read_filter_data(ok) : 0;
                if (!ok) return false;
                if (f_len > 0x400000) f_len = 0;
                if (reader.bits_remaining() < 3) return false;
                core::uint32 f_type = reader.get_bits(3);
                core::uint32 f_ch = 1;
                if (f_type == 0) {
                    if (reader.bits_remaining() < 5) return false;
                    f_ch = reader.get_bits(5) + 1;
                }
                close_run();
                OpRecord r;
                r.tag = static_cast<core::uint8>(OpRecord::Tag::Filter);
                r.aux = static_cast<core::uint8>(f_type | ((f_ch - 1) << 3));
                r.a = f_start; // raw delta as-read; applier resolves [R7]
                r.b = f_len;
                out.recs.push_back(r);
                rec_bytes_now += sizeof(OpRecord);
                continue;
            }
            if (slot == 257) {
                // No payload; the applier re-emits from its own rep state and
                // counts the real length. The worker's bound stays a bound.
                close_run();
                OpRecord r;
                r.tag = static_cast<core::uint8>(OpRecord::Tag::R257);
                r.aux = 0;
                r.a = 0;
                r.b = 0;
                out.recs.push_back(r);
                rec_bytes_now += sizeof(OpRecord);
                continue;
            }
            if (slot >= 262) {
                core::uint32 len = slot_to_length(reader, slot - 262);
                if (reader.bits_remaining() < 1) return false;
                core::uint32 dist_slot = dd_decoder_.decode(reader);
                if (dist_slot >= (use_extra_dist_ ? 80u : 64u)) return false;
                // C-02: 64-bit decode + validate before narrowing (shared helper
                // with decompress_internal so the engines cannot drift).
                const core::uint64 d64 = decode_match_distance(reader, ldd_decoder_, dist_slot);
                if (d64 == 0 || d64 > static_cast<core::uint64>(engine_.win_size_)) return false;
                if (reader.truncated()) return false; // H-07
                const size_t distance = static_cast<size_t>(d64);
                // The distance-band ladder applies HERE, exactly once: the
                // record carries the adjusted length and the applier never
                // re-applies it (Gate 0 section 0).
                if (distance > 0x100) {
                    ++len;
                    if (distance > 0x2000) {
                        ++len;
                        if (distance > 0x40000) ++len;
                    }
                }
                // Position-free validity only (Gate 0 section 4): the applier
                // re-runs the full sequential check set at apply positions.
                if (distance == 0 || distance > engine_.win_size_) return false;
                close_run();
                OpRecord r;
                r.tag = static_cast<core::uint8>(OpRecord::Tag::Match);
                r.aux = 0;
                // Stored 0-BASED: distance is 1-based and reaches win_size_,
                // which is 2^32 for a maximal v0 member — one value that does
                // not fit a u32 as-is (2^32 truncates to 0). Shifting the whole
                // range down by one makes [1, 2^32] map exactly onto
                // [0, 0xFFFFFFFF], with no dependency on the caller having
                // gated win_size to <= 4 GiB (v1.41.1).
                r.a = static_cast<core::uint32>(distance - 1);
                r.b = len;
                out.recs.push_back(r);
                rec_bytes_now += sizeof(OpRecord);
                total_local += len;
                continue;
            }
            // slots 258..261: rep distances. No ladder (the sequential
            // decoder applies none on this path); the applier resolves
            // old_dist[idx] and rotates.
            core::uint32 dist_num = slot - 258;
            if (reader.bits_remaining() < 1) return false;
            core::uint32 len_slot = rd_decoder_.decode(reader);
            core::uint32 len = slot_to_length(reader, len_slot);
            if (reader.truncated()) return false; // H-07
            close_run();
            OpRecord r;
            r.tag = static_cast<core::uint8>(OpRecord::Tag::Rep);
            r.aux = static_cast<core::uint8>(dist_num);
            r.a = len;
            r.b = 0;
            out.recs.push_back(r);
            rec_bytes_now += sizeof(OpRecord);
            total_local += len;
        }
        // H-06: same verdict as decompress_internal - a token that ends past
        // the declared block bit size is malformed, not silently tolerated.
        if (header.block_size > 0 && reader.bit_pos() > end_bit) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "token overran block bit size";
            return false;
        }
        if (bi == last_block && expect.last_block) out.saw_last_block = true;
    }
    close_run();
    out.out_lower_bound = total_local;
    out.crossed_hint = total_local >= dest_size;
    out.ok = true;
    return true;
}

bool Decompressor50::apply_begin() {
    if (last_error_ != DecompressErrorCode::Ok) return false;
    if (!engine_.ensure_window_alloc()) {
        last_error_ = DecompressErrorCode::AllocationFailed;
        last_error_str_ = "dictionary too large: allocation failed";
        return false;
    }
    // Non-solid member start: the !solid branch of decompress_internal,
    // minus table state (tables are span-worker concerns; the applier owns
    // only window, filter queue, flush cursor, and rep state).
    engine_.reset_window();
    engine_.filters_.clear();
    engine_.filters_head_ = 0;
    engine_.begin_call();
    filters_total_len_ = 0;
    std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
    last_length_ = 0;
    return true;
}

bool Decompressor50::apply_span_records(const SpanRecords& sr, size_t dest_size, bool is_last,
                                        OutputCallback flush_cb, size_t* io_total_written) {
    if (!sr.ok) return false; // the producer bailed; half-built records are not records
    // Every other out-param in this class is null-guarded; this one was
    // dereferenced unconditionally (v1.41.1).
    size_t total_written = io_total_written ? *io_total_written : 0;
    // The sequential decoder's per-token flush cadence, verbatim: plain
    // bytes flush at the 64 KiB output cadence and at filter completions
    // (inside engine_.flush_pending). Filter records skip the cadence check
    // exactly like the sequential filter-token branch.
    const size_t flush_cadence = engine_.flush_cadence();
    auto cadence = [&]() -> bool {
        if (total_written - engine_.last_flushed_ < flush_cadence &&
            !engine_.has_completed_filter(0, total_written))
            return true;
        return engine_.flush_pending(false, 0, total_written, dest_size, flush_cb);
    };

    for (const auto& r : sr.recs) {
        if (total_written >= dest_size) break; // applier clamp point [R8]
        auto tag = static_cast<OpRecord::Tag>(r.tag);
        if (tag == OpRecord::Tag::Lit) {
            // Bulk literal copy: wrap-aware memcpys straight from the record
            // pool, segmented at the window edge AND at the sequential
            // decoder's 64 KiB flush cadence points — the flush fires at the
            // exact byte it fires at in the sequential loop, so callback
            // chunk boundaries stay identical (M0/R1 invariant).
            size_t remaining = r.aux;
            if (remaining > dest_size - total_written) remaining = dest_size - total_written;
            // The distance side of every record is validated below; the
            // literal-pool side was not, so an offset into lit_pool was taken
            // on trust and copied straight into the output. decode_span
            // writes self-consistent offsets today, but SpanRecords is a
            // public struct and the read side must not depend on there being
            // exactly one producer. Bound by subtraction so it cannot wrap
            // (v1.41.1).
            if (remaining > 0) {
                if (static_cast<size_t>(r.b) > sr.lit_pool.size() ||
                    remaining > sr.lit_pool.size() - static_cast<size_t>(r.b)) {
                    last_error_ = DecompressErrorCode::InvalidInput;
                    last_error_str_ = "span literal record out of range";
                    return false;
                }
            }
            const core::byte* srcp = sr.lit_pool.data() + r.b;
            while (remaining > 0) {
                size_t stretch = engine_.win_size_ - engine_.win_pos_;
                if (stretch > remaining) stretch = remaining;
                // Cadence segmentation only while the flush cursor is inside
                // the current window: with a PENDING FILTER, flush_pending
                // stops at the region start and freezes last_flushed_ below
                // total_written (the sequential per-byte loop just re-attempts
                // and emits nothing) - clamping to a wrapped to_cadence there
                // would zero the stretch and spin forever. The post-copy
                // flush attempt below still fires at the same positions, so
                // callback chunk boundaries are unchanged.
                const size_t pending = total_written - engine_.last_flushed_;
                if (pending < flush_cadence) {
                    const size_t to_cadence = flush_cadence - pending;
                    if (to_cadence < stretch) stretch = to_cadence;
                }
                if (!engine_.reserve(stretch)) return window_alloc_failed();
                std::memcpy(engine_.window_.data() + engine_.win_pos_, srcp, stretch);
                engine_.win_pos_ = engine_.wrap_up(engine_.win_pos_ + stretch);
                if (engine_.win_pos_ == 0) engine_.first_win_done_ = true;
                srcp += stretch;
                total_written += stretch;
                remaining -= stretch;
                if (!cadence()) return false;
            }
            continue;
        }
        if (tag == OpRecord::Tag::Match) {
            // r.a is the 0-based distance (see the producer): widen before the
            // +1 so 0xFFFFFFFF becomes 2^32, not 0 (v1.41.1).
            size_t distance = static_cast<size_t>(r.a) + 1;
            size_t len = r.b;
            // Sequential order, verbatim: rep-state update, last_length
            // (the WIRE length — the dest clamp does not shrink it),
            // validation, clamp, copy.
            old_dist_[3] = old_dist_[2];
            old_dist_[2] = old_dist_[1];
            old_dist_[1] = old_dist_[0];
            old_dist_[0] = distance;
            last_length_ = len;
            if (distance == 0 || distance > engine_.win_size_) return false;
            if (len > dest_size - total_written) len = dest_size - total_written;
            if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
            total_written += len;
            if (!cadence()) return false;
            continue;
        }
        if (tag == OpRecord::Tag::Rep) {
            core::uint32 dist_num = r.aux;
            if (dist_num >= 4) return false;
            size_t distance = old_dist_[dist_num];
            // Repeat distances are attacker-controlled state: the sentinel
            // (never set) or an out-of-window value means the stream is
            // corrupt (report M6) — the sequential check, verbatim.
            if (distance == static_cast<size_t>(-1) || distance == 0 ||
                distance > engine_.win_size_) {
                return false;
            }
            for (core::uint32 i = dist_num; i > 0; --i) old_dist_[i] = old_dist_[i - 1];
            old_dist_[0] = distance;
            last_length_ = r.a;
            size_t len = r.a;
            if (len > dest_size - total_written) len = dest_size - total_written;
            if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
            total_written += len;
            if (!cadence()) return false;
            continue;
        }
        if (tag == OpRecord::Tag::R257) {
            // Re-emit from the applier's own state; updates neither
            // old_dist_ nor last_length_ (sequential slot-257 semantics).
            if (last_length_ != 0) {
                size_t distance = old_dist_[0];
                if (distance == static_cast<size_t>(-1) || distance == 0 ||
                    distance > engine_.win_size_) {
                    return false;
                }
                size_t len = last_length_;
                if (len > dest_size - total_written) len = dest_size - total_written;
                if (!engine_.copy_match(distance, len, total_written)) return window_alloc_failed();
                total_written += len;
                if (!cadence()) return false;
            }
            continue;
        }
        // Filter: resolve the raw start delta against the running position
        // [R7] and run the sequential push-site check set, verbatim.
        // This branch was the unconditional fall-through, so any tag outside
        // 0..3 was silently reinterpreted as a filter record. Fail closed
        // instead: an unknown tag is a corrupt or future record, never a
        // reason to schedule a transform (v1.41.1).
        if (tag != OpRecord::Tag::Filter) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "unknown span op record tag";
            return false;
        }
        FilterEntry fe;
        fe.type = static_cast<core::uint8>(r.aux & 7);
        fe.channels = static_cast<core::uint8>(((r.aux >> 3) & 31) + 1);
        const core::uint64 rel_start = static_cast<core::uint64>(total_written) + r.a;
        if (rel_start < r.a || rel_start > static_cast<core::uint64>(SIZE_MAX)) return false;
        fe.block_start = rel_start;
        // Per-member transform base (v1.36.16): the base is the offset
        // WITHIN the member; the applier's frame IS the member frame.
        fe.file_offset = rel_start;
        fe.block_length = r.b;
        // A filter region larger than the window can never be applied
        // intact (report M5).
        if (static_cast<size_t>(fe.block_length) > engine_.win_size_) return false;
        // Aggregate budget against unbounded scheduled transform work
        // (report M7); dest_size == SIZE_MAX is a sequential-streaming
        // shape that never reaches the applier, kept for symmetry.
        if (filters_total_len_ > SIZE_MAX - static_cast<size_t>(fe.block_length)) return false;
        filters_total_len_ += static_cast<size_t>(fe.block_length);
        if (dest_size != SIZE_MAX && dest_size <= SIZE_MAX - engine_.win_size_ &&
            filters_total_len_ > dest_size + engine_.win_size_) {
            return false;
        }
        // Legitimate encoders emit disjoint, ordered regions; overlap or
        // backward regions let a crafted stream emit transform output out
        // of frame order (the sequential rejection, verbatim).
        if (!engine_.filters_.empty()) {
            const FilterEntry& prev = engine_.filters_.back();
            if (fe.block_start < prev.block_start ||
                fe.block_start - prev.block_start < prev.block_length) {
                return false;
            }
        }
        if (engine_.filters_.size() - engine_.filters_head_ >= 65536) return false;
#ifdef OPENRAR_CROSS_VALIDATE
        if (engine_.val_src_ != nullptr) {
            engine_.val_regions_.emplace_back(static_cast<size_t>(fe.block_start), fe.block_length);
        }
#endif
        engine_.filters_.push_back(fe);
    }

    if (is_last) {
        // End-of-call flush, strict: incomplete regions mean a corrupt
        // stream (the sequential whole-stream call's behavior).
        if (!engine_.flush_pending(true, 0, total_written, dest_size, flush_cb)) return false;
        // Truncation: a member that ends below dest_size without a
        // LastBlock flag is incomplete (the sequential check; the span
        // trailer carries the flag because the worker decoded the whole
        // span regardless of the applier's stop point [R8]).
        if (total_written < dest_size && !sr.saw_last_block) return false;
    }
    if (io_total_written) *io_total_written = total_written;
    return true;
}

bool Decompressor50::decompress_to_vector(const core::byte* src, size_t src_size,
                                          std::vector<core::byte>& out, bool solid) {
    out.clear();
    if (src == nullptr || src_size == 0) return true;

    size_t cap = std::max<size_t>(64 * 1024, src_size * 8);
    if (cap > static_cast<size_t>(MAX_STREAM_OUTPUT)) cap = static_cast<size_t>(MAX_STREAM_OUTPUT);
    try {
        out.reserve(cap);
    } catch (const std::bad_alloc&) {
        last_error_ = DecompressErrorCode::AllocationFailed;
        last_error_str_ = "failed to reserve stream output buffer";
        return false;
    }

    auto cb = [&](const core::byte* data, size_t size) -> bool {
        if (out.size() > MAX_STREAM_OUTPUT || size > MAX_STREAM_OUTPUT - out.size()) return false;
        try {
            out.insert(out.end(), data, data + size);
        } catch (const std::bad_alloc&) {
            last_error_ = DecompressErrorCode::AllocationFailed;
            last_error_str_ = "out of memory growing stream output buffer";
            return false;
        }
        return true;
    };

    size_t written = 0;
    bool finished = false;
    if (!decompress(src, src_size, SIZE_MAX, solid, cb, &written, &finished)) {
        out.clear();
        return false;
    }

    if (written != out.size() || !finished) {
        out.clear();
        return false;
    }

    return true;
}

} // namespace openrar::compress
