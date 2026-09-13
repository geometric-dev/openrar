#ifndef OPENRAR_COMPRESS_COMPRESSOR50_HPP
#define OPENRAR_COMPRESS_COMPRESSOR50_HPP

#include "../core/types.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/blake2sp.hpp"
#include "../io/file_stream.hpp"

#include <vector>
#include <cstring>
#include <algorithm>
#include <cassert>

namespace openrar::compress {

class BitOutput {
private:
    static const size_t OUTBUF_SIZE = 0x40000;

    std::vector<core::byte> buf_;
    io::FileStream* dest_file_;
    std::vector<core::byte>* mem_dest_;

    core::uint64 block_bytes_{0};
    core::uint64 block_bits_{0};
    core::uint64 acc_{0};
    core::uint32 acc_bits_{0};
    // Set when a flush to dest_file_ wrote fewer bytes than requested; the
    // compressor checks this instead of reporting success on a short write
    // (sweep finding M5).
    bool write_failed_{false};

public:
    BitOutput()
        : dest_file_(nullptr), mem_dest_(nullptr), block_bytes_(0), block_bits_(0), acc_(0),
          acc_bits_(0) {
        buf_.reserve(OUTBUF_SIZE + 16);
    }

    void set_dest_file(io::FileStream* dest) { dest_file_ = dest; }
    void set_memory(std::vector<core::byte>* mem) { mem_dest_ = mem; }

    core::uint64 get_block_bytes() const { return block_bytes_ + buf_.size(); }
    core::uint64 get_block_bits() const { return block_bits_; }
    bool failed() const { return write_failed_; }

    inline void put_bits(core::uint32 value, core::uint32 num_bits) {
        assert(num_bits <= 32);
        if (num_bits == 0) return;
        block_bits_ += num_bits;
        acc_ = (acc_ << num_bits) |
               (num_bits >= 32 ? value : (value & ((core::uint32(1) << num_bits) - 1)));
        acc_bits_ += num_bits;
        assert(acc_bits_ <= 64);
        if (acc_bits_ >= 32) {
            core::uint32 val = static_cast<core::uint32>(acc_ >> (acc_bits_ - 32));
#if defined(_MSC_VER)
            val = _byteswap_ulong(val);
#else
            val = __builtin_bswap32(val);
#endif
            size_t sz = buf_.size();
            if (sz + 4 > buf_.capacity()) buf_.reserve(sz + OUTBUF_SIZE);
            buf_.resize(sz + 4);
            std::memcpy(&buf_[sz], &val, 4);
            acc_bits_ -= 32;
            if (buf_.size() >= OUTBUF_SIZE) flush_buf();
        }
    }

    void flush_buf() {
        if (buf_.empty()) return;
        if (mem_dest_ != nullptr) {
            block_bytes_ += buf_.size();
            mem_dest_->insert(mem_dest_->end(), buf_.begin(), buf_.end());
            buf_.clear();
            return;
        }
        if (dest_file_ != nullptr) {
            size_t wrote = dest_file_->write(buf_.data(), buf_.size());
            if (wrote != buf_.size()) write_failed_ = true;
            block_bytes_ += wrote;
        }
        buf_.clear();
    }

    void align_byte() {
        while (acc_bits_ >= 8) {
            buf_.push_back(static_cast<core::byte>(acc_ >> (acc_bits_ - 8)));
            acc_bits_ -= 8;
        }
        if (acc_bits_ > 0) {
            core::byte partial = static_cast<core::byte>(acc_ << (8 - acc_bits_));
            buf_.push_back(partial);
            acc_ = 0;
            acc_bits_ = 0;
        }
        if (buf_.size() >= OUTBUF_SIZE) flush_buf();
    }

    void reset_block_count() {
        flush_buf();
        block_bytes_ = 0;
        block_bits_ = 0;
    }

    inline void put_bits64(core::uint64 value, core::uint32 num_bits) {
        // MSB-first: emit the high part, then the low 32 bits.
        if (num_bits <= 32) {
            put_bits(static_cast<core::uint32>(value), num_bits);
            return;
        }
        put_bits(static_cast<core::uint32>(value >> (num_bits - 32)), num_bits - 32);
        put_bits(static_cast<core::uint32>(value & 0xFFFFFFFFu), 32);
    }

    void finish_block_data() {
        align_byte();
        flush_buf();
    }
};

struct Compressor50Token {
    enum class TokenType : core::byte {
        Literal = 0,
        Match = 1,
        Rep0 = 2,
        Rep1 = 3,
        Rep2 = 4,
        Rep3 = 5
    };
    // uint64: RAR7 extra-distance slots need up to 38 raw bits for distances
    // inside windows above 16 GiB; a 32-bit field silently truncated the top
    // bits and emitted a wrong distance (sweep finding H1).
    core::uint64 dist_extra{0};
    core::uint16 len_extra{0};
    core::uint16 packed{0}; // type(3) | len_slot/data(6/8) | dist_slot(6)

    inline TokenType get_type() const { return static_cast<TokenType>(packed & 7); }
    inline void set_type(TokenType t) { packed = (packed & ~7) | static_cast<core::uint16>(t); }

    inline core::byte get_len_slot() const { return static_cast<core::byte>((packed >> 3) & 0x3F); }
    inline void set_len_slot(core::byte s) {
        packed = (packed & ~(0x3F << 3)) | (static_cast<core::uint16>(s) << 3);
    }

    inline core::byte get_dist_slot() const {
        return static_cast<core::byte>((packed >> 9) & 0x3F);
    }
    inline void set_dist_slot(core::byte s) {
        packed = (packed & ~(0x3F << 9)) | (static_cast<core::uint16>(s) << 9);
    }

    inline core::byte get_data() const { return static_cast<core::byte>(packed >> 3); }
    inline void set_data(core::byte d) {
        packed = (packed & 7) | (static_cast<core::uint16>(d) << 3);
    }

    inline core::byte get_len_bits() const {
        core::byte s = get_len_slot();
        return s < 8 ? 0 : (s / 4 - 1);
    }

    inline core::byte get_dist_bits() const {
        core::byte s = get_dist_slot();
        return s < 4 ? 0 : (s / 2 - 1);
    }
};

struct HuffmanBuilder {
    static void build_lengths(const core::uint32* freq, core::uint32 size, core::uint32 max_bits,
                              core::byte* lengths);
    static void build_codes(const core::byte* lengths, core::uint32 size, core::uint32* codes);
};

class Compressor50 {
public:
    static const core::uint32 NC = 306;
    static const core::uint32 DCB = 64; // RAR5 base (up to 4 GB)
    static const core::uint32 DCX = 80; // RAR7 extended (up to 1 TB, ExtraDist)
    static const core::uint32 LDC = 16;
    static const core::uint32 RC = 44;
    static const core::uint32 BC = 20;
    static const core::uint32 TABLE_SIZE = NC + DCB + LDC + RC;  // 430 RAR5
    static const core::uint32 TABLE_SIZEX = NC + DCX + LDC + RC; // 446 RAR7

    static const core::uint32 HASH_BITS = 17;
    static const core::uint32 HASH_SIZE = 1 << HASH_BITS;

    static const size_t MIN_MATCH = 3;
    static const core::uint32 MAX_LZ_MATCH = 0x1001;

private:
    static const size_t READ_CHUNK = 0x100000;

    io::FileStream *src_file_{nullptr}, *dest_file_{nullptr};

    core::uint64 src_size_{0};
    core::uint64 src_loaded_{0};
    bool src_eof_{false};
    // Match-search parameters (derived from method_ per session) and the
    // FailCount heuristic counter — members so process_available() can be
    // shared verbatim by the one-shot loop and streaming feeds.
    core::uint32 max_chain_{0};
    core::uint32 nice_len_{0};
    core::uint32 lazy_tests_{0};
    core::uint32 fail_count_{0};
    bool streaming_{false};
    bool stream_finished_{false};
    // Look-ahead depth one-shot processing guarantees at every position
    // (the load_data(cur_ + 0x80000) quantum). Streaming defers decisions
    // until the same depth is fed, keeping output byte-identical.
    static constexpr core::uint64 STREAM_LOOKAHEAD = 0x80000;

    std::vector<core::byte> buf_;
    const core::byte* buf_data_{nullptr};
    bool external_buf_{false};

    const core::byte* mem_src_ptr_{nullptr};
    size_t mem_src_size_{0};
    size_t mem_src_pos_{0};

    core::uint64 pos_base_{0};
    size_t buf_size_{0};

    size_t win_size_{0};
    core::uint64 max_dist_{0};
    int method_{3};

    std::vector<core::uint32> head_;
    std::vector<core::uint32> prev_;

    // Split token storage: literals go in lit_bytes_ (1 byte each) and
    // match/rep tokens in match_tokens_ (8 bytes each).  token_seq_ records
    // the interleaved order (0 = literal, 1 = match/rep) so emit_tokens can
    // replay them in sequence.  This avoids wasting 6 bytes per literal.
    std::vector<core::byte> token_seq_;           // 0 = literal, 1 = match/rep
    std::vector<core::byte> lit_bytes_;           // literal byte values
    std::vector<Compressor50Token> match_tokens_; // match/rep data
    std::vector<core::byte> block_mem_;
    core::uint32 freq_ld_[NC], freq_dd_[DCX], freq_ldd_[LDC], freq_rd_[RC], freq_bd_[BC];
    core::byte len_ld_[NC], len_dd_[DCX], len_ldd_[LDC], len_rd_[RC], len_bd_[BC];
    core::uint32 code_ld_[NC], code_dd_[DCX], code_ldd_[LDC], code_rd_[RC], code_bd_[BC];
    core::byte table_bits_[TABLE_SIZEX];
    // Current table size in use (430 RAR5 or 446 RAR7 ExtraDist)
    core::uint32 cur_table_size_{TABLE_SIZE};

    size_t old_dist_[4];

    crypto::Crc32 hash_crc32_;
    crypto::Blake2sp hash_blake2_;

    core::uint64 input_since_block_{0};
    core::uint64 packed_total_{0};
    core::uint64 total_at_file_start_{0};
    core::uint64 cur_{0};
    core::uint64 unhashed_pos_{0};

    std::vector<core::byte>* mem_out_{nullptr};

    void init_freq();
    void reset_old_dist() {
        for (core::uint32 i = 0; i < 4; i++) old_dist_[i] = static_cast<size_t>(-1);
    }
    // Restore every scalar/pointer member to its post-default-construction
    // state. Call from the constructor and from any entry-point that begins
    // a fresh session so the object cannot leak state across reuse.
    // Specifically covers external_buf_, mem_src_*, buf_data_ — fields that
    // set_external_buffer() and compress_buffer() latch and that neither
    // the old constructor nor begin_archive() were resetting.
    void reset_state();
    void insert_old_dist(size_t distance) {
        for (core::uint32 i = 3; i > 0; i--) old_dist_[i] = old_dist_[i - 1];
        old_dist_[0] = distance;
    }

    size_t avail_at(core::uint64 pos) const {
        return pos < src_loaded_ ? static_cast<size_t>(src_loaded_ - pos) : 0;
    }
    core::byte byte_at(core::uint64 pos) const {
        return buf_data_[static_cast<size_t>(pos - pos_base_)];
    }
    void load_data(core::uint64 until);

    // 32-bit truncation of absolute positions is safe here. The match_length_simd
    // function performs a physical post-reconstruction verification of the matched bytes.
    // If a hash chain collision spans exactly a 4GB interval, the bytes will differ and
    // it will be safely rejected.
    inline core::int64 reconstruct_pos(core::uint64 pos, core::uint32 trunc) {
        if (trunc == 0xffffffff) return -1;
        core::int64 cand = (pos & ~0xffffffffULL) | trunc;
        if (static_cast<core::uint64>(cand) > pos) {
            cand -= 0x100000000LL;
        }
        if (pos - static_cast<core::uint64>(cand) > max_dist_) return -1;
        return cand;
    }

    core::uint32 calc_hash(core::uint64 pos);
    void insert_position(core::uint64 pos);

    struct MatchInfo {
        size_t length;
        core::int64 candidate;
    };
    MatchInfo find_match(core::uint64 pos, core::uint32 max_chain, core::uint32 nice_len);
    size_t match_length(core::uint64 pos, core::uint64 cand, size_t cap);
    size_t rep_length(core::uint64 pos, size_t dist, size_t cap);

    static void length_to_slot(core::uint32 length, core::uint32& slot, core::uint32& extra,
                               core::uint32& bits);
    static void distance_to_slot(size_t distance, core::uint32& slot, core::uint64& extra,
                                 core::uint32& raw_bits);
    static core::uint32 length_increment(size_t distance);

    void add_literal(core::byte b);
    void add_match(size_t length, size_t distance);
    void add_rep(core::uint32 index, size_t length);

    bool need_flush();
    // Shared streaming/one-shot loop: slides the window, loads input (no-op
    // for streaming), hashes, finds matches and emits blocks. Runs until
    // cur_ consumes everything loaded; when !final it stops early at
    // cur_+4 > src_loaded_ so pending match decisions wait for more input.
    int process_available(bool final);
    void slide_window();
    void init_match_params();
    // Emit one RAR5 compressed block. Returns false when the encoded block
    // body exceeds the 16 MiB (2^24) size ceiling the RAR5 block header can
    // address. In practice need_flush() fires at 32768 tokens / 512 KiB of
    // input so this branch is unreachable during normal compression; the
    // check exists so a hypothetical future encoder change (larger token_seq_
    // threshold, unbounded literal runs, etc.) can't silently truncate
    // data_size to 0xFFFFFF and emit a header whose block-size field lies
    // about the byte count that follows.
    bool write_block(bool last_block);
    void make_tables();
    void emit_table(BitOutput& local_out);
    void emit_tokens(BitOutput& local_out);
    void consume_source(core::uint64 from, core::uint64 to) {
        if (from < to) {
            const core::byte* data = &buf_data_[static_cast<size_t>(from - pos_base_)];
            size_t size = static_cast<size_t>(to - from);
            hash_crc32_.update(data, size);
            hash_blake2_.update(data, size);
        }
    }

public:
    Compressor50();
    ~Compressor50() = default;

    void set_memory_dest(std::vector<core::byte>* mem) { mem_out_ = mem; }

    void init(io::FileStream* src, io::FileStream* dest, int method, core::uint64 src_file_size,
              size_t win_size);
    void begin_archive(io::FileStream* dest, int method, size_t win_size);
    void start_file(io::FileStream* src, core::uint64 src_file_size, bool continue_window);

    void set_external_buffer(const core::byte* src, size_t n) {
        external_buf_ = true;
        buf_data_ = src;
        buf_size_ = n;
        src_size_ = n;
        src_loaded_ = n;
        src_eof_ = true;
        pos_base_ = 0;
        cur_ = 0;
        unhashed_pos_ = 0;
    }

    core::int64 compress();

    static bool compress_buffer(const core::byte* src, size_t src_size,
                                std::vector<core::byte>& dest, int method = 3,
                                size_t win_size = 0x200000);

    // ── Streaming (incremental) compression ─────────────────────────────────
    // Path-A design (docs/streaming-considerations.md §7): the compressor
    // owns every streaming invariant internally; callers never patch state.
    //
    //   begin_stream(method, win_size)  — one session, methods 1..5
    //   feed(data, n)                   — copy in bytes; emits blocks as
    //                                     need_flush() fires into the
    //                                     memory dest (set_memory_dest)
    //   finish_stream()                 — final block (last_block=true)
    //
    // The concatenation of all emitted blocks byte-identically equals
    // compress_buffer() of the same concatenated input at the same
    // method/win_size (enforced by tests/unit/stream_encoder_tests.cpp).
    // Intermediate blocks carry last_block=false; exactly one
    // last_block=true block is written, by finish_stream().
    bool begin_stream(int method, size_t win_size);
    // Returns 0 on success, -1 on error (invalid state, OOM, encode failure).
    int feed(const core::byte* data, size_t n);
    int finish_stream();
    bool streaming() const { return streaming_; }
    bool stream_finished() const { return stream_finished_; }

    core::uint32 get_unpacked_crc32() { return hash_crc32_.get(); }

    // Returns the number of source bytes actually loaded by the compressor.
    core::uint64 source_bytes_loaded() const { return src_loaded_; }
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_COMPRESSOR50_HPP
