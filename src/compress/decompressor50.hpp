#ifndef OPENRAR_COMPRESS_DECOMPRESSOR50_HPP
#define OPENRAR_COMPRESS_DECOMPRESSOR50_HPP

#include "../core/types.hpp"
#include "filters50.hpp"
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

namespace openrar::compress {

class BitReader {
public:
    using InputCallback = std::function<size_t(core::byte* buf, size_t max_size)>;
    BitReader(const core::byte* data, size_t size);
    BitReader(InputCallback cb, size_t size);

    core::uint32 peek_bits(unsigned int count);
    core::uint64 peek_bits64(unsigned int count);
    core::uint32 get_bits(unsigned int count);
    core::uint64 get_bits64(unsigned int count);
    void consume_bits(unsigned int count);
    void align_byte();

    size_t bits_remaining() const;
    size_t bit_pos() const { return consumed_; }
    size_t byte_pos() const { return consumed_ / 8; }

private:
    void refill();
    void fetch_more();

    InputCallback cb_{nullptr};
    std::vector<core::byte> buf_;

    const core::byte* p_;
    const core::byte* end_;
    core::uint64 acc_{0};
    unsigned int n_{0};
    size_t consumed_{0};
    size_t size_{0};
    size_t fetched_{0};
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

struct FilterEntry {
    core::uint8 type{0};
    core::uint8 channels{0};
    size_t block_start{0};       // linear index into this file's output
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
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(_M_IX86) || defined(__i386__)
    // Implementation alloc limit 1 GiB on 32-bit/WASM; larger dictionaries fail instead of truncating.
    static constexpr size_t ALLOC_LIMIT = 1ULL * 1024 * 1024 * 1024;
#else
    // Implementation alloc limit 64 GiB on 64-bit; larger dictionaries fail instead of truncating.
    static constexpr size_t ALLOC_LIMIT = 64ULL * 1024 * 1024 * 1024;
#endif
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

    explicit Decompressor50(size_t win_size = DEFAULT_WIN_SIZE);
    ~Decompressor50() = default;

    DecompressErrorCode last_error() const { return last_error_; }
    const std::string& last_error_message() const { return last_error_str_; }
    bool is_dictionary_too_large() const {
        return last_error_ == DecompressErrorCode::DictionaryTooLarge;
    }
    size_t win_size() const { return win_size_; }

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
        val_src_ = src;
        val_size_ = size;
        val_regions_.clear();
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
#ifdef OPENRAR_CROSS_VALIDATE
    const core::byte* val_src_{nullptr};
    size_t val_size_{0};
    // (start, len) of every decoded filter region, ascending; validation is
    // suppressed for matches whose dictionary range intersects one.
    std::vector<std::pair<size_t, size_t>> val_regions_;
#endif

    struct BlockHeader {
        int block_size{-1};
        int block_bit_size{0};
        int block_start{0};
        int header_size{0};
        bool last_block_in_file{false};
        bool table_present{false};
    };

    bool read_block_header(BitReader& reader, BlockHeader& header);
    bool read_tables(BitReader& reader, BlockHeader& header);
    core::uint32 slot_to_length(BitReader& reader, core::uint32 slot);
    size_t wrap_up(size_t pos) const;
    void copy_match(size_t distance, size_t length, size_t total_written);
    bool apply_filters(core::byte* data, size_t size, core::uint64 file_offset);
    bool flush_pending(bool flush_all, OutputCallback cb);

    bool decompress_internal(BitReader& reader, size_t dest_size, bool solid,
                             OutputCallback flush_cb, size_t* out_written, bool* out_finished,
                             bool single_block = false);

    size_t win_size_;
    size_t win_mask_;
    bool win_pow2_{false};
    std::vector<core::byte> window_;
    size_t win_pos_{0};
    size_t unp_ptr_{0}; // linear position for filter handling
    bool first_win_done_{false};
    bool window_ready_{false};

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

    // Repeat match distances
    size_t old_dist_[4]{0, 0, 0, 0};
    size_t last_length_{0};

    std::vector<FilterEntry> filters_;
    core::uint64 file_base_{0}; // unpacked bytes of prior solid files in this group

    DecompressErrorCode last_error_{DecompressErrorCode::Ok};
    std::string last_error_str_;
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_DECOMPRESSOR50_HPP
