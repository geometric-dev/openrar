#ifndef OPENRAR_COMPRESS_INFLATE_HPP
#define OPENRAR_COMPRESS_INFLATE_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace openrar::compress {

// RFC 1951 DEFLATE decoder (clean-room; spec: docs/spec/11-foreign-formats.md
// §3). Serves the ZIP (method 8) and GZIP readers (v1.29.0).
//
// Trust model (plan D5, normative): NO declared size is ever a bound. The
// decoder enforces a mandatory in-flight output cap; callers wire it to the
// same LimitState debit as every other output path. Declared sizes are
// verified against actual bytes downstream, never the reverse.
//
// Error semantics are strict per RFC 1951: oversubscribed Huffman sets are
// hard errors; incomplete sets are allowed ONLY for the documented distance
// special cases (a single code of length 1, or all-zero = literals-only).
// Invalid bit patterns never coerce to a symbol (unlike the RAR5 decoder,
// which permits sparse codes by design).

enum class InflateError {
    Ok = 0,                 // stream end reached (BFINAL block completed)
    TruncatedInput = 1,     // stream needed bits past end of input
    BadBlockType = 2,       // BTYPE 11 (reserved)
    BadStoredLen = 3,       // stored block NLEN != ~LEN
    BadCodeLengths = 4,     // malformed dynamic-header code-length alphabet
    BadHuffmanCode = 5,     // decode resolved to an invalid bit pattern
    OversubscribedCode = 6, // Kraft sum exceeds the complete-code value
    BadLiteralCode = 7,     // length/distance symbol outside the spec range
    DistanceTooFar = 8,     // match distance exceeds window fill
    OutputCapExceeded = 9,  // in-flight output cap (D5) fired
    SinkFailed = 10         // sink returned false (caller abort/cancel path)
};

std::string inflate_error_string(InflateError code);

class Inflate {
public:
    // Input refill: return the number of bytes read (< requested = EOF).
    using InputFn = std::function<size_t(core::byte* buf, size_t max_size)>;
    // Output sink: return false to abort decode (cancel/limit paths).
    using SinkFn = std::function<bool(const core::byte* data, size_t size)>;

    static constexpr core::uint64 kUnlimited = ~core::uint64(0);

    explicit Inflate(core::uint64 max_inflight_output = kUnlimited);

    // Decode ONE complete DEFLATE stream (runs to the BFINAL block's end).
    // Reusable across streams via reset(). The sink receives 64 KiB chunks
    // (plus a final flush); windowed match copies reach the sink through
    // the same chunks.
    InflateError decode(const InputFn& in, const SinkFn& sink);

    void reset();

    core::uint64 total_out() const { return total_out_; }
    core::uint64 total_in() const { return total_in_; }
    // Bits of the DEFLATE stream actually consumed (excludes buffered-ahead
    // input): the byte after (bits+7)/8 is the stream's exact end — where a
    // trailer (e.g. GZIP CRC32+ISIZE) begins.
    core::uint64 bits_consumed() const { return consumed_bits_; }

private:
    // ---- bit reader (LSB-first for integers; Huffman codes are consumed
    // MSB-first per RFC 1951 §3.1.1 and handled by the table logic) ----
    bool ensure_bits(unsigned int count);      // real bits only; false = EOF
    core::uint32 get_bits(unsigned int count); // count <= 24 (strict)
    void consume(unsigned int count);
    void align_byte();
    bool read_byte(core::byte& out); // byte-agnostic single byte

    // Zero-fill peek (Huffman only): resolves against zeros at EOF; the
    // resolved code length is validated against real availability.
    core::uint32 peek_zero_fill(unsigned int count);
    unsigned int real_bits_available() const { return real_bits_; }

    bool refill_input();
    bool pull_acc_byte();

    // ---- canonical Huffman tables (docs/spec/11 §3) ----
    struct HuffmanTable {
        static constexpr unsigned int ROOT_BITS = 9;
        static constexpr size_t ROOT_SIZE = 1 << ROOT_BITS;
        static constexpr unsigned int MAX_BITS = 15;
        static constexpr size_t MAX_SYMBOLS = 288;

        core::uint16 counts[MAX_BITS + 1]{};
        core::uint16 symbols[MAX_SYMBOLS]{}; // sorted by (length, symbol)
        core::uint32 first_code[MAX_BITS + 1]{};
        core::uint16 first_index[MAX_BITS + 1]{};
        bool complete{false};

        // Root LUT, indexed by the next ROOT_BITS stream bits reversed
        // (first-read bit = MSB of the index): (length << 16) | symbol for
        // codes <= ROOT_BITS; length 0 marks the slow path.
        core::uint32 root[ROOT_SIZE]{};

        bool build(const core::uint8* lengths, size_t n, bool allow_incomplete);
        // Slow path over a 15-bit zero-fill peek (LSB = first-read bit):
        // 0 = invalid bit pattern, else (length << 16) | symbol.
        core::uint32 decode_canonical(core::uint32 peek15) const;
    };

    static core::uint32 reverse_bits(core::uint32 value, unsigned int nbits);

    bool read_dynamic_tables();
    InflateError build_fixed_tables();
    // Resolves one Huffman symbol from `table` into `sym` (consuming its
    // code bits). Returns false only on truncation (checked via truncated_).
    bool next_symbol(const HuffmanTable& table, unsigned int& sym);
    InflateError decode_block_body(const HuffmanTable& lit, const HuffmanTable& dist_tbl);

    InflateError emit(const core::byte* data, size_t size);
    InflateError emit_literal(core::byte b);
    InflateError window_copy(core::uint32 dist, core::uint32 len);
    InflateError flush_output();

    core::uint64 max_out_;
    core::uint64 total_out_{0};
    core::uint64 total_in_{0};

    // input staging (heap-held; the decoder object must stay small enough
    // for member use)
    std::vector<core::byte> in_buf_;
    size_t in_pos_{0};
    size_t in_end_{0};
    bool in_eof_{false};
    core::uint64 bit_acc_{0};
    unsigned int bit_count_{0};
    unsigned int real_bits_{0}; // bits in the accumulator from real input
    core::uint64 consumed_bits_{0};
    bool truncated_{false};

    InputFn input_fn_;
    SinkFn sink_;
    std::vector<core::byte> out_buf_;
    size_t out_len_{0};

    // sliding window (32 KiB ring per RFC 1951)
    static constexpr size_t WINDOW_SIZE = 32768;
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    std::vector<core::byte> window_;
    size_t window_pos_{0}; // next write slot
    size_t window_fill_{0};

    bool have_fixed_{false};
    HuffmanTable fixed_litlen_;
    HuffmanTable fixed_dist_;
    HuffmanTable litlen_;
    HuffmanTable dist_;
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_INFLATE_HPP
