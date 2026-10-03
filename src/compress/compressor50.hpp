#ifndef OPENRAR_COMPRESS_COMPRESSOR50_HPP
#define OPENRAR_COMPRESS_COMPRESSOR50_HPP

#include "../core/types.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/blake2sp.hpp"
#include "../io/file_stream.hpp"
#include "filters50.hpp"

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
        Rep3 = 5,
        // LD symbol 257: repeat (OldDist[0], LastLength) — one Huffman
        // symbol, no extra bits. Carries no length/distance slot data.
        LastLen = 6
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

    // Minimum match length the encoder will emit. RAR5 length slot 0 decodes to
    // length 2 and is fully wire-legal (spec/03: lengths are stored as
    // `length - 2`, so slot 0 covers base 2), and both directions already
    // handle it: length_to_slot(2) -> slot 0 and slot_to_length(0) -> 2
    // (decompressor50.cpp:377-378). This constant was the only thing making the
    // class unreachable.
    //
    // Admissibility still filters by distance: a 2-byte match is legal only
    // where increment(distance) == 0, i.e. distance <= 0x100. That is enforced
    // by the decision loop's `len < inc + 2` rejection (and again inside
    // add_match), never by clamping. Rep lengths carry no increment on either
    // side (decompressor50.cpp:901-903), so a 2-byte rep is legal at any
    // in-window distance.
    static const size_t MIN_MATCH = 2;
    static const core::uint32 MAX_LZ_MATCH = 0x1001;

    // Decoder-slot seeding for block-header table reuse (v1.33 Design A). A
    // decoder's multithreaded driver does not keep one table state per member:
    // it pre-scans the block headers, hands each block to a work item that
    // owns a PRIVATE copy of the table state (initially empty), and assigns
    // blocks to those slots round-robin in batches of 2 x threads. A block
    // that carries a description seeds the slot it lands in; a bit-7-clear
    // block inherits whatever that slot already accumulated. A reuse block
    // landing in a slot that has never received a description therefore
    // decodes against an EMPTY table set: symbol lookup degenerates, the
    // block is exhausted after a few tokens, and the member silently comes
    // out short. The model is behavioral, confirmed by black-box experiment
    // against the extraction oracles (docs/question-log.md); no reference
    // implementation source informs it.
    //
    // Two obligations follow for the encoder, and both are cheap:
    //   1. Cover every slot before the first reuse. The slot count follows
    //      the extractor's requested thread count; the worker pool is clamped
    //      to MAX_DECODER_THREADS, so the period is at most
    //      2 x MAX_DECODER_THREADS. Emitting REUSE_SEED_BLOCKS CONSECUTIVE
    //      descriptions covers every residue class modulo any period up to
    //      that bound.
    //   2. Re-cover after every table-set change. A change refreshes exactly
    //      the one slot it lands in, leaving the others holding a stale set,
    //      so a change is treated as a fresh re-seed event.
    //
    // Between changes, reuse is unbounded. Cost is a fixed REUSE_SEED_BLOCKS
    // descriptions per table change, independent of member size, so this
    // keeps essentially the whole saving. The sequential path has no slot
    // structure and is unaffected either way. Normative text:
    // docs/spec/03-compression-m1-m5.md §Table lifecycle.
    static constexpr unsigned MAX_DECODER_THREADS = 8;
    static constexpr unsigned REUSE_SEED_BLOCKS = 2 * MAX_DECODER_THREADS;

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
    std::vector<core::uint64> head64_;
    std::vector<core::uint64> prev64_;
    bool is_large_window_{false};

    // Split token storage: literals go in lit_bytes_ (1 byte each),
    // match/rep tokens in match_tokens_ (8 bytes each), and filters in filter_tokens_.
    // token_seq_ records the interleaved order:
    // 0 = literal, 1 = match/rep, 2 = filter
    struct FilterToken {
        core::uint32 block_start{0};
        core::uint32 block_length{0};
        core::uint8 type{0};
        core::uint8 channels{1};
    };
    std::vector<core::byte> token_seq_;           // 0 = literal, 1 = match/rep, 2 = filter
    std::vector<core::byte> lit_bytes_;           // literal byte values
    std::vector<Compressor50Token> match_tokens_; // match/rep data
    std::vector<FilterToken> filter_tokens_;      // filter descriptors (slot 256)
    FilterConfig filter_config_{};
    FilterType active_filter_{FilterType::None};
    core::uint8 filter_channels_{1};
    core::uint64 filter_emitted_until_{0};
    // Set when the caller feeds the packer data that is already transformed
    // for the exact filter-token chunking this packer will emit (the
    // compress_buffer() memory path pre-transforms). Suppresses the
    // in-loop transform in process_available(), which would otherwise
    // transform the same bytes a second time.
    bool filter_pretransformed_{false};
    core::uint64 file_start_pos_{0};
    std::vector<core::byte> block_mem_;
    core::uint32 freq_ld_[NC], freq_dd_[DCX], freq_ldd_[LDC], freq_rd_[RC], freq_bd_[BC];
    core::byte len_ld_[NC], len_dd_[DCX], len_ldd_[LDC], len_rd_[RC], len_bd_[BC];
    core::uint32 code_ld_[NC], code_dd_[DCX], code_ldd_[LDC], code_rd_[RC], code_bd_[BC];
    core::byte table_bits_[TABLE_SIZEX];
    // Current table size in use (430 RAR5 or 446 RAR7 ExtraDist)
    core::uint32 cur_table_size_{TABLE_SIZE};

    // Minimum emitted match length actually in force: MIN_MATCH (2) for the
    // shallow-finder methods m1-m3, and 3 for m4/m5 where admitting 2-byte
    // matches measured as a regression. Set by init_match_params().
    size_t min_match_{MIN_MATCH};

    // Block-header table reuse (v1.33 Design A). A block whose freshly built
    // Huffman code lengths are bit-identical to the ones the decoder already
    // holds can clear bit 7 ("tables present") and omit the ~26 B table
    // description. prev_tables_valid_ mirrors the decoder's tables_ready_,
    // which is dropped at a non-solid file boundary
    // (decompressor50.cpp:568) and at stream reset, so reuse is offered only
    // where the decoder still has the tables in hand. The decoder-slot model
    // that dictates REUSE_SEED_BLOCKS and the re-seed-on-change rule is
    // documented with those constants (public, beside the wire-contract
    // constants) and in docs/spec/03-compression-m1-m5.md §Table lifecycle.
    bool prev_tables_valid_{false};
    core::uint32 prev_table_size_{0};
    core::byte prev_len_ld_[NC], prev_len_dd_[DCX], prev_len_ldd_[LDC], prev_len_rd_[RC];
    unsigned seed_remaining_{REUSE_SEED_BLOCKS};

    size_t old_dist_[4];
    // Encoder-side shadow of the decoder's last_length_ (the slot-257
    // continuation length): set to the full emitted length on fresh matches
    // (INCLUDING the distance-dependent increment) and on rep matches
    // (WITHOUT it); unchanged by literals, filters, and 257 tokens; reset
    // with the rep distances per non-solid file. Mirrors
    // decompressor50.cpp's last_length_ lifecycle exactly — the 257
    // emission rule in process_available() is only legal while this shadow
    // and the decoder's state agree.
    size_t last_length_{0};
    // Test-only: suppress 257 emission to pin the pure-addition property
    // (T9: suppressed output must be byte-identical to v1.30.4). Always
    // false in production paths.
    bool lastlen_suppressed_{false};

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
        // The decoder resets last_length_ on every non-solid file start
        // alongside the rep-distance sentinel — the shadow resets at the
        // same sites so a 257 can never precede the first match of a file.
        last_length_ = 0;
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
    // Emit a fresh-match token and return the number of input bytes the emitted
    // token ACTUALLY covers, which is `base + increment(distance)` and not
    // necessarily `length`. The decoder reconstructs base + increment
    // (decompressor50.cpp:867-878) and validates nothing about the length, so a
    // (length, distance) pair whose base falls outside [2, MAX_LZ_MATCH] would
    // silently decode to a DIFFERENT length than the caller intended - a
    // one-byte desync, not a rejection. Therefore:
    //   - below the range, the pair is FILTERED and nothing is emitted (0 is
    //     returned); it is never clamped up into a legal-looking token;
    //   - above the range the match is truncated to the longest legal token,
    //     which is a genuine "emit a shorter match", and the covered count is
    //     reported so the caller advances by what the wire carries;
    //   - last_length_ is derived from the encoded base, so the shadow state
    //     can never disagree with the decoder's copy of it.
    // Callers must advance by the return value, not by `length`.
    size_t add_match(size_t length, size_t distance);
    void add_rep(core::uint32 index, size_t length);
    // Emit the slot-257 repeat-last-length token: one LD Huffman symbol,
    // no extra bits, no OldDist rotation, no shadow change.
    void add_last_len();

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
    void add_filter(const FilterToken& ft);
    void emit_filter_data(BitOutput& out, core::uint32 val);
    void apply_pending_filter(core::uint64 up_to);
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

    void set_filter_config(const FilterConfig& cfg) { filter_config_ = cfg; }
    const FilterConfig& get_filter_config() const { return filter_config_; }
    FilterType get_active_filter() const { return active_filter_; }
    // The power-of-two window this session actually runs with (what
    // begin_archive() derived from the requested size). Filter chunking
    // decisions outside the class must derive from this, not from the
    // requested size, so their chunk boundaries match the filter tokens
    // process_available() emits.
    size_t window_size() const { return win_size_; }
    void set_active_filter(FilterType type, core::uint8 channels = 1, bool pretransformed = false) {
        active_filter_ = type;
        filter_channels_ = channels;
        filter_pretransformed_ = pretransformed;
        filter_emitted_until_ = 0;
    }

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

    // Compress `data_len` bytes at `history`, where the first `seed_len` bytes
    // of that buffer are HISTORY that precedes the data in the member.
    //
    // RAR5 scopes the LZ dictionary to the member, not the block: blocks are
    // framing and entropy-coding boundaries only, and the format has no field
    // expressing a block-independent dictionary. A multi-threaded encoder
    // therefore seeds each chunk's context with the bytes that precede it, so
    // its match finder sees exactly the history a single-threaded encoder
    // would have had at that member offset.
    //
    // The seed is indexed into the hash (process_available's catch-up inserts
    // every position below cur_) but never emitted, and the rep state starts
    // unset so no block can open with a rep-distance or 257.
    //
    // `history` MUST be the `seed_len` bytes immediately preceding the data
    // in the member, contiguous with it. That is the member's POST-FILTER
    // byte stream. The MT paths force filters off, so source bytes and
    // post-filter bytes coincide; if filters are ever enabled here this must
    // become the predecessor's filtered window, not a re-read of the file.
    void set_seeded_external_buffer(const core::byte* history, size_t seed_len, size_t data_len) {
        external_buf_ = true;
        buf_data_ = history;
        buf_size_ = seed_len + data_len;
        src_size_ = seed_len + data_len;
        src_loaded_ = seed_len + data_len;
        src_eof_ = true;
        pos_base_ = 0;
        cur_ = seed_len;
        unhashed_pos_ = 0;
    }

    core::int64 compress(bool last_block = true);

    static bool compress_buffer(const core::byte* src, size_t src_size,
                                std::vector<core::byte>& dest, int method = 3,
                                size_t win_size = 0x200000, const FilterConfig& filter_cfg = {});

    static bool compress_buffer_parallel(const core::byte* src, size_t src_size,
                                         std::vector<core::byte>& dest, int method = 3,
                                         size_t win_size = 0x200000,
                                         const FilterConfig& filter_cfg = {}, unsigned threads = 0);

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
    bool begin_stream(int method, size_t win_size, bool continue_window = false);
    // Solid-chain variant: continue_window=true starts a NEW stream (fresh
    // tables/tokens/CRC, per-file filter scope) while KEEPING the LZ window,
    // hash chains, and rep-distance state — the encoder-side mirror of the
    // decoder's solid carry (Decompressor50::decompress_internal solid=true).
    // Only legal immediately after finish_stream() on a session with the
    // SAME win_size; every distance emitted into the carried window resolves
    // identically in the decoder's carried window. Returns 0 on success,
    // -1 on error (invalid state, OOM, encode failure).
    int feed(const core::byte* data, size_t n);
    int finish_stream();
    bool streaming() const { return streaming_; }
    bool stream_finished() const { return stream_finished_; }

    core::uint32 get_unpacked_crc32() { return hash_crc32_.get(); }

    // Returns the number of source bytes actually loaded by the compressor.
    core::uint64 source_bytes_loaded() const { return src_loaded_; }

    friend class Compressor50TestAccess;
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_COMPRESSOR50_HPP
