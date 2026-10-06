#include "decompressor50.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>

namespace openrar::compress {

// ------------------------------------------------------------------
// BitReader : bit_pos based, MSB first, zero padded beyond end
// ------------------------------------------------------------------
BitReader::BitReader(const core::byte* data, size_t size)
    : p_(data), end_(data + size), acc_(0), n_(0), consumed_(0), size_(size), fetched_(size) {}

BitReader::BitReader(InputCallback cb, size_t size)
    : cb_(cb), buf_(65536), p_(buf_.data()), end_(buf_.data()), acc_(0), n_(0), consumed_(0),
      size_(size), fetched_(0) {}

void BitReader::refill() {
    // Fill the accumulator to at least 57 bits, fetching successive input
    // chunks as needed, so a peek/consume is served short ONLY at the true
    // end of the stream (zero padding is EOF semantics). The staging buffer
    // boundary must never leak into delivered bits: with a single-shot load,
    // an accumulator down to its last bits coupled with a buffer down to its
    // last bytes returned a short mid-stream peek (observed as scattered
    // single-byte corruption in every streaming consumer of large members
    // while contiguous readers over the same stream decoded cleanly).
    if (n_ > 56) return;
    for (;;) {
        if (p_ >= end_) {
            if (!cb_ || fetched_ >= size_) return; // true end of stream
            size_t to_read = std::min<size_t>(buf_.size(), size_ - fetched_);
            size_t n = cb_(buf_.data(), to_read);
            p_ = buf_.data();
            end_ = p_ + n;
            fetched_ += n;
            if (n == 0) return; // input starved; treat as end of stream
        }
        unsigned int max_bytes = (64 - n_) / 8;
        unsigned int k = std::min<unsigned int>(max_bytes, static_cast<unsigned int>(end_ - p_));
        if (k == 0) return; // accumulator holds >= 57 bits
        core::uint64 v = 0;
        std::memcpy(&v, p_, k);
#if defined(_MSC_VER)
        v = _byteswap_uint64(v);
#else
        v = __builtin_bswap64(v);
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

size_t BitReader::bits_remaining() const {
    size_t rem_bytes = (end_ - p_) + (size_ - fetched_);
    return rem_bytes * 8 + n_;
}

core::uint32 BitReader::peek_bits(unsigned int count) {
    if (count == 0) return 0;
    if (n_ < count) refill();
    unsigned int take = std::min(count, n_);
    core::uint32 val = static_cast<core::uint32>(take ? acc_ >> (n_ - take) : 0);
    return val << (count - take);
}

core::uint64 BitReader::peek_bits64(unsigned int count) {
    if (count == 0) return 0;
    if (n_ < count) refill();
    unsigned int take = std::min(count, n_);
    core::uint64 val = (take ? acc_ >> (n_ - take) : 0);
    return val << (count - take);
}

core::uint32 BitReader::get_bits(unsigned int count) {
    core::uint32 val = peek_bits(count);
    consume_bits(count);
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
Decompressor50::Decompressor50(size_t win_size) {
    // win_size == 0 is rejected here: a zero window makes the circular-window
    // arithmetic (modulo win_size_, window_[win_pos_]) undefined. Callers that
    // genuinely want a placeholder window get the default 2 MiB one
    // (DEFAULT_WIN_SIZE - see decompressor50.hpp for why it must mirror the
    // compressor's 2 MiB default).
    engine_.init(win_size ? win_size : DEFAULT_WIN_SIZE);
    last_error_ = DecompressErrorCode::Ok;
    last_error_str_.clear();

    // Two failure modes for the sliding dictionary allocation:
    //   (a) declared size exceeds this build's ALLOC_LIMIT (1 GiB). The spec
    //       ceilings SPEC_MAX_V0 (4 GiB, version0) and SPEC_MAX_V1 (1 TiB,
    //       version1) are both strictly greater than ALLOC_LIMIT, so any
    //       value past either spec limit necessarily trips this branch —
    //       no separate SPEC* check is needed. The archive layer maps
    //       spec-invalid dictionary bits to a sentinel > ALLOC_LIMIT
    //       (format/header_reader.cpp), which lands here.
    //   (b) size is within limits but the OS refuses the allocation.
    if (engine_.win_size_ > ALLOC_LIMIT) {
        last_error_ = DecompressErrorCode::DictionaryTooLarge;
        last_error_str_ = "dictionary too large";
    }
    std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
    // use_extra_dist_ enables the RAR7 446-symbol table whose extra distance
    // slots 68-79 need d_bits up to 38 for windows > 4 GiB.
    use_extra_dist_ = (engine_.win_size_ > (4ULL * 1024 * 1024 * 1024));
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
}

// ── ApplyEngine (v1.37.0 plan M0, review R1): the single apply path ─────────
//
// Driven by decompress_internal's token loop below AND by the two-phase
// record applier (parallel_decode driver). Extracted verbatim from the
// sequential decoder; the refactor is byte-identity-neutral and frozen by
// the full gate.

void Decompressor50::ApplyEngine::init(size_t win_size) {
    win_size_ = win_size;
    win_mask_ = win_size_ ? win_size_ - 1 : 0;
    win_pow2_ = (win_size_ & (win_size_ - 1)) == 0;
}

size_t Decompressor50::ApplyEngine::wrap_up(size_t pos) const {
    if (win_pow2_) return pos & win_mask_;
    return pos >= win_size_ ? pos - win_size_ : pos;
}

bool Decompressor50::ApplyEngine::ensure_window_alloc() {
    if (window_ready_ || win_size_ == 0) return true;
    try {
        window_.assign(win_size_, 0);
        window_ready_ = true;
    } catch (...) {
        window_.clear();
        return false;
    }
    return true;
}

void Decompressor50::ApplyEngine::reset_window() {
    if (window_ready_) std::fill(window_.begin(), window_.end(), static_cast<core::byte>(0));
    win_pos_ = 0;
    unp_ptr_ = 0;
    first_win_done_ = false;
}

void Decompressor50::ApplyEngine::write_literal(core::uint32 slot) {
    window_[win_pos_] = static_cast<core::byte>(slot);
    win_pos_ = wrap_up(win_pos_ + 1);
    unp_ptr_ = wrap_up(unp_ptr_ + 1);
    if (unp_ptr_ == 0) first_win_done_ = true;
}

void Decompressor50::ApplyEngine::copy_match(size_t distance, size_t length,
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

    if (distance == 0 || distance > win_size_) return;
    size_t src = wrap_up(unp_ptr_ + win_size_ - distance);
    if (distance > unp_ptr_ && !first_win_done_) {
        // INTENDED (RAR5 spec): references past written data on the first
        // window pass decode as zeros. Do not "fix" this into an error.
        for (size_t i = 0; i < length; ++i) {
            window_[win_pos_] = 0;
            win_pos_ = wrap_up(win_pos_ + 1);
            unp_ptr_ = wrap_up(unp_ptr_ + 1);
            if (unp_ptr_ == 0) first_win_done_ = true;
        }
        return;
    }

    if (src == win_pos_) {
        // distance == win_size_: the circular reference is this very slot, so
        // a byte-exact copy rewrites each position with its own old content.
        // Advancing without writing is the same result and cannot be expressed
        // as memcpy (dst == src) or a chunked loop with gap 0.
        win_pos_ = wrap_up(win_pos_ + length);
        unp_ptr_ = wrap_up(unp_ptr_ + length);
        if (unp_ptr_ == 0) first_win_done_ = true;
        return;
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
        unp_ptr_ = wrap_up(unp_ptr_ + length);
        if (unp_ptr_ == 0) first_win_done_ = true;
        return;
    }

    while (length > 0) {
        size_t chunk = length;
        if (chunk > win_size_ - win_pos_) chunk = win_size_ - win_pos_;
        if (chunk > win_size_ - src) chunk = win_size_ - src;
        // The linear gap must be recomputed from the CURRENT positions: when
        // the match source wraps the window end mid-copy (first chunk clamped
        // by the window edge), src re-enters at 0 and ends up BEHIND dst — the
        // pre-loop gap is then stale by a full window, and an unclamped memcpy
        // overtakes its own source, letting memmove's as-if-through-temp
        // semantics substitute stale pre-match bytes for the just-written
        // ones (single-byte corruption, found on a 16.7 MB file where a
        // dist=26 match started at window offset 0x19). With src < win_pos_
        // the clamp is win_pos_ - src; positions advance in lockstep, so this
        // equals the true circular distance and keeps every memcpy disjoint
        // from its own source region.
        size_t gap_now = (src < win_pos_) ? (win_pos_ - src) : (src - win_pos_);
        if (chunk > gap_now) chunk = gap_now;

        std::memcpy(&window_[win_pos_], &window_[src], chunk);

        // Wildcopy RLE expansion. Replicating from the head of the just-written
        // region is byte-exact only once at least `distance` bytes are written:
        // from then on every reference falls inside the written region. That is
        // the common low-distance case (gap == distance, chunk == distance).
        // When src sits ahead of dst (post-wrap, gap == win_size_ - distance)
        // chunk can never reach `distance`, so the expansion must be skipped
        // and the outer loop copies chunk-wise. The step clamp against the
        // window end also disables it there; the outer loop then wraps.
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
        unp_ptr_ = wrap_up(unp_ptr_ + chunk);
        if (unp_ptr_ == 0) first_win_done_ = true;
    }
}

bool Decompressor50::ApplyEngine::flush_plain_up_to(size_t target, size_t total_written,
                                                    size_t dest_size, OutputCallback cb) {
    if (target > dest_size) target = dest_size;
    while (last_flushed_ < target) {
        size_t chunk = target - last_flushed_;
        size_t back = (total_written - last_flushed_) % win_size_;
        size_t circ_start = (unp_ptr_ + win_size_ - back) % win_size_;
        size_t max_contig = win_size_ - circ_start;
        if (chunk > max_contig) chunk = max_contig;

        if (chunk == 0) {
            std::fprintf(stderr, "FLUSH CHUNK0 last_flushed=%zu target=%zu total=%zu\n",
                         last_flushed_, target, total_written);
            std::abort();
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
    // Pending regions are recorded in ABSOLUTE file coordinates (block_start
    // includes base_at_entry) so a queue carried across per-block calls keeps
    // a stable frame. abs_pos() is this call's current absolute output
    // position; local_region_start() converts a region into this call's local
    // frame (regions carried from an earlier block clamp to 0 — their leading
    // plain bytes were already flushed before this call started). total_written
    // does not change during a flush, so the call-site value is equivalent to
    // the sequential decoder's live lambda capture.
    auto abs_pos = [&]() -> core::uint64 {
        return base_at_entry + static_cast<core::uint64>(total_written);
    };
    auto local_region_start = [&](const FilterEntry& f) -> size_t {
        return f.block_start > base_at_entry ? static_cast<size_t>(f.block_start - base_at_entry)
                                             : 0;
    };
    while (!filters_.empty()) {
        const auto& f = filters_.front();
        if (f.block_start + f.block_length <= abs_pos()) {
            // 1. Flush any plain data preceding this filter:
            size_t region_local = local_region_start(f);
            if (last_flushed_ < region_local) {
                if (!flush_plain_up_to(region_local, total_written, dest_size, cb)) return false;
            }

            // 2. Extract filter region from circular window WITHOUT modifying window_:
            size_t len = f.block_length;
            if (len > 0) {
                if (abs_pos() - f.block_start >= win_size_) {
                    return false;
                }
                size_t back = static_cast<size_t>((abs_pos() - f.block_start) % win_size_);
                size_t circ_start = (unp_ptr_ + win_size_ - back) % win_size_;
                std::vector<core::byte> buf(len);
                size_t cur = circ_start;
                for (size_t i = 0; i < len; ++i) {
                    buf[i] = window_[cur];
                    cur++;
                    if (cur == win_size_) cur = 0;
                }

                std::vector<core::byte> out_buf(len);
                if (f.type == 0) {
                    Filters50::apply_delta(buf.data(), out_buf.data(), len, f.channels);
                } else if (f.type == 1) {
                    std::memcpy(out_buf.data(), buf.data(), len);
                    Filters50::apply_e8(out_buf.data(), len, f.file_offset, false);
                } else if (f.type == 2) {
                    std::memcpy(out_buf.data(), buf.data(), len);
                    Filters50::apply_e8(out_buf.data(), len, f.file_offset, true);
                } else if (f.type == 3) {
                    std::memcpy(out_buf.data(), buf.data(), len);
                    Filters50::apply_arm(out_buf.data(), len, f.file_offset);
                } else {
                    // Undefined filter types 4-7: treat as raw data per spec
                    std::memcpy(out_buf.data(), buf.data(), len);
                }

                // 3. Emit filtered bytes directly to flush_cb (never touching window_!)
                size_t to_emit = len;
                if (last_flushed_ + to_emit > dest_size) {
                    to_emit = dest_size - last_flushed_;
                }
                if (to_emit > 0) {
                    if (cb && !cb(out_buf.data(), to_emit)) return false;
                    last_flushed_ += to_emit;
                }
            }
            filters_.erase(filters_.begin());
        } else {
            if (flush_all) {
                if (f.block_start < abs_pos()) {
                    return false;
                }
                filters_.erase(filters_.begin());
                continue;
            } else {
                break;
            }
        }
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
    header.block_start = static_cast<int>(reader.byte_pos()); // after header
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

    const size_t TABLE_SIZE = cur_table_size_; // 430 RAR5 or 446 RAR7 ExtraDist
    const size_t cur_dc = use_extra_dist_ ? 80 : 64;
    size_t pos = 0;
    while (pos < TABLE_SIZE) {
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
            while (n-- > 0 && pos < TABLE_SIZE) {
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
            while (n-- > 0 && pos < TABLE_SIZE) table_[pos++] = 0;
        }
        assert(pos <= TABLE_SIZE && "Table size overrun");
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
    }
    core::uint64 base_at_entry = solid ? file_base_ : 0;
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

    auto block_end_bit = [&](const BlockHeader& h) -> size_t {
        if (h.block_size <= 0) return static_cast<size_t>(h.block_start) * 8;
        return static_cast<size_t>(h.block_start + h.block_size - 1) * 8 +
               static_cast<size_t>(h.block_bit_size);
    };
    size_t end_bit = block_end_bit(header);
    auto cur_bit = [&]() -> size_t {
        return reader.bit_pos();
    };

    size_t total_written = 0;
    engine_.begin_call(); // call-local flush cursor (was a local last_flushed)
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

    while (total_written < dest_size) {
        size_t cb = cur_bit();
        if (cb >= end_bit) {
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
        assert(slot < 306 && "Main LD table slot out of bounds");
        if (slot < 256) {
            engine_.write_literal(slot);
            ++total_written;
            if (total_written - engine_.last_flushed_ >= 65536) {
                if (!engine_.flush_pending(false, base_at_entry, total_written, dest_size,
                                           flush_cb))
                    return false;
            }
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
            // ABSOLUTE file coordinates: carried across per-block calls without
            // frame drift (see abs_pos()/local_region_start() above).
            fe.block_start = static_cast<size_t>(base_at_entry) + total_written + f_start;
            // E8/E8E9/ARM transform base is the offset WITHIN the member: the
            // reference resets it per member even on solid chains
            // (oracle-verified, v1.36.x — a carried base corrupted every
            // non-first solid member with E8-filtered content).
            fe.file_offset =
                base_at_entry + static_cast<core::uint64>(total_written) - member_start_ + f_start;
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
                if (fe.block_start < prev.block_start + prev.block_length) {
                    return false;
                }
            }
            // Queue bound: with disjoint regions enforced, per-region work is
            // bounded by the region length itself, so this only bounds queue
            // memory (65536 entries ~= 2 MiB) — large filtered files emit one
            // region per ~1 MiB and legitimately exceed the old 8192 cap.
            if (engine_.filters_.size() >= 65536) return false;
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
                engine_.copy_match(distance, len, total_written);
                total_written += len;
                if (total_written - engine_.last_flushed_ >= 65536) {
                    if (!engine_.flush_pending(false, base_at_entry, total_written, dest_size,
                                               flush_cb))
                        return false;
                }
            }
            continue;
        }
        if (slot >= 262) {
            core::uint32 len = slot_to_length(reader, slot - 262);
            if (reader.bits_remaining() < 1) return false;
            core::uint32 dist_slot = dd_decoder_.decode(reader);
            assert(dist_slot < (use_extra_dist_ ? 80 : 64) && "Distance slot out of bounds");
            size_t distance = 1;
            core::uint32 d_bits = 0;
            if (dist_slot < 4) {
                distance += dist_slot;
                d_bits = 0;
            } else {
                d_bits = dist_slot / 2 - 1;
                distance += static_cast<size_t>(2 | (dist_slot & 1)) << d_bits;
            }
            if (d_bits > 0) {
                if (d_bits >= 4) {
                    if (d_bits > 4) {
                        if (reader.bits_remaining() < d_bits - 4) return false;
                        core::uint64 extra = 0;
                        if (d_bits > 36) {
                            extra = reader.get_bits64(d_bits - 4);
                        } else {
                            extra = reader.get_bits(d_bits - 4);
                        }
                        distance += static_cast<size_t>(extra) << 4;
                    }
                    if (reader.bits_remaining() < 1) return false;
                    core::uint32 low = ldd_decoder_.decode(reader);
                    distance += low;
                } else {
                    if (reader.bits_remaining() < d_bits) return false;
                    distance += reader.get_bits(d_bits);
                }
            }
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
            // A distance beyond the window cannot be copied; fail the stream
            // instead of silently skipping and desyncing the output
            // accounting (report M6). (The old assert vanished in Release.)
            if (distance == 0 || distance > engine_.win_size_) return false;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            engine_.copy_match(distance, len, total_written);
            total_written += len;
            if (total_written - engine_.last_flushed_ >= 65536) {
                if (!engine_.flush_pending(false, base_at_entry, total_written, dest_size,
                                           flush_cb))
                    return false;
            }
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
            last_length_ = len;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            engine_.copy_match(distance, len, total_written);
            total_written += len;
            if (total_written - engine_.last_flushed_ >= 65536) {
                if (!engine_.flush_pending(false, base_at_entry, total_written, dest_size,
                                           flush_cb))
                    return false;
            }
            continue;
        }
    }

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

    // Same table flavor the applier/worker instances derive from the entry's
    // window size (decompress_internal does this per member).
    use_extra_dist_ = (engine_.win_size_ > (4ULL * 1024 * 1024 * 1024));
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
    auto rec_bytes = [&]() {
        return out.recs.size() * sizeof(OpRecord) + out.lit_pool.size();
    };
    auto close_run = [&]() {
        if (run_count == 0) return;
        OpRecord r;
        r.tag = static_cast<core::uint8>(OpRecord::Tag::Lit);
        r.aux = static_cast<core::uint8>(run_count);
        r.a = 0;
        r.b = static_cast<core::uint32>(run_start);
        out.recs.push_back(r);
        run_count = 0;
        run_start = static_cast<size_t>(-1);
    };
    auto push_lit = [&](core::uint32 slot) {
        if (run_count == 0) run_start = out.lit_pool.size();
        out.lit_pool.push_back(static_cast<core::byte>(slot));
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
            size_t remain = expect.token_bits - reader.bit_pos();
            reader.consume_bits(static_cast<unsigned>(remain > 57 ? 57 : remain));
        }
        if (reader.bit_pos() != expect.token_bits) {
            last_error_ = DecompressErrorCode::InvalidInput;
            last_error_str_ = "span token-bit skip mismatch";
            return false;
        }

        auto block_end_bit = [&](const BlockHeader& h) -> size_t {
            if (h.block_size <= 0) return static_cast<size_t>(h.block_start) * 8;
            return static_cast<size_t>(h.block_start + h.block_size - 1) * 8 +
                   static_cast<size_t>(h.block_bit_size);
        };
        const size_t end_bit = block_end_bit(header);

        while (reader.bit_pos() < end_bit && reader.bits_remaining() >= 1) {
            if (rec_bytes() > rec_cap) {
                last_error_ = DecompressErrorCode::AllocationFailed;
                last_error_str_ = "span record pool cap exceeded (G5)";
                return false;
            }
            core::uint32 slot = ld_decoder_.decode(reader);
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
                continue;
            }
            if (slot >= 262) {
                core::uint32 len = slot_to_length(reader, slot - 262);
                if (reader.bits_remaining() < 1) return false;
                core::uint32 dist_slot = dd_decoder_.decode(reader);
                assert(dist_slot < (use_extra_dist_ ? 80 : 64) && "Distance slot out of bounds");
                size_t distance = 1;
                core::uint32 d_bits = 0;
                if (dist_slot < 4) {
                    distance += dist_slot;
                    d_bits = 0;
                } else {
                    d_bits = dist_slot / 2 - 1;
                    distance += static_cast<size_t>(2 | (dist_slot & 1)) << d_bits;
                }
                if (d_bits > 0) {
                    if (d_bits >= 4) {
                        if (d_bits > 4) {
                            if (reader.bits_remaining() < d_bits - 4) return false;
                            core::uint64 extra = 0;
                            if (d_bits > 36) {
                                extra = reader.get_bits64(d_bits - 4);
                            } else {
                                extra = reader.get_bits(d_bits - 4);
                            }
                            distance += static_cast<size_t>(extra) << 4;
                        }
                        if (reader.bits_remaining() < 1) return false;
                        core::uint32 low = ldd_decoder_.decode(reader);
                        distance += low;
                    } else {
                        if (reader.bits_remaining() < d_bits) return false;
                        distance += reader.get_bits(d_bits);
                    }
                }
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
                r.a = static_cast<core::uint32>(distance);
                r.b = len;
                out.recs.push_back(r);
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
            close_run();
            OpRecord r;
            r.tag = static_cast<core::uint8>(OpRecord::Tag::Rep);
            r.aux = static_cast<core::uint8>(dist_num);
            r.a = len;
            r.b = 0;
            out.recs.push_back(r);
            total_local += len;
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
    engine_.begin_call();
    filters_total_len_ = 0;
    std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
    last_length_ = 0;
    return true;
}

bool Decompressor50::apply_span_records(const SpanRecords& sr, size_t dest_size, bool is_last,
                                        OutputCallback flush_cb, size_t* io_total_written) {
    size_t total_written = *io_total_written;
    // The sequential decoder's per-token flush cadence, verbatim: plain
    // bytes flush at the 64 KiB output cadence and at filter completions
    // (inside engine_.flush_pending). Filter records skip the cadence check
    // exactly like the sequential filter-token branch.
    auto cadence = [&]() -> bool {
        if (total_written - engine_.last_flushed_ < 65536) return true;
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
                if (pending < 65536) {
                    const size_t to_cadence = 65536 - pending;
                    if (to_cadence < stretch) stretch = to_cadence;
                }
                std::memcpy(engine_.window_.data() + engine_.win_pos_, srcp, stretch);
                engine_.win_pos_ = engine_.wrap_up(engine_.win_pos_ + stretch);
                engine_.unp_ptr_ = engine_.wrap_up(engine_.unp_ptr_ + stretch);
                if (engine_.unp_ptr_ == 0) engine_.first_win_done_ = true;
                srcp += stretch;
                total_written += stretch;
                remaining -= stretch;
                if (total_written - engine_.last_flushed_ >= 65536) {
                    if (!engine_.flush_pending(false, 0, total_written, dest_size, flush_cb))
                        return false;
                }
            }
            continue;
        }
        if (tag == OpRecord::Tag::Match) {
            size_t distance = r.a;
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
            engine_.copy_match(distance, len, total_written);
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
            engine_.copy_match(distance, len, total_written);
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
                engine_.copy_match(distance, len, total_written);
                total_written += len;
                if (!cadence()) return false;
            }
            continue;
        }
        // Filter: resolve the raw start delta against the running position
        // [R7] and run the sequential push-site check set, verbatim.
        FilterEntry fe;
        fe.type = static_cast<core::uint8>(r.aux & 7);
        fe.channels = static_cast<core::uint8>(((r.aux >> 3) & 31) + 1);
        fe.block_start = total_written + r.a;
        // Per-member transform base (v1.36.16): the base is the offset
        // WITHIN the member; the applier's frame IS the member frame.
        fe.file_offset = static_cast<core::uint64>(total_written) + r.a;
        fe.block_length = r.b;
        // A filter region larger than the window can never be applied
        // intact (report M5).
        if (static_cast<size_t>(fe.block_length) > engine_.win_size_) return false;
        // Aggregate budget against unbounded scheduled transform work
        // (report M7); dest_size == SIZE_MAX is a sequential-streaming
        // shape that never reaches the applier, kept for symmetry.
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
            if (fe.block_start < prev.block_start + prev.block_length) return false;
        }
        if (engine_.filters_.size() >= 65536) return false;
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
    *io_total_written = total_written;
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
        if (out.size() + size > MAX_STREAM_OUTPUT) return false;
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
