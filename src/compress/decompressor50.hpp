#ifndef OPENRAR_COMPRESS_DECOMPRESSOR50_HPP
#define OPENRAR_COMPRESS_DECOMPRESSOR50_HPP

#include "../core/types.hpp"
#include "filters50.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace openrar::compress {

class BitReader {
public:
    using InputCallback = std::function<size_t(core::byte* buf, size_t max_size)>;
    BitReader(const core::byte* data, size_t size);
    BitReader(InputCallback cb, core::uint64 size);

    core::uint32 peek_bits(unsigned int count);
    core::uint64 peek_bits64(unsigned int count);
    core::uint32 get_bits(unsigned int count);
    core::uint64 get_bits64(unsigned int count);
    void consume_bits(unsigned int count);
    void align_byte();

    core::uint64 bits_remaining() const;
    // H-07: sticky flag, set once any get/consume had to zero-pad past the
    // true end of input. Decoders check it before emitting a token so a
    // truncated stream cannot fabricate symbols from the padding.
    bool truncated() const { return truncated_; }
    core::uint64 bit_pos() const { return consumed_; }
    core::uint64 byte_pos() const { return consumed_ / 8; }

private:
    void refill();
    bool fetch_chunk();

    InputCallback cb_{nullptr};
    std::vector<core::byte> buf_;

    const core::byte* p_;
    const core::byte* end_;
    core::uint64 acc_{0};
    unsigned int n_{0};
    core::uint64 consumed_{0};
    core::uint64 size_{0};
    core::uint64 fetched_{0};
    bool truncated_{false};
};

class HuffmanDecoder {
public:
    static constexpr size_t MAX_SYMBOLS = 306;
    static constexpr unsigned int MAX_CODE_BITS = 15;

    bool build(const core::byte* bit_lengths, size_t count);
    core::uint32 decode(BitReader& reader) const;

    // Validation helpers
    size_t max_num() const { return max_num_; }

private:
    static constexpr unsigned int QUICK_BITS = 10;
    static constexpr size_t QUICK_SIZE = 1 << QUICK_BITS;

    // Quick table for first QUICK_BITS
    // Entry format: (length << 16) | symbol
    core::uint32 quick_table_[QUICK_SIZE]{0};

    // Canonical tables for slow path
    core::uint32 decode_len_[16]{};
    core::uint32 decode_pos_[16]{};
    core::uint16 decode_num_[MAX_SYMBOLS]{};
    size_t max_num_{0};
    unsigned int quick_bits_{QUICK_BITS};
};

// C++17 default-initialising allocator: leaves primitive types uninitialised
// on resize/allocation (OPT-D01), eliminating redundant page zeroing.
template <typename T, typename A = std::allocator<T>> struct default_init_allocator : A {
    using A::A;
    template <typename U> struct rebind {
        using other =
            default_init_allocator<U, typename std::allocator_traits<A>::template rebind_alloc<U>>;
    };
    template <typename U>
    void construct(U* ptr) noexcept(std::is_nothrow_default_constructible<U>::value) {
        ::new (static_cast<void*>(ptr)) U;
    }
    template <typename U, typename... Args> void construct(U* ptr, Args&&... args) {
        std::allocator_traits<A>::construct(static_cast<A&>(*this), ptr,
                                            std::forward<Args>(args)...);
    }
};

// Dictionary storage that grows IN PLACE. On 64-bit native builds a large
// window reserves address space for the whole declared size up front (no
// memory committed) and commits pages as decoded output arrives, so growth
// never copies and the peak footprint is the committed size, not old + new.
// Small windows and 32-bit/WASM builds use a std::vector (bounded at 1 GiB
// there, where copying is affordable). Allocated uninitialised via
// default_init_allocator (OPT-D01). Move-only.
class WindowBuffer {
public:
    WindowBuffer() = default;
    ~WindowBuffer() { release(); }
    WindowBuffer(const WindowBuffer&) = delete;
    WindowBuffer& operator=(const WindowBuffer&) = delete;
    WindowBuffer(WindowBuffer&& o) noexcept { take(o); }
    WindowBuffer& operator=(WindowBuffer&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }

    // Set the maximum size (the declared window). Reserves address space
    // when the virtual-memory path applies. False if the reservation fails.
    bool init(size_t max_bytes);
    // Ensure size() >= n (n <= max). False on allocation/commit failure,
    // leaving the buffer unchanged.
    bool grow_to(size_t n);
    void release();

    core::byte* data() { return base_; }
    const core::byte* data() const { return base_; }
    size_t size() const { return size_; }
    core::byte& operator[](size_t i) { return base_[i]; }
    const core::byte& operator[](size_t i) const { return base_[i]; }

private:
    void take(WindowBuffer& o) noexcept {
        vec_ = std::move(o.vec_);
        base_ = o.base_;
        size_ = o.size_;
        reserved_ = o.reserved_;
        committed_ = o.committed_;
        vm_ = o.vm_;
        o.base_ = nullptr;
        o.size_ = o.reserved_ = o.committed_ = 0;
        o.vm_ = false;
    }
    std::vector<core::byte, default_init_allocator<core::byte>> vec_; // non-VM storage
    core::byte* base_{nullptr};
    size_t size_{0};      // usable bytes
    size_t reserved_{0};  // VM: reserved address space
    size_t committed_{0}; // VM: committed bytes (page-rounded, >= size_)
    bool vm_{false};
};

struct FilterEntry {
    core::uint8 type{0};
    core::uint8 channels{0};
    core::uint64 block_start{0}; // absolute output position (see flush_pending)
    core::uint64 file_offset{0}; // linear file position for E8/ARM point fixup
    core::uint32 block_length{0};
};

enum class DecompressErrorCode {
    Ok = 0,
    DictionaryTooLarge =
        1, // spec max exceeded or > alloc limit 1 GiB – spec says fail not truncate
    AllocationFailed = 2, // valid size but OOM (bad_alloc)
    InvalidInput = 3
};

class Decompressor50 {
public:
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(_M_IX86) || defined(__i386__) ||       \
    defined(__arm__) || defined(_M_ARM)
    // Implementation alloc limit 1 GiB on 32-bit/WASM; larger dictionaries fail instead of truncating.
    static constexpr size_t ALLOC_LIMIT = 1ULL * 1024 * 1024 * 1024;
#else
    // Implementation alloc limit 64 GiB on 64-bit; larger dictionaries fail instead of truncating.
    static constexpr size_t ALLOC_LIMIT = 64ULL * 1024 * 1024 * 1024;
#endif
    // ILP32 guard: on a 32-bit size_t the 64 GiB constant above would silently
    // truncate (mod 2^32 == 0), making EVERY decoder report DictionaryTooLarge
    // (found by the armv7 CI leg: all 12 decompression tests failed at once).
    // This assert turns that class of mistake into a compile error.
    static_assert(ALLOC_LIMIT == 1ULL * 1024 * 1024 * 1024 ||
                      ALLOC_LIMIT == 64ULL * 1024 * 1024 * 1024,
                  "ALLOC_LIMIT does not fit this target's size_t");
    // Spec max per 01-headers.md:95 : 128 KiB << N  ; version0 N<=15 (4096 MiB), version1 N<=23 (1 TB) with FCI_DICT_FRACT
    static constexpr core::uint64 SPEC_MAX_V0 = 128ULL * 1024 << 15;
    static constexpr core::uint64 SPEC_MAX_V1 = 128ULL * 1024 << 23;
    static constexpr core::uint64 SPEC_MAX_ABSOLUTE = SPEC_MAX_V1;

    // Default sliding-dictionary window. A raw block stream carries no
    // dictionary-size header, so a decoder that is not told the window must
    // assume the compressor's default: compress_buffer() defaults to
    // 0x200000 (its pow2 clamp only ever shrinks the window) and the dll/wasm
    // compress fallbacks document the same 2 MiB. A window larger than the
    // stream's actual dictionary is strictly more permissive — decoded
    // output is invariant once the window covers every distance and filter
    // region the stream references.
    static constexpr size_t DEFAULT_WIN_SIZE = 0x200000;

    // extra_dist selects the RAR7 algorithm's 446-entry tables (DCX = 80
    // distance slots). It comes from the file header (FileBlock::extra_dist:
    // unp_ver 1 without FCI_RAR5_COMPAT), NEVER from the window size: a
    // RAR5-algorithm member may declare a window above 4 GiB and still use
    // 430-entry tables (that is what RAR writes and our encoder emits). Raw
    // block-codec streams carry no header and default to the RAR5 flavor.
    explicit Decompressor50(size_t win_size = DEFAULT_WIN_SIZE, bool extra_dist = false,
                            size_t alloc_win = 0);
    ~Decompressor50() = default;
    // Move-only: the window owns a virtual-memory reservation (WindowBuffer).
    Decompressor50(Decompressor50&&) noexcept = default;
    Decompressor50& operator=(Decompressor50&&) noexcept = default;
    Decompressor50(const Decompressor50&) = delete;
    Decompressor50& operator=(const Decompressor50&) = delete;

    // Re-select the table flavor between solid members (RAR allows a RAR7
    // member to continue a RAR5 solid stream). Invalidates the current
    // tables when the flavor changes; the member must carry its own.
    void set_extra_dist(bool extra_dist);
    bool extra_dist() const { return use_extra_dist_; }

    // Bytes of dictionary currently allocated. Grows with decoded output up
    // to win_size(); see ApplyEngine::grow().
    size_t window_capacity() const { return engine_.cap_; }
    static constexpr size_t window_initial_bytes() { return ApplyEngine::INITIAL_WINDOW; }

    DecompressErrorCode last_error() const { return last_error_; }
    const std::string& last_error_message() const { return last_error_str_; }
    bool is_dictionary_too_large() const {
        return last_error_ == DecompressErrorCode::DictionaryTooLarge;
    }
    size_t win_size() const { return engine_.win_size_; }
    size_t declared_win() const { return declared_win_; }

    // Callback type for streaming decompressed output.
    // The callback is invoked with contiguous chunks of unpacked data.
    // Return false to abort decompression early.
    using OutputCallback = std::function<bool(const core::byte* data, size_t size)>;

    // Decompresses a compressed RAR 5.0 block payload and streams it to the
    // provided callback. The callback receives data in chunks once they are
    // safe from any pending filters. Callback chunks are capped by the window
    // wrap, so callers must loop over arbitrary chunk sizes as needed.
    //
    // dest_size specifies the maximum number of bytes to unpack (truncation limit).
    // Decompression stops when this limit is reached or the stream naturally ends.
    //
    // Optional out-parameters:
    //   out_written  — number of decoded bytes actually processed. Useful when
    //                  the caller wants to know whether the stream produced less
    //                  than dest_size (e.g. failing fast on truncated streams).
    //   out_finished — true iff the stream terminated on a LastBlock flag;
    //                  false if decoding stopped because dest_size was filled
    //                  and more compressed data remained.
    bool decompress(const core::byte* src, size_t src_size, size_t dest_size, bool solid = false,
                    OutputCallback flush_cb = nullptr, size_t* out_written = nullptr,
                    bool* out_finished = nullptr);

    bool decompress(BitReader::InputCallback src_cb, size_t src_size, size_t dest_size,
                    bool solid = false, OutputCallback flush_cb = nullptr,
                    size_t* out_written = nullptr, bool* out_finished = nullptr);

    // Decompresses exactly one compressed RAR 5.0 block.
    // If solid is true, dictionary history and active tables carry over.
    bool decompress_block(const core::byte* src, size_t src_size, OutputCallback flush_cb = nullptr,
                          size_t* out_written = nullptr, bool* out_finished = nullptr,
                          bool solid = false);

    // Streams the decompressed output into a growing vector until the RAR5
    // stream terminates on a LastBlock flag. Uses decompress() internally
    // with a geometric grow-and-retry strategy so we never require callers
    // to know unp_size upfront (WASM/DLL/BufferArchive callers do not have
    // it). Refuses to allocate more than MAX_STREAM_OUTPUT bytes to bound
    // memory usage on adversarial input.
    bool decompress_to_vector(const core::byte* src, size_t src_size, std::vector<core::byte>& out,
                              bool solid = false);

    // ── v1.37.0 two-phase parallel decode (docs/v1.37.0-two-phase-*.md) ────
    //
    // Phase 0: the pre-scan. A parse-only walk of the packed stream's block
    // headers (the decoder's own read_block_header/read_tables), producing
    // the block timeline (per-block bit positions + description in force)
    // phase-1 span workers claim. NO tokens are decoded: the symbol parse is
    // stateless given the tables (Gate 0 §0), so a worker starting at block
    // i needs only the timeline and the parsed description — never rep
    // state, output position, or any prior token.
    struct PrescanBlock {
        size_t src_byte{0};     // packed-stream byte offset of the header
        uint32_t header_len{0}; // header bytes (flags + check + size)
        uint32_t block_size{0}; // payload bytes after the header
        uint32_t desc_id{0};    // description in force for this block
        uint32_t token_bits{0}; // bits from src_byte to the block's first token
                                // (header + description); a worker that builds
                                // tables from the recorded description [R10]
                                // skips here instead of re-parsing the bits
        bool table_present{false};
        bool last_block{false};
        int block_bit_size{0};
    };

    // H-12: fixed-capacity code-length table. Every RAR5 block carries a
    // table, so a std::vector here cost one heap allocation per block in
    // prescan_member(). Keeps the vector-like assign/data/size surface the
    // decoder and the kernel probes use.
    struct TableLengths {
        static constexpr size_t CAPACITY = 446; // TABLE_SIZEX (RAR7 ExtraDist)
        std::array<core::byte, CAPACITY> v{};
        size_t n{0};
        void assign(const core::byte* first, const core::byte* last) {
            const size_t count = static_cast<size_t>(last - first);
            n = count < CAPACITY ? count : CAPACITY;
            std::memcpy(v.data(), first, n);
        }
        const core::byte* data() const { return v.data(); }
        core::byte* data() { return v.data(); }
        size_t size() const { return n; }
    };

    struct PrescanDescription {
        TableLengths lengths;   // cur_table_size_ code lengths
        bool extra_dist{false}; // 446-entry table (RAR7 >4 GiB dict)
    };

    // Hostile-stream cap (review R2): minimal 3-byte headers would otherwise
    // force O(packed) timeline entries with ~16x memory amplification.
    // Overflow => pre-scan failure => sequential fallback.
    static constexpr size_t PRESCAN_MAX_BLOCKS = 1u << 22; // 4 Mi blocks

    struct PrescanTimeline {
        std::vector<PrescanBlock> blocks;
        std::vector<PrescanDescription> descriptions;
        size_t packed_extent{0}; // byte offset just past the last block's payload
        bool saw_last_block{false};
        bool ok{false};
    };

    // Fails fail-closed on any framing anomaly (bad header checksum, size
    // overrun, description parse error, block-count cap). The scan is at
    // least as strict as the sequential framing walk (it reuses the same
    // readers); a member it rejects goes to the sequential path, which owns
    // the failure verdict. Call on a scratch instance (like the parked
    // scout): read_tables mutates this instance's table state.
    bool prescan_member(const core::byte* src, size_t src_size, PrescanTimeline& out);

    // ── Phase 1: operation records (plan M2, Gate 0 §0 record set) ──────────
    //
    // One wire token per record, in span-relative token order. 12 bytes:
    // {tag, aux, a, b}. The v0 flavor is gated (win_size <= 4 GiB), and a
    // resolved 1-based distance spans [1, win_size] — so the maximal 4 GiB
    // window has the single value 2^32, which does not fit a u32. `a` is
    // therefore stored 0-BASED ([1, 2^32] -> [0, 0xFFFFFFFF]); the applier
    // widens and adds one. v1-flavor members (446 tables, > 4 GiB windows)
    // take the sequential path.
    struct OpRecord {
        enum class Tag : core::uint8 {
            Lit = 0,    // aux = count (1..255), b = literal-pool offset
            Match = 1,  // a = distance - 1 (resolved, u32), b = length (ladder applied)
            Rep = 2,    // aux = index 0..3, a = RD length (no ladder)
            R257 = 3,   // no payload: applier re-emits (old_dist[0], last_length)
            Filter = 4, // a = raw start delta, b = length (post-clamp), aux = type | ((ch-1)<<3)
        };
        core::uint8 tag;
        core::uint8 aux;
        core::uint32 a;
        core::uint32 b;
    };
    static_assert(sizeof(OpRecord) == 12, "OpRecord layout is part of the span contract");

    // One span's phase-1 output. Record memory is bounded by the driver's
    // per-span cap (review R2/G5); overflow fails the span => sequential
    // fallback, never OOM.
    struct SpanRecords {
        std::vector<OpRecord> recs;
        std::vector<core::byte> lit_pool; // Lit runs point here
        size_t out_lower_bound{0};        // wire output length, a LOWER bound:
                                          // slot-257 tokens contribute their
                                          // last_length, which a span worker
                                          // cannot know — the applier is the
                                          // sole authority for stop/clamp [R8]
        bool crossed_hint{false};         // out_lower_bound reached dest_size
        bool saw_last_block{false};
        bool ok{false};
    };

    // Builds the four token decoders from a pre-scanned description — the
    // same product read_tables would build (review R10: workers never
    // re-parse description bits).
    bool initialize_from_description(const PrescanDescription& desc);

    // Phase-1 span worker: decodes tokens of blocks [first_block,
    // last_block] (inclusive, from the pre-scan timeline) into records.
    // No window, no rep-state dependency, no output writes. Position-free
    // validity only (dist = 0 / dist > win_size fails the span; everything
    // position-dependent is the applier's sequential check set). Each wire
    // header is verified against the timeline (F6 detector). rec_cap bytes
    // bound records + literal pool. Call on a scratch instance per span.
    bool decode_span(const PrescanTimeline& tl, const core::byte* src, size_t src_size,
                     size_t dest_size, uint32_t first_block, uint32_t last_block, size_t rec_cap,
                     SpanRecords& out);

    // Phase 2: the applier (plan M3). Consumes spans in order through the
    // SAME ApplyEngine the sequential token loop drives [R1]: identical
    // window, copy, filter-queue, and flush semantics - byte- AND chunk-
    // identical emission. The applier owns the member's rep state and the
    // running output position; span records are member-local [R7]. Driver
    // lifecycle: apply_begin once per member, apply_span_records per span
    // strictly in order (is_last on the final span runs the end-of-call
    // flush and the truncation check). The applier reports success/failure
    // and the byte count; the sequential out_finished semantics stay in the
    // sequential API (the driver's contract is ok + byte count).
    bool apply_begin();
    bool apply_span_records(const SpanRecords& sr, size_t dest_size, bool is_last,
                            OutputCallback flush_cb, size_t* io_total_written);

    // Aggregate filter budget across the member's spans (the sequential
    // decoder's filters_total_len, report M7); reset by apply_begin.
    size_t filters_total_len_{0};

#ifdef OPENRAR_CROSS_VALIDATE
    // Seed the decompressor with the expected original source bytes.
    // When enabled, copy_match will verify that all dictionary references
    // produce identical bytes to the original source.
    //
    // Filter-awareness (v1.22.0): inside a filter region the window holds
    // the TRANSFORMED stream (the encoder pre-transformed it), which
    // legitimately differs from the raw source — the inverse transform is
    // applied out-of-place on flush. Matches whose window range intersects
    // a decoded filter region are therefore exempt from validation; the
    // final memcmp in the harness still fully validates the output.
    void set_validation_source(const core::byte* src, size_t size) {
        engine_.val_src_ = src;
        engine_.val_size_ = size;
        engine_.val_regions_.clear();
    }
#endif

    // Safety cap on decompress_to_vector's growable output. 4 GiB is large
    // enough for any realistic RAR block payload while keeping memory
    // bounded on hostile input (e.g. crafted archives claiming to expand
    // into unbounded output).
    //
    // MUST stay 64-bit (core::uint64): as size_t it truncates to 0 on
    // wasm32 (4 GiB is not representable in a 32-bit size_t), which made
    // every decompress_to_vector call fail on wasm while native builds
    // passed. On wasm32 size_t itself is already bounded at 4 GiB - 1,
    // so the cap stays meaningful there.
#if defined(__EMSCRIPTEN__) || defined(_M_IX86) || defined(__i386__)
    static constexpr core::uint64 MAX_STREAM_OUTPUT =
        2ULL * 1024 * 1024 * 1024; // 2 GiB on 32-bit / WASM
#else
    static constexpr core::uint64 MAX_STREAM_OUTPUT =
        64ULL * 1024 * 1024 * 1024; // 64 GiB on 64-bit native
#endif

private:
    struct BlockHeader {
        int block_size{-1};
        int block_bit_size{0};
        // int64, not int: the sequential decoder's BitReader runs continuously
        // across the whole packed member, so byte_pos() is the CUMULATIVE member
        // offset and a member over 2 GiB wrapped this field negative. The
        // wrapped value then sign-extended through block_end_bit() and the
        // decoder parsed the next block header out of the middle of a payload —
        // a valid >2 GiB member could not be decoded at all (v1.41.1).
        core::int64 block_start{0};
        int header_size{0};
        bool last_block_in_file{false};
        bool table_present{false};
    };

    // The shared apply engine (v1.37.0 plan M0, review directive R1): ring
    // window + match copy + filter queue + flush cadence. Driven by BOTH the
    // sequential token loop (decompress_internal) and the two-phase record
    // applier — the LZ dependency chain has exactly ONE home, so the
    // sequential and parallel paths cannot drift. Not a public surface; the
    // class layout is internal to the static library.
    struct ApplyEngine {
        // window configuration (set once by init)
        size_t win_size_{0};
        size_t win_mask_{0};
        bool win_pow2_{false};

        // Plain-byte flush cadence. Unflushed output lives in the circular
        // window until flushed, and flush_plain_up_to() fails closed once it
        // exceeds win_size_. The check fires only between tokens, so the
        // cadence needs headroom for one maximal token (match <= 0x1001 + 3
        // bytes): half the window for small windows (the 64 KiB floor), else
        // 64 KiB. The previous min(64 KiB, win) EQUALLED the window at the
        // floor, so a long match could push the unflushed span past win_size_
        // and a valid stream failed to decode (found by fuzz_target).
        size_t flush_cadence() const { return std::min<size_t>(65536, win_size_ / 2); }
        // ring window. win_size_ is the DECLARED dictionary (the circular
        // geometry every distance/flush check uses); cap_ is the bytes
        // actually allocated. Invariant: cap_ < win_size_ implies the ring
        // has never wrapped (!first_win_done_) and all written data lives in
        // the linear prefix [0, win_pos_), so growing in place by resize()
        // is exact. Every write path reserves first (grow()), so no index
        // ever reaches cap_. Memory therefore tracks decoded output, not the
        // header's claim: a hostile 64 GiB declaration over a tiny payload
        // allocates kilobytes.
        WindowBuffer window_;
        size_t cap_{0};
        size_t win_pos_{0};
        bool first_win_done_{false};
        bool window_ready_{false};
        // filter queue + flush cursor (last_flushed_ is call-local; reset
        // by begin_call, exactly like the sequential decoder's call-local)
        std::vector<FilterEntry> filters_;
        size_t filters_head_{0}; // consumed-prefix cursor (v1.38.0 M2)
        size_t last_flushed_{0};
        // Bounded persistent scratch buffers for filter application (OPT-19);
        // capped to <= 1 MiB retained capacity per docs/invariants.md §2.
        std::vector<core::byte> filter_scratch_;
        std::vector<core::byte> delta_scratch_;
#ifdef OPENRAR_CROSS_VALIDATE
        const core::byte* val_src_{nullptr};
        size_t val_size_{0};
        // (start, len) of every decoded filter region, ascending; validation
        // is suppressed for matches whose dictionary range intersects one.
        std::vector<std::pair<size_t, size_t>> val_regions_;
#endif

        void init(size_t win_size);
        inline size_t wrap_up(size_t pos) const {
            if (win_pow2_) return pos & win_mask_;
            return pos >= win_size_ ? pos - win_size_ : pos;
        }
        // Allocates the initial window (min(win_size_, INITIAL_WINDOW));
        // false on bad_alloc.
        bool ensure_window_alloc();
        // Member-start reset (the !solid branch): position reset only. No
        // zero-fill is needed: on the first pass every reference past the
        // written prefix takes copy_match's zero path, so stale bytes from
        // an earlier member are never observable. Capacity is retained.
        void reset_window();
        // Make room for n more bytes at win_pos_. No-op once the full ring
        // is allocated. Grows geometrically (amortized O(1) per byte).
        // False on allocation failure; the window is left unchanged.
        inline bool reserve(size_t n) {
            if (cap_ == win_size_ || n <= cap_ - win_pos_) return true;
            return grow(n);
        }
        bool grow(size_t n);
        static constexpr size_t INITIAL_WINDOW = 256 * 1024;
        // Resets the call-local flush cursor (per decompress/decompress_block
        // call, like the sequential decoder's call-local last_flushed).
        void begin_call() { last_flushed_ = 0; }
        inline bool write_literal(core::uint32 slot) {
            if (win_pos_ >= cap_ && !grow(1)) return false;
            window_[win_pos_] = static_cast<core::byte>(slot);
            win_pos_ = wrap_up(win_pos_ + 1);
            if (win_pos_ == 0) first_win_done_ = true;
            return true;
        }
        // False only when the window could not grow (allocation failure).
        bool copy_match(size_t distance, size_t length, size_t total_written);
        bool flush_plain_up_to(size_t target, size_t total_written, size_t dest_size,
                               OutputCallback cb);
        bool flush_pending(bool flush_all, core::uint64 base_at_entry, size_t total_written,
                           size_t dest_size, OutputCallback cb);
    };

    ApplyEngine engine_;

    // The window could not grow to hold more output: report it with the
    // same code the up-front allocation used (archive layer -> RAR_ERR_NOMEM).
    bool window_alloc_failed() {
        last_error_ = DecompressErrorCode::AllocationFailed;
        last_error_str_ = "dictionary window allocation failed";
        return false;
    }

    bool read_block_header(BitReader& reader, BlockHeader& header);
    bool read_tables(BitReader& reader, BlockHeader& header);
    core::uint32 slot_to_length(BitReader& reader, core::uint32 slot);
    bool decompress_internal(BitReader& reader, size_t dest_size, bool solid,
                             OutputCallback flush_cb, size_t* out_written, bool* out_finished,
                             bool single_block = false);

    // Repeat match distances
    size_t old_dist_[4]{0, 0, 0, 0};
    size_t last_length_{0};

    // Huffman tables
    HuffmanDecoder ld_decoder_;  // NC=306
    HuffmanDecoder dd_decoder_;  // DC 64/80
    HuffmanDecoder ldd_decoder_; // LDC 16
    HuffmanDecoder rd_decoder_;  // RC 44
    HuffmanDecoder bd_decoder_;  // BC 20
    bool tables_ready_{false};
    core::byte table_[446];
    // Current table size (430 RAR5 or 446 RAR7 ExtraDist)
    size_t cur_table_size_{430};
    bool use_extra_dist_{false};

    core::uint64 file_base_{0}; // unpacked bytes of prior solid files in this group
    // Absolute position where the current member started. The E8/E8E9/ARM
    // transform base is the offset WITHIN the member (the reference resets
    // it per member even on solid chains — oracle-verified, v1.36.x), so
    // filter fixups subtract this from the accumulated position. Whole-
    // stream solid calls are member starts; per-block solid calls continue
    // the current member.
    core::uint64 member_start_{0};

    DecompressErrorCode last_error_{DecompressErrorCode::Ok};
    std::string last_error_str_;
    size_t declared_win_{DEFAULT_WIN_SIZE};
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_DECOMPRESSOR50_HPP
