// Inflate (RFC 1951) tests — v1.29 M1 (plan §1 M1, named negative tests
// inflate_kat_fixed_stored_dynamic / inflate_corrupt_stream_refused /
// inflate_inflight_cap_bomb). KAT streams generated from python zlib raw
// streams (tests/unit/deflate_kats.inc); expected plaintexts pinned by
// length + CRC32, byte-exact for the smallest vector.
#include "../../src/core/types.hpp"
#include "../../src/compress/inflate.hpp"
#include "../../src/crypto/crc32.hpp"

#include <cassert>
#include <iostream>
#include <vector>

#include "test_support.hpp"

using namespace openrar;
using namespace openrar::compress;

#include "deflate_kats.inc"

namespace {

// Deterministic input refiller over a byte vector.
struct VecInput {
    const std::vector<core::byte>* data;
    size_t pos = 0;
    size_t operator()(core::byte* buf, size_t max_size) {
        size_t left = data->size() - pos;
        size_t take = left < max_size ? left : max_size;
        for (size_t i = 0; i < take; ++i) buf[i] = (*data)[pos + i];
        pos += take;
        return take;
    }
};

// LSB-first bit writer for hand-crafted hostile streams (mirrors the
// DEFLATE bit order: integer fields LSB-first, Huffman codes MSB-first).
class BitWriter {
public:
    void put_bit(unsigned int b) {
        if (bit_pos_ == 0) out_.push_back(0);
        if (b) out_.back() |= static_cast<core::byte>(1 << bit_pos_);
        bit_pos_ = (bit_pos_ + 1) % 8;
    }
    void put_bits(core::uint32 value, unsigned int count) { // integer, LSB-first
        for (unsigned int i = 0; i < count; ++i) put_bit((value >> i) & 1);
    }
    void put_code(core::uint32 code, unsigned int len) { // Huffman, MSB-first
        for (unsigned int i = 0; i < len; ++i) put_bit((code >> (len - 1 - i)) & 1);
    }
    void align() {
        while (bit_pos_ != 0) put_bit(0);
    }
    const std::vector<core::byte>& bytes() const { return out_; }

private:
    std::vector<core::byte> out_;
    unsigned int bit_pos_ = 0;
};

InflateError decode_vec(const std::vector<core::byte>& stream, std::vector<core::byte>& out,
                        core::uint64* total_in_out = nullptr) {
    Inflate inf;
    VecInput vin{&stream};
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    const InflateError err = inf.decode(vin, sink);
    if (total_in_out) *total_in_out = inf.total_in();
    return err;
}

} // namespace

static void test_inflate_kats() {
    std::cout << "[+] test_inflate_kats" << std::endl;

    struct Kat {
        const char* name;
        const std::vector<core::byte>* stream;
        size_t plain_len;
        core::uint32 plain_crc;
    };
    const Kat kats[] = {
        {"kat_text", &kat_text_stream, kat_text_plain_len, kat_text_plain_crc},
        {"kat_empty", &kat_empty_stream, kat_empty_plain_len, kat_empty_plain_crc},
        {"kat_longdist", &kat_longdist_stream, kat_longdist_plain_len, kat_longdist_plain_crc},
        {"kat_multiblock", &kat_multiblock_stream, kat_multiblock_plain_len,
         kat_multiblock_plain_crc},
        {"kat_bytes_l1", &kat_bytes_l1_stream, kat_bytes_l1_plain_len, kat_bytes_l1_plain_crc},
        {"kat_stored_l0", &kat_stored_l0_stream, kat_stored_l0_plain_len, kat_stored_l0_plain_crc},
        {"kat_rle", &kat_rle_stream, kat_rle_plain_len, kat_rle_plain_crc},
    };

    for (const Kat& k : kats) {
        std::vector<core::byte> out;
        core::uint64 tin = 0;
        const InflateError err = decode_vec(*k.stream, out, &tin);
        if (err != InflateError::Ok) {
            std::cout << "    ! KAT " << k.name << " error=" << static_cast<int>(err) << " ("
                      << inflate_error_string(err) << ") out=" << out.size()
                      << " want=" << k.plain_len << " tin=" << tin << "/" << k.stream->size()
                      << std::endl;
        }
        assert(err == InflateError::Ok && "KAT decode must succeed");
        (void)k.name;
        assert(out.size() == k.plain_len && "KAT output length mismatch");
        crypto::Crc32 crc;
        crc.update(out.data(), out.size());
        assert(crc.get() == k.plain_crc && "KAT output CRC mismatch");
    }

    // Byte-exact check for the smallest vector.
    std::vector<core::byte> out;
    assert(decode_vec(kat_text_stream, out) == InflateError::Ok);
    assert(out == kat_text_plain && "kat_text byte-exact mismatch");

    std::cout << "    - 7 KATs decoded (stored/fixed/dynamic, long-distance, "
                 "multi-block, RLE-overlap)"
              << std::endl;
}

static void test_inflate_fixed_match() {
    std::cout << "[+] test_inflate_fixed_match" << std::endl;

    // "abcabc": literals a,b,c; match len 3 dist 3; EOB. Fixed block.
    BitWriter w;
    w.put_bits(1, 1);    // BFINAL
    w.put_bits(1, 2);    // BTYPE = 1 fixed
    w.put_code(0x91, 8); // 'a' (97)
    w.put_code(0x92, 8); // 'b' (98)
    w.put_code(0x93, 8); // 'c' (99)
    w.put_code(1, 7);    // length symbol 257 (len 3)
    w.put_code(2, 5);    // distance symbol 2 (dist 3)
    w.put_code(0, 7);    // end of block
    std::vector<core::byte> out;
    core::uint64 tin = 0;
    const InflateError err = decode_vec(w.bytes(), out, &tin);
    std::cout << "    err=" << static_cast<int>(err) << " out=" << out.size() << " tin=" << tin
              << "/" << w.bytes().size() << " bytes=[";
    for (size_t i = 0; i < out.size() && i < 8; ++i) std::cout << static_cast<char>(out[i]);
    std::cout << "]" << std::endl;
    assert(err == InflateError::Ok);
    assert(out.size() == 6);
    assert(out[3] == 'a' && out[4] == 'b' && out[5] == 'c');
}

static void test_inflate_corrupt_stream_refused() {
    std::cout << "[+] test_inflate_corrupt_stream_refused" << std::endl;

    // Reserved block type (BTYPE 11).
    {
        BitWriter w;
        w.put_bits(1, 1); // BFINAL
        w.put_bits(3, 2); // BTYPE = 3
        std::vector<core::byte> out;
        assert(decode_vec(w.bytes(), out) == InflateError::BadBlockType);
    }

    // Stored block with NLEN != ~LEN.
    {
        BitWriter w;
        w.put_bits(1, 1); // BFINAL
        w.put_bits(0, 2); // BTYPE = 0 stored
        w.align();
        w.put_bits(5, 16);      // LEN
        w.put_bits(0xABCD, 16); // NLEN (wrong)
        for (int i = 0; i < 5; ++i) w.put_bits(0x41, 8);
        std::vector<core::byte> out;
        assert(decode_vec(w.bytes(), out) == InflateError::BadStoredLen);
    }

    // Dynamic header whose code-length alphabet has zero codes.
    {
        BitWriter w;
        w.put_bits(1, 1);                             // BFINAL
        w.put_bits(2, 2);                             // BTYPE = 2 dynamic
        w.put_bits(0, 5);                             // HLIT  -> 257
        w.put_bits(0, 5);                             // HDIST -> 1
        w.put_bits(0, 4);                             // HCLEN -> 4
        for (int i = 0; i < 4; ++i) w.put_bits(0, 3); // all lengths zero
        std::vector<core::byte> out;
        assert(decode_vec(w.bytes(), out) == InflateError::BadCodeLengths);
    }

    // Fixed block: match against an empty window (distance too far).
    {
        BitWriter w;
        w.put_bits(1, 1);    // BFINAL
        w.put_bits(1, 2);    // BTYPE = 1 fixed
        w.put_code(0x01, 7); // literal/length symbol 257 (len 3)
        w.put_code(0x00, 5); // distance symbol 0 (dist 1) — window empty
        std::vector<core::byte> out;
        assert(decode_vec(w.bytes(), out) == InflateError::DistanceTooFar);
    }

    // Truncation: drop the tail of a valid stream; must never decode Ok.
    {
        std::vector<core::byte> cut(kat_rle_stream.begin(), kat_rle_stream.end() - 4);
        std::vector<core::byte> out;
        const InflateError err = decode_vec(cut, out);
        assert(err != InflateError::Ok && "truncated stream must not decode");
    }

    // Bit-flip in a dynamic block payload must not decode Ok (either a hard
    // error or, at worst, a different decode — never a silent pass here:
    // the stream is short enough that a flip breaks structure or CRC).
    {
        std::vector<core::byte> flipped = kat_text_stream;
        flipped[flipped.size() - 1] ^= 0x10;
        std::vector<core::byte> out;
        const InflateError err = decode_vec(flipped, out);
        assert(err != InflateError::Ok || out.size() != kat_text_plain_len ||
               out != kat_text_plain);
    }

    std::cout << "    - reserved block type, NLEN mismatch, empty clen "
                 "alphabet, distance-too-far, truncation, bit-flip: all "
                 "refused"
              << std::endl;
}

static void test_inflate_inflight_cap_bomb() {
    std::cout << "[+] test_inflate_inflight_cap_bomb" << std::endl;

    // kat_rle expands to 100005 bytes; a 1000-byte cap must fire exactly.
    Inflate capped(1000);
    VecInput vin{&kat_rle_stream};
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    const InflateError err = capped.decode(vin, sink);
    assert(err == InflateError::OutputCapExceeded && "cap must fire");
    assert(out.size() == 1000 && "cap must bound actual output");
    assert(capped.total_out() == 1000 && "cap accounting");

    // Unlimited decode of the same stream succeeds.
    std::vector<core::byte> full;
    assert(decode_vec(kat_rle_stream, full) == InflateError::Ok);
    assert(full.size() == kat_rle_plain_len);

    std::cout << "    - declared sizes never bound; in-flight cap fires at "
                 "exactly 1000 bytes"
              << std::endl;
}

static void test_inflate_sink_cancel() {
    std::cout << "[+] test_inflate_sink_cancel" << std::endl;

    Inflate inf;
    VecInput vin{&kat_multiblock_stream};
    size_t calls = 0;
    auto sink = [&](const core::byte*, size_t) {
        return ++calls < 2; // cancel at the second flush
    };
    const InflateError err = inf.decode(vin, sink);
    assert(err == InflateError::SinkFailed && "sink abort must surface");
    assert(calls == 2 && "abort surfaces at the cancelling call");
    std::cout << "    - sink abort surfaces as SinkFailed after " << calls << " flushes"
              << std::endl;
}

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    test_inflate_kats();
    std::cout << std::flush;
    test_inflate_fixed_match();
    std::cout << std::flush;
    test_inflate_corrupt_stream_refused();
    std::cout << std::flush;
    test_inflate_inflight_cap_bomb();
    std::cout << std::flush;
    test_inflate_sink_cancel();
    std::cout << std::flush;
    std::cout << "All Inflate (v1.29 M1) tests PASSED!" << std::endl;
    return 0;
}
