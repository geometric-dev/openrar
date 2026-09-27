#include "inflate.hpp"

#include <cstring>

namespace openrar::compress {

namespace {

// RFC 1951 §3.2.5 length codes 257..285: base length and extra bits.
struct LengthEntry {
    core::uint16 base;
    core::uint8 extra;
};
constexpr LengthEntry kLengthTable[29] = {
    {3, 0},  {4, 0},  {5, 0},  {6, 0},   {7, 0},   {8, 0},   {9, 0},   {10, 0},  {11, 1}, {13, 1},
    {15, 1}, {17, 1}, {19, 2}, {23, 2},  {27, 2},  {31, 2},  {35, 3},  {43, 3},  {51, 3}, {59, 3},
    {67, 4}, {83, 4}, {99, 4}, {115, 4}, {131, 5}, {163, 5}, {195, 5}, {227, 5}, {258, 0}};

// RFC 1951 §3.2.5 distance codes 0..29: base distance and extra bits.
constexpr LengthEntry kDistTable[30] = {
    {1, 0},     {2, 0},     {3, 0},     {4, 0},      {5, 1},      {7, 1},
    {9, 2},     {13, 2},    {17, 3},    {25, 3},     {33, 4},     {49, 4},
    {65, 5},    {97, 5},    {129, 6},   {193, 6},    {257, 7},    {385, 7},
    {513, 8},   {769, 8},   {1025, 9},  {1537, 9},   {2049, 10},  {3073, 10},
    {4097, 11}, {6145, 11}, {8193, 12}, {12289, 12}, {16385, 13}, {24577, 13}};

// RFC 1951 §3.2.6 code-length alphabet transmit order.
constexpr core::uint8 kCodeLengthOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                              11, 4,  12, 3, 13, 2, 14, 1, 15};

constexpr size_t kInChunk = 65536;
constexpr size_t kOutChunk = 65536;
constexpr core::uint32 kStoredChunk = 4096;

} // namespace

std::string inflate_error_string(InflateError code) {
    switch (code) {
    case InflateError::Ok:
        return "ok";
    case InflateError::TruncatedInput:
        return "truncated deflate stream";
    case InflateError::BadBlockType:
        return "reserved deflate block type";
    case InflateError::BadStoredLen:
        return "stored block length complement mismatch";
    case InflateError::BadCodeLengths:
        return "malformed deflate code-length header";
    case InflateError::BadHuffmanCode:
        return "invalid deflate Huffman bit pattern";
    case InflateError::OversubscribedCode:
        return "oversubscribed deflate Huffman code";
    case InflateError::BadLiteralCode:
        return "deflate symbol outside spec range";
    case InflateError::DistanceTooFar:
        return "deflate distance beyond window";
    case InflateError::OutputCapExceeded:
        return "in-flight output cap exceeded";
    case InflateError::SinkFailed:
        return "output sink aborted decode";
    }
    return "unknown deflate error";
}

// ------------------------------------------------------------------
// HuffmanTable
// ------------------------------------------------------------------

bool Inflate::HuffmanTable::build(const core::uint8* lengths, size_t n, bool allow_incomplete) {
    std::memset(counts, 0, sizeof(counts));
    std::memset(root, 0, sizeof(root));
    std::memset(first_code, 0, sizeof(first_code));
    std::memset(first_index, 0, sizeof(first_index));
    complete = false;
    if (n > MAX_SYMBOLS) return false;

    core::uint32 sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (lengths[i] > MAX_BITS) return false;
        ++counts[lengths[i]];
        if (lengths[i] > 0) sum += core::uint32(1) << (MAX_BITS - lengths[i]);
    }
    if (sum > (core::uint32(1) << MAX_BITS)) return false; // oversubscribed
    complete = (sum == (core::uint32(1) << MAX_BITS));
    if (!complete && !allow_incomplete) return false;

    // Canonical first code / first symbol index per length (the procedure
    // stated in RFC 1951 §3.2.2 itself).
    core::uint32 code = 0;
    core::uint16 idx = 0;
    for (unsigned int len = 1; len <= MAX_BITS; ++len) {
        first_code[len] = code;
        first_index[len] = idx;
        code = (code + counts[len]) << 1;
        idx = static_cast<core::uint16>(idx + counts[len]);
    }

    // Symbols sorted by (length, symbol).
    core::uint16 next[MAX_BITS + 1];
    std::memcpy(next, first_index, sizeof(next));
    for (size_t sym = 0; sym < n; ++sym) {
        if (lengths[sym] > 0) symbols[next[lengths[sym]]++] = static_cast<core::uint16>(sym);
    }

    // Root LUT for codes that fit ROOT_BITS. The canonical code's MSB is
    // the first bit read; the LUT index stores the first-read bit in its
    // MSB, so the base index is code << (ROOT_BITS - length).
    core::uint32 next_code[MAX_BITS + 1];
    std::memcpy(next_code, first_code, sizeof(next_code));
    for (size_t sym = 0; sym < n; ++sym) {
        unsigned int len = lengths[sym];
        if (len == 0 || len > ROOT_BITS) continue;
        core::uint32 base = next_code[len]++ << (ROOT_BITS - len);
        core::uint32 span = core::uint32(1) << (ROOT_BITS - len);
        core::uint32 entry = (core::uint32(len) << 16) | core::uint32(sym);
        for (core::uint32 i = 0; i < span; ++i) root[base + i] = entry;
    }
    return true;
}

core::uint32 Inflate::HuffmanTable::decode_canonical(core::uint32 peek15) const {
    core::uint32 code = 0;
    for (unsigned int len = 1; len <= MAX_BITS; ++len) {
        code = (code << 1) | ((peek15 >> (len - 1)) & 1);
        if (code >= first_code[len] && code - first_code[len] < counts[len]) {
            return (core::uint32(len) << 16) |
                   core::uint32(symbols[first_index[len] + (code - first_code[len])]);
        }
    }
    return 0; // invalid bit pattern (or incomplete-code miss)
}

// ------------------------------------------------------------------
// Bit reader
// ------------------------------------------------------------------

Inflate::Inflate(core::uint64 max_inflight_output)
    : max_out_(max_inflight_output), in_buf_(kInChunk), out_buf_(kOutChunk), window_(WINDOW_SIZE) {}

void Inflate::reset() {
    in_pos_ = in_end_ = 0;
    in_eof_ = false;
    bit_acc_ = 0;
    bit_count_ = 0;
    real_bits_ = 0;
    consumed_bits_ = 0;
    truncated_ = false;
    total_out_ = 0;
    total_in_ = 0;
    out_len_ = 0;
    window_pos_ = 0;
    window_fill_ = 0;
    have_fixed_ = false;
}

bool Inflate::refill_input() {
    if (in_eof_ || !input_fn_) return false;
    size_t got = input_fn_(in_buf_.data(), in_buf_.size());
    total_in_ += got;
    in_pos_ = 0;
    in_end_ = got;
    if (got < in_buf_.size()) in_eof_ = true;
    return got > 0;
}

bool Inflate::pull_acc_byte() {
    if (in_pos_ >= in_end_ && !refill_input()) return false;
    bit_acc_ |= core::uint64(in_buf_[in_pos_++]) << bit_count_;
    bit_count_ += 8;
    real_bits_ += 8;
    return true;
}

bool Inflate::ensure_bits(unsigned int count) {
    while (real_bits_ < count) {
        if (!pull_acc_byte()) return false;
    }
    return true;
}

core::uint32 Inflate::peek_zero_fill(unsigned int count) {
    // pull_acc_byte/refill_input terminate on true EOF; in_eof_ only marks
    // the last short read and says nothing about buffered bytes.
    while (bit_count_ < count) {
        if (!pull_acc_byte()) break;
    }
    if (bit_count_ >= count)
        return static_cast<core::uint32>(bit_acc_ & ((core::uint64(1) << count) - 1));
    // Past real input: the accumulator high bits are zero by construction.
    return static_cast<core::uint32>(bit_acc_);
}

core::uint32 Inflate::get_bits(unsigned int count) {
    if (!ensure_bits(count)) {
        truncated_ = true;
        return 0;
    }
    core::uint32 val = static_cast<core::uint32>(bit_acc_ & ((core::uint64(1) << count) - 1));
    consume(count);
    return val;
}

void Inflate::consume(unsigned int count) {
    bit_acc_ >>= count;
    bit_count_ -= count;
    real_bits_ -= count;
    consumed_bits_ += count;
}

void Inflate::align_byte() {
    // Discard up to the next byte boundary: (8 - consumed % 8) % 8 bits.
    const unsigned int rem = static_cast<unsigned int>((8 - (consumed_bits_ % 8)) % 8);
    if (rem == 0) return;
    // Mid-stream these are real bits.
    if (!ensure_bits(rem)) {
        truncated_ = true;
        return;
    }
    consume(rem);
}

bool Inflate::read_byte(core::byte& out) {
    if (!ensure_bits(8)) return false;
    out = static_cast<core::byte>(get_bits(8));
    return true;
}

core::uint32 Inflate::reverse_bits(core::uint32 value, unsigned int nbits) {
    core::uint32 out = 0;
    for (unsigned int i = 0; i < nbits; ++i) {
        out = (out << 1) | ((value >> i) & 1);
    }
    return out;
}

// ------------------------------------------------------------------
// Tables
// ------------------------------------------------------------------

InflateError Inflate::build_fixed_tables() {
    if (have_fixed_) return InflateError::Ok;
    core::uint8 lengths[288];
    unsigned int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    if (!fixed_litlen_.build(lengths, 288, false)) return InflateError::OversubscribedCode;
    core::uint8 dlengths[32];
    for (i = 0; i < 32; ++i) dlengths[i] = 5;
    if (!fixed_dist_.build(dlengths, 32, false)) return InflateError::OversubscribedCode;
    have_fixed_ = true;
    return InflateError::Ok;
}

bool Inflate::read_dynamic_tables() {
    const unsigned int hlit = get_bits(5) + 257;
    if (truncated_) return false;
    const unsigned int hdist = get_bits(5) + 1;
    if (truncated_) return false;
    const unsigned int hclen = get_bits(4) + 4;
    if (truncated_) return false;
    if (hlit > 286 || hdist > 30) return false;

    core::uint8 clen_lengths[19] = {0};
    for (unsigned int i = 0; i < hclen; ++i) {
        clen_lengths[kCodeLengthOrder[i]] = static_cast<core::uint8>(get_bits(3));
        if (truncated_) return false;
    }
    HuffmanTable clen;
    if (!clen.build(clen_lengths, 19, false)) return false;

    core::uint8 all_lengths[286 + 30] = {0};
    const unsigned int total = hlit + hdist;
    unsigned int prev = 0;
    unsigned int idx = 0;
    while (idx < total) {
        core::uint32 peek = peek_zero_fill(HuffmanTable::ROOT_BITS);
        core::uint32 rev = reverse_bits(peek, HuffmanTable::ROOT_BITS);
        core::uint32 entry = clen.root[rev];
        if ((entry >> 16) == 0) {
            entry = clen.decode_canonical(peek_zero_fill(HuffmanTable::MAX_BITS));
            if (entry == 0) return false;
        }
        unsigned int len = entry >> 16;
        if (len > real_bits_available()) return false;
        consume(len);
        unsigned int sym = entry & 0xFFFF;

        if (sym < 16) {
            prev = sym;
            all_lengths[idx++] = static_cast<core::uint8>(sym);
        } else if (sym == 16) {
            if (idx == 0) return false;
            unsigned int rep = 3 + get_bits(2);
            if (truncated_) return false;
            if (idx + rep > total) return false;
            for (unsigned int r = 0; r < rep; ++r)
                all_lengths[idx++] = static_cast<core::uint8>(prev);
        } else if (sym == 17) {
            unsigned int rep = 3 + get_bits(3);
            if (truncated_) return false;
            if (idx + rep > total) return false;
            idx += rep;
            prev = 0;
        } else { // 18
            unsigned int rep = 11 + get_bits(7);
            if (truncated_) return false;
            if (idx + rep > total) return false;
            idx += rep;
            prev = 0;
        }
    }

    if (!litlen_.build(all_lengths, hlit, false)) return false;
    if (!dist_.build(all_lengths + hlit, hdist, true)) return false;
    // Distance special cases (RFC 1951 §3.2.7): incomplete is legal ONLY
    // as a single code of length 1, or an all-zero (literals-only) table.
    if (!dist_.complete) {
        bool single_len1 = (dist_.counts[1] == 1);
        bool all_zero = true;
        for (unsigned int l = 1; l <= HuffmanTable::MAX_BITS; ++l) {
            if (dist_.counts[l] != 0) all_zero = false;
        }
        if (!single_len1 && !all_zero) return false;
    }
    return true;
}

// ------------------------------------------------------------------
// Output
// ------------------------------------------------------------------

InflateError Inflate::flush_output() {
    if (out_len_ == 0) return InflateError::Ok;
    if (total_out_ + out_len_ > max_out_) {
        // Emit up to the cap, then report the breach (D5: the cap is a hard
        // in-flight bound; partial output is the caller's discard).
        const core::uint64 room = max_out_ - total_out_;
        const size_t take = static_cast<size_t>(room < out_len_ ? room : out_len_);
        if (take > 0) {
            if (sink_ && !sink_(out_buf_.data(), take)) return InflateError::SinkFailed;
            total_out_ += take;
        }
        return InflateError::OutputCapExceeded;
    }
    if (sink_ && !sink_(out_buf_.data(), out_len_)) return InflateError::SinkFailed;
    total_out_ += out_len_;
    out_len_ = 0;
    return InflateError::Ok;
}

InflateError Inflate::emit(const core::byte* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        InflateError err = emit_literal(data[i]);
        if (err != InflateError::Ok) return err;
    }
    return InflateError::Ok;
}

InflateError Inflate::emit_literal(core::byte b) {
    out_buf_[out_len_++] = b;
    window_[window_pos_] = b;
    window_pos_ = (window_pos_ + 1) & WINDOW_MASK;
    if (window_fill_ < WINDOW_SIZE) ++window_fill_;
    if (out_len_ == out_buf_.size()) return flush_output();
    return InflateError::Ok;
}

InflateError Inflate::window_copy(core::uint32 dist, core::uint32 len) {
    if (dist > window_fill_) return InflateError::DistanceTooFar;
    for (core::uint32 r = 0; r < len; ++r) {
        const size_t src = (window_pos_ + WINDOW_SIZE - dist) & WINDOW_MASK;
        const core::byte b = window_[src];
        out_buf_[out_len_++] = b;
        window_[window_pos_] = b;
        window_pos_ = (window_pos_ + 1) & WINDOW_MASK;
        if (window_fill_ < WINDOW_SIZE) ++window_fill_;
        if (out_len_ == out_buf_.size()) {
            const InflateError err = flush_output();
            if (err != InflateError::Ok) return err;
        }
    }
    return InflateError::Ok;
}

// ------------------------------------------------------------------
// Decode
// ------------------------------------------------------------------

bool Inflate::next_symbol(const HuffmanTable& table, unsigned int& sym) {
    const core::uint32 peek = peek_zero_fill(HuffmanTable::ROOT_BITS);
    const core::uint32 idx = reverse_bits(peek, HuffmanTable::ROOT_BITS);
    core::uint32 entry = table.root[idx];
    if ((entry >> 16) == 0) {
        entry = table.decode_canonical(peek_zero_fill(HuffmanTable::MAX_BITS));
        if (entry == 0) return false;
    }
    const unsigned int len = entry >> 16;
    if (len > real_bits_available()) {
        truncated_ = true;
        return false;
    }
    consume(len);
    sym = entry & 0xFFFF;
    return true;
}

InflateError Inflate::decode(const InputFn& in, const SinkFn& sink) {
    reset();
    input_fn_ = in;
    sink_ = sink;

    for (;;) {
        const core::uint32 bfinal = get_bits(1);
        if (truncated_) return InflateError::TruncatedInput;
        const core::uint32 btype = get_bits(2);
        if (truncated_) return InflateError::TruncatedInput;

        if (btype == 0) {
            align_byte();
            if (truncated_) return InflateError::TruncatedInput;
            const core::uint32 len = get_bits(16);
            const core::uint32 nlen = get_bits(16);
            if (truncated_) return InflateError::TruncatedInput;
            if ((len ^ 0xFFFF) != nlen) return InflateError::BadStoredLen;
            core::byte scratch[kStoredChunk];
            core::uint32 left = len;
            while (left > 0) {
                const core::uint32 take = left < kStoredChunk ? left : kStoredChunk;
                for (core::uint32 r = 0; r < take; ++r) {
                    core::byte b = 0;
                    if (!read_byte(b)) return InflateError::TruncatedInput;
                    scratch[r] = b;
                }
                const InflateError err = emit(scratch, take);
                if (err != InflateError::Ok) return err;
                left -= take;
            }
        } else if (btype == 1) {
            const InflateError ferr = build_fixed_tables();
            if (ferr != InflateError::Ok) return ferr;
            const InflateError berr = decode_block_body(fixed_litlen_, fixed_dist_);
            if (berr != InflateError::Ok) return berr;
        } else if (btype == 2) {
            if (!read_dynamic_tables()) {
                return truncated_ ? InflateError::TruncatedInput : InflateError::BadCodeLengths;
            }
            const InflateError berr = decode_block_body(litlen_, dist_);
            if (berr != InflateError::Ok) return berr;
        } else {
            return InflateError::BadBlockType;
        }

        if (bfinal) break;
    }

    return flush_output();
}

InflateError Inflate::decode_block_body(const HuffmanTable& lit, const HuffmanTable& dist_tbl) {
    for (;;) {
        unsigned int sym = 0;
        if (!next_symbol(lit, sym))
            return truncated_ ? InflateError::TruncatedInput : InflateError::BadHuffmanCode;

        if (sym < 256) {
            const InflateError err = emit_literal(static_cast<core::byte>(sym));
            if (err != InflateError::Ok) return err;
        } else if (sym == 256) {
            return InflateError::Ok; // end of block
        } else if (sym <= 285) {
            const LengthEntry& le = kLengthTable[sym - 257];
            core::uint32 mlen = le.base;
            if (le.extra) {
                mlen += get_bits(le.extra);
                if (truncated_) return InflateError::TruncatedInput;
            }
            unsigned int dsym = 0;
            if (!next_symbol(dist_tbl, dsym))
                return truncated_ ? InflateError::TruncatedInput : InflateError::BadHuffmanCode;
            if (dsym > 29) return InflateError::BadLiteralCode;
            const LengthEntry& dt = kDistTable[dsym];
            core::uint32 dist = dt.base;
            if (dt.extra) {
                dist += get_bits(dt.extra);
                if (truncated_) return InflateError::TruncatedInput;
            }
            const InflateError err = window_copy(dist, mlen);
            if (err != InflateError::Ok) return err;
        } else {
            return InflateError::BadLiteralCode; // 286/287 never occur
        }
    }
}

} // namespace openrar::compress
