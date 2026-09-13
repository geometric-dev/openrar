#include "decompressor50.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
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

void BitReader::fetch_more() {
    if (!cb_ || p_ < end_) return;
    if (fetched_ >= size_) return;
    size_t to_read = std::min<size_t>(buf_.size(), size_ - fetched_);
    size_t n = cb_(buf_.data(), to_read);
    p_ = buf_.data();
    end_ = p_ + n;
    fetched_ += n;
}

size_t BitReader::bits_remaining() const {
    size_t rem_bytes = (end_ - p_) + (size_ - fetched_);
    return rem_bytes * 8 + n_;
}

void BitReader::refill() {
    if (n_ > 56) return;
    if (p_ >= end_) fetch_more();
    if (p_ >= end_) return;
    unsigned int max_bytes = (64 - n_) / 8;
    unsigned int k = std::min<unsigned int>(max_bytes, static_cast<unsigned int>(end_ - p_));
    if (k == 0) return;
    core::uint64 v = 0;
    std::memcpy(&v, p_, k);
#if defined(_MSC_VER)
    v = _byteswap_uint64(v);
#else
    v = __builtin_bswap64(v);
#endif
    v >>= (64 - 8 * k);
    if (k == 8) {
        acc_ = v;
    } else {
        acc_ = (acc_ << (8 * k)) | v;
    }
    n_ += 8 * k;
    p_ += k;
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
Decompressor50::Decompressor50(size_t win_size)
    : win_size_(win_size), win_mask_(win_size ? win_size - 1 : 0) {
    win_pow2_ = (win_size != 0) && ((win_size & (win_size - 1)) == 0);
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
    if (win_size_ > ALLOC_LIMIT) {
        last_error_ = DecompressErrorCode::DictionaryTooLarge;
        last_error_str_ = "dictionary too large";
    }
    std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
    // INTENDED (capability cap, report Q16): use_extra_dist_ enables the
    // RAR7 446-symbol table whose extra distance slots 68-79 need d_bits up
    // to 38. It can never be true in this build: ALLOC_LIMIT (1 GiB) refuses
    // any window above 1 GiB, far below the 4 GiB threshold here, so the
    // d_bits > 32 decode path in decompress_internal is dead by design.
    // Keep the extra-distance code; the assert at the decode site trips
    // loudly if the alloc limit is ever raised past 4 GiB, since
    // BitReader::get_bits silently truncates reads wider than 32 bits.
    use_extra_dist_ = (win_size_ > (4ULL * 1024 * 1024 * 1024));
    cur_table_size_ = use_extra_dist_ ? 446 : 430;
}

size_t Decompressor50::wrap_up(size_t pos) const {
    if (win_pow2_) return pos & win_mask_;
    return pos >= win_size_ ? pos - win_size_ : pos;
}

void Decompressor50::copy_match(size_t distance, size_t length,
                                [[maybe_unused]] size_t total_written) {
#ifdef OPENRAR_CROSS_VALIDATE
    if (val_src_ != nullptr) {
        if (total_written >= distance && total_written + length <= val_size_) {
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

bool Decompressor50::decompress_internal(BitReader& reader, size_t dest_size, bool solid,
                                         OutputCallback flush_cb, size_t* out_written,
                                         bool* out_finished) {
    if (out_written) *out_written = 0;
    if (out_finished) *out_finished = false;

    if (last_error_ != DecompressErrorCode::Ok) return false;

    if (win_size_ > 0 && !window_ready_) {
        try {
            window_.assign(win_size_, 0);
            window_ready_ = true;
        } catch (...) {
            last_error_ = DecompressErrorCode::AllocationFailed;
            last_error_str_ = "dictionary too large: allocation failed";
            window_.clear();
            return false;
        }
    }

    // Filter queue is per-file: a filter region never spans a file boundary,
    // so always start with an empty queue even when the LZ state carries over.
    filters_.clear();
    core::uint64 base_at_entry = solid ? file_base_ : 0;
    if (!solid) {
        file_base_ = 0;
        base_at_entry = 0;
        if (window_ready_) std::fill(window_.begin(), window_.end(), static_cast<core::byte>(0));
        win_pos_ = 0;
        unp_ptr_ = 0;
        first_win_done_ = false;
        std::fill(std::begin(old_dist_), std::end(old_dist_), static_cast<size_t>(-1));
        std::fill(std::begin(table_), std::end(table_), static_cast<core::byte>(0));
        last_length_ = 0;
        tables_ready_ = false;
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
    size_t last_flushed = 0;
    // Aggregate filter budget for this decode (report M7).
    size_t filters_total_len = 0;

    auto apply_filter_circular = [&](const FilterEntry& f) -> bool {
        size_t len = f.block_length;
        if (len == 0) return true;
        // The region's start byte must still be inside the window when the
        // filter runs. total_written - f.block_start bytes have been written
        // since the region began; if that reaches win_size_ the start has
        // been overwritten and the transform would silently operate on
        // garbage (the cross-window case the dead next_window flag once
        // tried to paper over). Such streams are corrupt: fail the decode.
        size_t back = (total_written - f.block_start) % win_size_;
        if (total_written - f.block_start >= win_size_) return false;
        std::vector<core::byte> buf(len);
        // Start position in circular buffer: `back` is reduced modulo the
        // window first, so the arithmetic below stays correct for
        // non-power-of-two window sizes too (a raw unp_ptr_ + win_size_ -
        // back underflows whenever back > unp_ptr_ and only pow2 windows
        // absorb that in the final modulo).
        size_t circ_start = (unp_ptr_ + win_size_ - back) % win_size_;

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
            // Undefined filter types 4-7: refuse rather than silently write
            // back a zero-filled region and destroy the decoded data.
            return false;
        }

        cur = circ_start;
        for (size_t i = 0; i < len; ++i) {
            window_[cur] = out_buf[i];
            cur++;
            if (cur == win_size_) cur = 0;
        }
        return true;
    };

    auto flush_pending_blocks = [&](bool flush_all) -> bool {
        if (!flush_cb) return true;

        while (!filters_.empty()) {
            const auto& f = filters_.front();
            if (f.block_start + f.block_length <= total_written) {
                if (!apply_filter_circular(f)) return false;
                filters_.erase(filters_.begin());
            } else {
                if (flush_all) {
                    if (f.block_start < total_written) {
                        return false;
                    }
                    filters_.erase(filters_.begin());
                    continue;
                } else {
                    break;
                }
            }
        }

        size_t safe_limit = total_written;
        if (!flush_all) {
            for (const auto& f : filters_) {
                if (f.block_start < safe_limit) {
                    safe_limit = f.block_start;
                }
            }
        }

        if (safe_limit > dest_size) safe_limit = dest_size;

        while (last_flushed < safe_limit) {
            size_t chunk = safe_limit - last_flushed;
            // Reduce the backward offset modulo the window first (same
            // non-power-of-two rationale as in apply_filter_circular).
            size_t back = (total_written - last_flushed) % win_size_;
            size_t circ_start = (unp_ptr_ + win_size_ - back) % win_size_;
            size_t max_contig = win_size_ - circ_start;
            if (chunk > max_contig) chunk = max_contig;

            if (!flush_cb(&window_[circ_start], chunk)) {
                return false;
            }
            last_flushed += chunk;
        }
        return true;
    };

    while (total_written < dest_size) {
        size_t cb = cur_bit();
        if (cb >= end_bit) {
            if (header.last_block_in_file) break;
            if (!read_block_header(reader, header)) break;
            if (!read_tables(reader, header)) return false;
            if (!header.table_present && !tables_ready_) return false;
            end_bit = block_end_bit(header);
            continue;
        }
        if (reader.bits_remaining() < 1) {
            if (header.last_block_in_file) break;
            if (!read_block_header(reader, header)) break;
            if (!read_tables(reader, header)) return false;
            end_bit = block_end_bit(header);
            continue;
        }

        core::uint32 slot = ld_decoder_.decode(reader);
        assert(slot < 306 && "Main LD table slot out of bounds");
        if (slot < 256) {
            window_[win_pos_] = static_cast<core::byte>(slot);
            win_pos_ = wrap_up(win_pos_ + 1);
            unp_ptr_ = wrap_up(unp_ptr_ + 1);
            if (unp_ptr_ == 0) first_win_done_ = true;
            ++total_written;
            if (total_written - last_flushed >= 65536) {
                if (!flush_pending_blocks(false)) return false;
            }
            continue;
        }
        if (slot == 256) {
            if (reader.bits_remaining() < 2) return false;
            auto read_var = [&](bool& ok) -> core::uint32 {
                core::uint32 v = 0;
                unsigned int shift = 0;
                while (reader.bits_remaining() >= 8) {
                    core::uint32 b = reader.get_bits(8);
                    if (shift < 32) v |= (b & 0x7F) << shift;
                    shift += 7;
                    if ((b & 0x80) == 0) return v;
                }
                ok = false;
                return 0;
            };
            bool ok = true;
            core::uint32 f_start = read_var(ok);
            core::uint32 f_len = read_var(ok);
            if (!ok) return false;
            // RAR5 spec ceilings for filter parameters. Values beyond these
            // are malformed; anything at or below them is accepted so valid
            // archives with large filter regions still decode.
            if (f_start > 0x400000 || f_len > 0x400000) return false;
            core::uint32 f_type = reader.get_bits(3);
            core::uint32 f_ch = 1;
            if (f_type == 0) {
                if (reader.bits_remaining() < 5) return false;
                f_ch = reader.get_bits(5) + 1;
            }
            FilterEntry fe;
            fe.type = static_cast<core::uint8>(f_type);
            fe.channels = static_cast<core::uint8>(f_ch);
            fe.block_start = total_written + static_cast<size_t>(f_start);
            fe.file_offset = base_at_entry + static_cast<core::uint64>(total_written) + f_start;
            fe.block_length = f_len;
            // A filter region larger than the window can never be applied
            // intact: by the time the region ends, its start has been
            // overwritten in the circular buffer, in every implementation
            // (window accounting is enforced again at apply time). Regions
            // beyond that are malformed for this dictionary (report M5).
            if (static_cast<size_t>(f_len) > win_size_) return false;
            // Aggregate budget: every output byte may belong to a handful of
            // overlapping filters, but not to thousands of full-window ones.
            // Without this, tens of KB of crafted stream schedule ~64 GB of
            // transform work at flush time (report M7).
            filters_total_len += static_cast<size_t>(f_len);
            if (filters_total_len > dest_size + win_size_) return false;
            if (filters_.size() >= 8192) return false;
            filters_.push_back(fe);
            continue;
        }
        if (slot == 257) {
            if (last_length_ != 0) {
                size_t distance = old_dist_[0];
                // Repeat distances are attacker-controlled state: the
                // sentinel (never set) or an out-of-window value means the
                // stream is corrupt. copy_match would silently skip the
                // copy while callers still advanced total_written,
                // desyncing output accounting from the window (report M6).
                if (distance == static_cast<size_t>(-1) || distance == 0 || distance > win_size_)
                    return false;
                core::uint32 len = static_cast<core::uint32>(last_length_);
                if (len > dest_size - total_written)
                    len = static_cast<core::uint32>(dest_size - total_written);
                copy_match(distance, len, total_written);
                total_written += len;
                if (total_written - last_flushed >= 65536) {
                    if (!flush_pending_blocks(false)) return false;
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
                        // Unreachable while ALLOC_LIMIT caps windows at 1 GiB:
                        // extra-distance slots need win_size > 4 GiB (see the
                        // use_extra_dist_ note in the constructor, report Q16).
                        // d_bits > 36 would make get_bits(d_bits - 4) read
                        // wider than 32 bits and silently truncate. Debug
                        // trip-wire for anyone raising the alloc limit.
                        assert(d_bits <= 32 &&
                               "extra-distance decode reached: ALLOC_LIMIT no longer caps windows "
                               "below 4 GiB; get_bits would truncate >32-bit reads");
                        if (d_bits > 36) {
                            core::uint32 extra =
                                static_cast<core::uint32>(reader.get_bits(d_bits - 4));
                            distance += static_cast<size_t>(extra) << 4;
                        } else {
                            core::uint32 extra = reader.get_bits(d_bits - 4);
                            distance += static_cast<size_t>(extra) << 4;
                        }
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
            if (distance == 0 || distance > win_size_) return false;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            copy_match(distance, len, total_written);
            total_written += len;
            if (total_written - last_flushed >= 65536) {
                if (!flush_pending_blocks(false)) return false;
            }
            continue;
        }
        if (slot < 262) {
            core::uint32 dist_num = slot - 258;
            size_t distance = old_dist_[dist_num];
            // Same corrupt-stream check as slot 257 (report M6).
            if (distance == static_cast<size_t>(-1) || distance == 0 || distance > win_size_)
                return false;
            for (core::uint32 i = dist_num; i > 0; --i) old_dist_[i] = old_dist_[i - 1];
            old_dist_[0] = distance;
            if (reader.bits_remaining() < 1) return false;
            core::uint32 len_slot = rd_decoder_.decode(reader);
            core::uint32 len = slot_to_length(reader, len_slot);
            last_length_ = len;
            if (len > dest_size - total_written)
                len = static_cast<core::uint32>(dest_size - total_written);
            copy_match(distance, len, total_written);
            total_written += len;
            if (total_written - last_flushed >= 65536) {
                if (!flush_pending_blocks(false)) return false;
            }
            continue;
        }
    }

    if (!flush_pending_blocks(true)) return false;

    // Truncation check: if the stream ended without a LastBlock flag before
    // producing dest_size bytes, the payload is incomplete. Reporting success
    // here would let callers write silently truncated output, so fail instead.
    // (dest_size may be SIZE_MAX when the caller streams unknown-length output;
    // in that case a genuine stream end is only ever reached on LastBlock.)
    if (total_written < dest_size && !header.last_block_in_file) return false;

    if (out_written) *out_written = total_written;
    if (out_finished) *out_finished = header.last_block_in_file;
    file_base_ = base_at_entry + static_cast<core::uint64>(total_written);
    return true;
}

bool Decompressor50::decompress_to_vector(const core::byte* src, size_t src_size,
                                          std::vector<core::byte>& out, bool solid) {
    out.clear();
    if (src == nullptr || src_size == 0) return true;

    size_t cap = std::max<size_t>(64 * 1024, src_size * 8);
    if (cap > MAX_STREAM_OUTPUT) cap = MAX_STREAM_OUTPUT;
    out.reserve(cap);

    auto cb = [&](const core::byte* data, size_t size) -> bool {
        if (out.size() + size > MAX_STREAM_OUTPUT) return false;
        out.insert(out.end(), data, data + size);
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
