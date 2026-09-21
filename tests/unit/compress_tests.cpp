#include "../../src/core/types.hpp"
#include "../../src/compress/filters50.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/stream_encoder.hpp"
#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/arch/match_simd.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
#include <cstdio>

using namespace openrar;
using namespace openrar::compress;

void test_huffman_builder_adversarial() {
    std::cout << "[+] test_huffman_builder_adversarial" << std::endl;
    const core::uint32 size = 306;
    const core::uint32 max_bits = 15;
    core::uint32 freq[size] = {0};
    core::byte lengths[size] = {0};

    auto verify = [&](const char* name) {
        HuffmanBuilder::build_lengths(freq, size, max_bits, lengths);
        core::uint32 kraft_sum = 0;
        core::uint32 kraft_target = 1u << max_bits;
        core::uint32 max_L = 0;
        for (core::uint32 i = 0; i < size; ++i) {
            if (lengths[i] > 0) {
                assert(lengths[i] <= max_bits && "max_bits violated");
                kraft_sum += 1u << (max_bits - lengths[i]);
                if (lengths[i] > max_L) max_L = lengths[i];
            }
        }
        assert(kraft_sum <= kraft_target && "Kraft inequality violated");
        if (kraft_sum > 0) {
            std::cout << "    - " << name << ": OK (max_L=" << max_L << " Kraft=" << kraft_sum
                      << "/" << kraft_target << ")" << std::endl;
        }
    };

    core::uint32 a = 1, b = 1;
    for (core::uint32 i = 0; i < 40; ++i) {
        freq[i] = a;
        core::uint32 next = a + b;
        a = b;
        b = next;
    }
    verify("Fibonacci-40");

    std::memset(freq, 0, sizeof(freq));
    freq[0] = 10000;
    freq[1] = 10000;
    for (core::uint32 i = 2; i < 50; ++i) freq[i] = 1;
    verify("Skewed-2");

    std::memset(freq, 0, sizeof(freq));
    for (core::uint32 i = 0; i < 20; ++i) freq[i] = 1u << i;
    verify("Powers of 2");

    std::memset(freq, 0, sizeof(freq));
    for (core::uint32 i = 0; i < size; ++i) freq[i] = 10;
    verify("Uniform dense");
}

void test_delta_filter() {
    const size_t SIZE = 16;
    core::byte original[SIZE] = {10, 20, 12, 22, 15, 25, 19, 29, 24, 34, 30, 40, 37, 47, 45, 55};
    core::byte encoded[SIZE];
    {
        // RAR delta encode is Prev - Cur (pack.cpp encode_delta_filter: *dst = prev - cur)
        // Decoder does Dst = Prev - Src, so encode must be prev - original to round-trip
        core::byte prev0 = 0, prev1 = 0;
        size_t idx0 = 0, idx1 = SIZE / 2;
        for (size_t i = 0; i < SIZE; i += 2) {
            encoded[idx0++] = static_cast<core::byte>(prev0 - original[i]);
            prev0 = original[i];
            encoded[idx1++] = static_cast<core::byte>(prev1 - original[i + 1]);
            prev1 = original[i + 1];
        }
    }
    core::byte restored[SIZE] = {0};
    bool ok = Filters50::apply_delta(encoded, restored, SIZE, 2);
    assert(ok);
    assert(std::memcmp(original, restored, SIZE) == 0);

    // Edge: size < channels, single channel, in-place non-aliasing
    {
        core::byte tiny[3] = {5, 10, 15};
        core::byte enc[3];
        core::byte p = 0;
        for (size_t i = 0; i < 3; i++) {
            enc[i] = static_cast<core::byte>(p - tiny[i]);
            p = tiny[i];
        }
        core::byte dec[3] = {0};
        assert(Filters50::apply_delta(enc, dec, 3, 1));
        assert(std::memcmp(tiny, dec, 3) == 0);
        // in-place aliasing must work
        core::byte alias[3];
        std::memcpy(alias, enc, 3);
        assert(Filters50::apply_delta(alias, alias, 3, 1));
        assert(std::memcmp(tiny, alias, 3) == 0);
    }
    // invalid channels
    {
        core::byte src[4] = {0}, dst[4] = {0};
        assert(!Filters50::apply_delta(src, dst, 4, 0));
        assert(!Filters50::apply_delta(src, dst, 4, 33));
    }
    std::cout << "[PASS] RAR 5.0 Delta Filter (2-channel + edge cases)\n";
}

void test_e8_filter() {
    core::byte code[16] = {0x90, 0xE8, 0x20, 0x00, 0x00, 0x00, 0x90, 0x90,
                           0x90, 0x90, 0xE9, 0x10, 0x00, 0x00, 0x00, 0x90};
    // Filter should use offset = pos+1+file_offset, not pos+file_offset
    // With file_offset 0, CALL at pos 1 (opcode at 1, operand at 2) -> offset = 2
    // addr 0x20 -> should become 0x20 -2 = 0x1E if <16MB
    Filters50::apply_e8(code, sizeof(code), 0, true);
    core::uint32 addr_call = core::read_le32(code + 2);
    assert(addr_call == 0x20 - 2);

    // size <5 should not OOB
    {
        core::byte tiny[4] = {0xE8, 0x01, 0x00, 0x00};
        Filters50::apply_e8(tiny, 4, 0, false);
        // pos+4 < size condition means last E8 at pos 0 with size 4 is NOT processed (needs 5 bytes)
        // So should be unchanged
        assert(tiny[0] == 0xE8);
    }
    {
        core::byte tiny[5] = {0xE8, 0x10, 0x00, 0x00, 0x90};
        Filters50::apply_e8(tiny, 5, 0, false);
        // now size 5, pos0+4<5 true, so processed
        core::uint32 a = core::read_le32(tiny + 1);
        assert(a != 0x10);
    }
    std::cout << "[PASS] RAR 5.0 x86 E8/E8E9 Filter Translation (off-by-one & bounds)\n";
}

void test_arm_filter() {
    core::byte arm_code[8] = {0x10, 0x00, 0x00, 0xEB, 0x20, 0x00, 0x00, 0xEB};
    Filters50::apply_arm(arm_code, sizeof(arm_code), 0);
    assert(arm_code[3] == 0xEB);
    assert(arm_code[7] == 0xEB);
    // file_offset truncated to 32 bits: use large offset >4GB
    {
        core::byte code[4] = {0x00, 0x01, 0x00, 0xEB};
        Filters50::apply_arm(code, 4, 0x100000000ULL); // lower 32 bits 0
        core::uint32 off = code[0] | (code[1] << 8) | (code[2] << 16);
        // with offset 0, should be 0x0100 -0 =0x0100
        assert(off == 0x0100);
        // with offset 0x100000000, truncated still 0, same result
        core::byte code2[4] = {0x00, 0x01, 0x00, 0xEB};
        Filters50::apply_arm(code2, 4, 0x100000004ULL); // low 32 =4, /4 =1
        core::uint32 off2 = code2[0] | (code2[1] << 8) | (code2[2] << 16);
        assert(off2 == 0xFF); // 0x100 -1
    }
    std::cout << "[PASS] RAR 5.0 ARM Branch Filter Translation\n";
}

void test_forward_filters() {
    // 1. x86 E8/E8E9 round-trip test
    {
        std::vector<core::byte> original(2048);
        for (size_t i = 0; i < original.size(); ++i) {
            original[i] = static_cast<core::byte>((i * 37 + 11) & 0xFF);
        }
        // Plant valid E8/E9 call sites
        // Call site 1: pos 10, offset 0x20
        original[10] = 0xE8;
        core::write_le32(&original[11], 0x00000020);
        // Call site 2: pos 50, backward jump (-100)
        original[50] = 0xE9;
        core::write_le32(&original[51], 0xFFFFFF9C);
        // Call site 3: pos 120, large jump near 16MB modular cycle
        original[120] = 0xE8;
        core::write_le32(&original[121], 0x00FFFF00);

        std::vector<core::byte> encoded = original;
        Filters50::encode_e8(encoded.data(), encoded.size(), 0x1000, true);

        // Prove that the transform actually altered the operands
        assert(encoded != original);

        // Vector vs scalar parity
        std::vector<core::byte> encoded_scalar = original;
        Filters50::encode_e8_scalar(encoded_scalar.data(), encoded_scalar.size(), 0x1000, true);
        assert(encoded == encoded_scalar);

        // Decode with apply_e8 and assert 100% byte-identity
        std::vector<core::byte> decoded = encoded;
        Filters50::apply_e8(decoded.data(), decoded.size(), 0x1000, true);
        assert(decoded == original);
    }

    // 2. ARM BL round-trip test
    {
        std::vector<core::byte> original(1024);
        for (size_t i = 0; i < original.size(); ++i) {
            original[i] = static_cast<core::byte>((i * 19 + 7) & 0xFF);
        }
        // ARM instructions are 4-byte aligned with 0xEB in byte 3
        original[3] = 0xEB;
        core::write_le32(&original[0], (core::read_le32(&original[0]) & 0xFF000000) | 0x00012345);
        original[19] = 0xEB;
        core::write_le32(&original[16], (core::read_le32(&original[16]) & 0xFF000000) | 0x00FEDCBA);

        std::vector<core::byte> encoded = original;
        Filters50::encode_arm(encoded.data(), encoded.size(), 0x2000);
        assert(encoded != original);

        // Vector vs scalar parity
        std::vector<core::byte> encoded_scalar = original;
        Filters50::encode_arm_scalar(encoded_scalar.data(), encoded_scalar.size(), 0x2000);
        assert(encoded == encoded_scalar);

        // Decode with apply_arm
        std::vector<core::byte> decoded = encoded;
        Filters50::apply_arm(decoded.data(), decoded.size(), 0x2000);
        assert(decoded == original);
    }

    // 3. Delta round-trip test across channels 1..32
    {
        for (core::uint8 ch = 1; ch <= 32; ++ch) {
            std::vector<core::byte> original(512 + ch);
            for (size_t i = 0; i < original.size(); ++i) {
                original[i] = static_cast<core::byte>((i * 23 + ch * 17) & 0xFF);
            }
            std::vector<core::byte> encoded(original.size());
            bool enc_ok =
                Filters50::encode_delta(original.data(), encoded.data(), original.size(), ch);
            assert(enc_ok);

            std::vector<core::byte> decoded(original.size());
            bool dec_ok =
                Filters50::apply_delta(encoded.data(), decoded.data(), original.size(), ch);
            assert(dec_ok);
            assert(decoded == original);

            // In-place encode/decode test
            std::vector<core::byte> in_place = original;
            assert(Filters50::encode_delta(in_place.data(), in_place.data(), in_place.size(), ch));
            assert(Filters50::apply_delta(in_place.data(), in_place.data(), in_place.size(), ch));
            assert(in_place == original);
        }
    }

    // 4. Randomized fuzzing roundtrip for E8/E8E9 (10,000 synthetic sites)
    {
        std::vector<core::byte> fuzz(65536);
        for (size_t i = 0; i < fuzz.size(); ++i) {
            fuzz[i] = static_cast<core::byte>((i * 101 + 43) & 0xFF);
        }
        for (size_t pos = 0; pos + 5 < fuzz.size(); pos += 7) {
            if ((pos % 3) == 0) {
                fuzz[pos] = 0xE8;
            } else if ((pos % 3) == 1) {
                fuzz[pos] = 0xE9;
            }
        }
        std::vector<core::byte> encoded = fuzz;
        Filters50::encode_e8(encoded.data(), encoded.size(), 0x76543210ULL, true);
        std::vector<core::byte> decoded = encoded;
        Filters50::apply_e8(decoded.data(), decoded.size(), 0x76543210ULL, true);
        assert(decoded == fuzz);
    }
    std::cout << "[PASS] RAR 5.0 Forward Filter Transforms & Mathematical Roundtrips\n";
}

void test_bit_reader_and_huffman() {
    core::byte data[4] = {0xAB, 0xCD, 0xEF, 0x12};
    BitReader reader(data, 4);
    assert(reader.get_bits(4) == 0xA);
    assert(reader.get_bits(4) == 0xB);
    assert(reader.get_bits(8) == 0xCD);
    assert(reader.get_bits(0) == 0);
    assert(reader.peek_bits(0) == 0);

    // Canonical Huffman with 4 symbols: lengths 1,2,3,3
    core::byte lens[4] = {1, 2, 3, 3};
    HuffmanDecoder dec;
    assert(dec.build(lens, 4));
    {
        core::byte s0[1] = {0x00}; // code 0 (0b0) -> symbol 0
        BitReader r(s0, 1);
        assert(dec.decode(r) == 0);
        core::byte s1[1] = {0x80}; // code 10xxxxxx -> symbol1 (0b10)
        BitReader r1(s1, 1);
        assert(r1.get_bits(1) == 1); // sanity
        BitReader r1b(s1, 1);
        assert(dec.decode(r1b) == 1);
    }
    // Test >10 bits codes: create alphabet where some symbols have length 12-15
    {
        core::byte lens2[8] = {12, 12, 12, 12, 12, 12, 12, 2};
        // 7 symbols with 12 bits, 1 with 2 bits: forces long codes
        HuffmanDecoder d2;
        assert(d2.build(lens2, 8));
        // Symbol 7 has short code 00, others have long 12-bit codes
        core::byte stream[2] = {0x00, 0x00}; // 00 -> symbol7
        BitReader br(stream, 2);
        assert(d2.decode(br) == 7);
        // Long code path: need to encode symbol0. BuildCodes gives code for lens2
        // Instead just verify decode does not return 0 fallback for long codes
        // Manually craft bits for symbol0: should be distinct from short code, decode should not be 0 fallback
        // We test that building with max bits 15 succeeds
        core::byte lens3[20];
        for (int i = 0; i < 20; i++) lens3[i] = (i % 2 == 0 ? 14 : 15);
        HuffmanDecoder d3;
        assert(d3.build(lens3, 20));
        // Should not assert and decode should not crash
        core::byte tmp[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        BitReader b(tmp, 4);
        core::uint32 sym = d3.decode(b);
        assert(sym < 20);
    }
    std::cout << "[PASS] BitReader & Canonical Huffman Prefix Decoding (quick+slow)\n";
}

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/compress/stream_encoder.hpp"
#include <fstream>
#include <filesystem>

void test_decompressor50() {
    // Test Decompressor50 empty stream decoding
    Decompressor50 unpacker(128 * 1024);
    std::vector<core::byte> out;
    bool ok = unpacker.decompress_to_vector(nullptr, 0, out);
    assert(ok);
    assert(out.empty());
    std::cout << "[PASS] Decompressor50 Initialization and Boundary Decoding\n";
}

void test_archive_roundtrip() {
    namespace fs = std::filesystem;
    std::string test_dir = "build/test_rt_dir";
    fs::create_directories(test_dir);

    std::string src_file = test_dir + "/sample.txt";
    std::string test_data = "Clean-Room RAR 5.0 End-to-End Verification Payload 1234567890\n";
    {
        std::ofstream ofs(src_file, std::ios::binary);
        ofs << test_data;
    }
    std::cout << "Creating sample file... " << std::flush;
    std::string arc_file = test_dir + "/test.rar";
    fs::remove(arc_file);

    std::cout << "Adding file to archive... " << std::flush;
    bool add_ok =
        openrar::archive::ArchiveMutator::add_file_to_archive(arc_file, src_file, "sample.txt");
    std::cout << (add_ok ? "OK\n" : "FAIL\n") << std::flush;
    assert(add_ok);

    std::cout << "Opening archive... " << std::flush;
    openrar::archive::ArchiveReader reader;
    bool open_ok = reader.open(arc_file);
    std::cout << (open_ok ? "OK\n" : "FAIL\n") << std::flush;
    assert(open_ok);
    assert(reader.entries().size() == 1);

    const auto& entry = reader.entries()[0];
    assert(entry.header.file_name == "sample.txt");
    assert(entry.header.unp_size == test_data.size());

    std::cout << "Testing entry... " << std::flush;
    bool test_ok = reader.test_entry(entry);
    std::cout << (test_ok ? "OK\n" : "FAIL\n") << std::flush;
    assert(test_ok);

    std::cout << "Extracting entry... " << std::flush;
    std::string out_file = test_dir + "/extracted.txt";
    fs::remove(out_file);
    bool extract_ok = reader.extract_entry(entry, out_file);
    std::cout << (extract_ok ? "OK\n" : "FAIL\n") << std::flush;
    assert(extract_ok);

    reader.close();
    {
        std::ifstream ifs(out_file, std::ios::binary);
        std::string extracted((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
        assert(extracted == test_data);
    }
    std::error_code ec;
    fs::remove_all(test_dir, ec);
    std::cout << "[PASS] Clean-Room Archive Round-Trip (Mutator + HeaderReader + ArchiveReader + "
                 "CRC32)\n";
}


static std::vector<core::byte> make_patterned_data(size_t size) {
    std::vector<core::byte> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<core::byte>((i * 13 + (i >> 5) * 7 + (i >> 12) * 11) & 0xFF);
    }
    return data;
}


void test_compressor50_roundtrip() {
    std::string text = "The quick brown fox jumps over the lazy dog. The quick brown fox jumps "
                       "over the lazy dog! 1234567890.";
    for (int i = 0; i < 6; i++) text += text; // ~6.5 KB repetitive data

    std::vector<core::byte> compressed;
    bool ok = Compressor50::compress_buffer(reinterpret_cast<const core::byte*>(text.data()),
                                            text.size(), compressed, 3);
    assert(ok);
    assert(!compressed.empty());
    assert(compressed.size() < text.size());

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker;
    bool dec_ok = unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
    assert(dec_ok);
    assert(decompressed.size() == text.size());
    assert(std::memcmp(decompressed.data(), text.data(), text.size()) == 0);

    std::cout << "[PASS] Compressor50 -> Decompressor50 Compression Roundtrip (" << text.size()
              << " -> " << compressed.size() << " bytes)\n";
}

// Regression N.1: the position slide in compress() ran unconditionally,
// including for set_external_buffer()/compress_buffer() small-input paths.
// Sliding pos_base_ without moving buf_data_ shifted every subsequent read
// by that amount, corrupting literals emitted past win_size_ (a 2 MiB 'A'
// run turned the trailing "BC" into "BB"), and the memmove target was
// buf_'s empty vector. The slide is now skipped for external buffers.
// This test reproduces the exact shape (dict-spanning run + tail) and
// fails hard on the old code (first byte diff at offset 2 MiB + tail).
void test_compressor50_external_buffer_no_slide() {
    const size_t WIN_SIZE = 0x200000; // 2 MiB default dictionary
    const size_t RUN_SIZE = WIN_SIZE; // must span the whole window
    std::vector<core::byte> src(RUN_SIZE + 2);
    std::fill(src.begin(), src.begin() + RUN_SIZE, static_cast<core::byte>('A'));
    src[RUN_SIZE] = static_cast<core::byte>('B');
    src[RUN_SIZE + 1] = static_cast<core::byte>('C');

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(src.data(), src.size(), compressed, 3, WIN_SIZE));

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker;
    assert(unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed));
    assert(decompressed.size() == src.size());
    assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);

    // Also confirm the tail itself survives verbatim.
    assert(decompressed[RUN_SIZE] == static_cast<core::byte>('B'));
    assert(decompressed[RUN_SIZE + 1] == static_cast<core::byte>('C'));

    std::cout << "[PASS] Compressor50 external-buffer dict-spanning run roundtrip (" << src.size()
              << " -> " << compressed.size() << " bytes)\n";
}

// Regression (report H1): copy_match's fast path used `distance >= length` as
// its non-overlap guard, but after the first window wrap the source sits
// AHEAD of the destination and the true linear gap is win_size_ - distance.
// A match with distance > win_pos_ and length > win_size - distance made
// memcpy run on overlapping regions (UB; ASan: memcpy-param-overlap).
// Shape: a unique 64-byte pattern at offset 0x13, then WIN_SIZE+3 filler bytes so
// win_pos_ wraps to 3, then the same pattern again — the only possible match
// at that point has distance = WIN_SIZE - 0x10 and length 0x40, while the linear
// source-to-destination gap is only 0x10.
void test_decompressor50_post_wrap_overlap_match() {
    const size_t WIN_SIZE = 0x20000; // 128 KiB minimum dictionary
    const size_t PATTERN_SIZE = 0x40;
    const size_t TAIL_SIZE = 16; // keep the match off the end-of-stream edge
    std::mt19937 rng(0xC0FFEEu);
    std::vector<core::byte> src(WIN_SIZE + 3 + PATTERN_SIZE + TAIL_SIZE);
    for (auto& b : src) b = static_cast<core::byte>(rng() & 0xFF);

    std::vector<core::byte> pat(PATTERN_SIZE);
    for (size_t i = 0; i < PATTERN_SIZE; ++i) pat[i] = static_cast<core::byte>(rng() & 0xFF);
    std::memcpy(src.data() + 0x13, pat.data(), PATTERN_SIZE);
    std::memcpy(src.data() + WIN_SIZE + 3, pat.data(), PATTERN_SIZE);

    // The pattern must occur exactly twice so the second copy's only match is
    // the far one (distance WIN_SIZE - 0x10), forcing the overlap shape.
    size_t occurrences = 0;
    for (size_t i = 0; i + PATTERN_SIZE <= src.size(); ++i) {
        if (std::memcmp(src.data() + i, pat.data(), PATTERN_SIZE) == 0) ++occurrences;
    }
    assert(occurrences == 2);

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(src.data(), src.size(), compressed, 5, WIN_SIZE));

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker(WIN_SIZE);
    assert(unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed));
    assert(decompressed.size() == src.size());
    assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);

    // The far match must actually be in the stream: the second copy region
    // must survive byte-exactly. On MSVC the old code's dst<src overlap is
    // benign (forward memcpy), so deterministic detection of the old UB needs
    // ASan (memcpy-param-overlap); this test locks in the correct path.
    assert(std::memcmp(decompressed.data() + WIN_SIZE + 3, pat.data(), PATTERN_SIZE) == 0);

    std::cout << "[PASS] Decompressor50 post-wrap overlap match roundtrip (" << src.size() << " -> "
              << compressed.size() << " bytes)\n";
}

// Regression (report H1, slow path): a match whose source region crosses the
// window end (src + length > win_size) forces the chunked slow loop with
// chunk = win_size - src. The old wildcopy ran whenever chunk < length, but
// replicating from the head of the written region is byte-exact only when
// chunk >= distance (period = distance). With chunk = win_size - src <
// distance it replicated the wrong period and corrupted the output.
// Geometry: win 0x20000; 0x10 literals after the wrap (win_pos_ = 0x10), then
// a 0x1010-byte copy of first-pass bytes at distance 0x1000. src = 0x1F010,
// src + length > win_size, chunk = 0xFF0 - shift < distance for any match
// start the fail-count heuristic shifts by 0..31 — so the test does not
// depend on where exactly the match begins.
void test_decompressor50_slow_path_wildcopy_wrap() {
    const size_t WIN_SIZE = 0x20000;
    const size_t DISTANCE = 0x1000;
    const size_t LEAD_SIZE = 0x10;   // literals after the wrap: win_pos_ at match time
    const size_t COPY_SIZE = 0x1010; // match region length (>= DISTANCE - LEAD_SIZE + 0x20)
    const size_t TAIL_SIZE = 16;
    std::mt19937 rng(0xBADF00Du);
    std::vector<core::byte> src(WIN_SIZE + LEAD_SIZE + COPY_SIZE + TAIL_SIZE);
    for (auto& b : src) b = static_cast<core::byte>(rng() & 0xFF);

    // stream[W+LEAD_SIZE .. W+LEAD_SIZE+COPY_SIZE) = stream[W+LEAD_SIZE-DISTANCE .. +DISTANCE): a
    // DISTANCE-distant copy whose source crosses the window end (it includes the
    // LEAD_SIZE wrapped bytes). memmove: source and destination overlap by LEAD_SIZE.
    std::memmove(&src[WIN_SIZE + LEAD_SIZE], &src[WIN_SIZE + LEAD_SIZE - DISTANCE], COPY_SIZE);

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(src.data(), src.size(), compressed, 5, WIN_SIZE));

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker(WIN_SIZE);
    assert(unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed));
    assert(decompressed.size() == src.size());
    assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);

    // The match region must survive byte-exactly (old code garbled it).
    assert(std::memcmp(decompressed.data() + WIN_SIZE + LEAD_SIZE,
                       src.data() + WIN_SIZE + LEAD_SIZE, COPY_SIZE) == 0);

    std::cout << "[PASS] Decompressor50 slow-path wildcopy window-end wrap roundtrip ("
              << src.size() << " -> " << compressed.size() << " bytes)\n";
}

// Regression: previously, a Compressor50 instance that had been used with
// set_external_buffer() (or the memory-input path inside compress_buffer)
// would latch external_buf_==true and stale mem_src_* pointers. Reusing
// that instance for a subsequent begin_archive() session silently produced
// corrupt output because load_data() short-circuited. reset_state() called
// from both the constructor and begin_archive() now prevents the leak.
void test_compressor50_reset_after_external_buffer() {
    std::string payload_a = "AAAAAA1111111122222222";
    for (int i = 0; i < 6; i++) payload_a += payload_a; // ~1.4 KB
    std::string payload_b = "BBBBBB3333333344444444";
    for (int i = 0; i < 6; i++) payload_b += payload_b;

    Compressor50 packer;
    // First session: external buffer.
    std::vector<core::byte> out_a;
    packer.begin_archive(nullptr, 3, 0x200000);
    packer.set_external_buffer(reinterpret_cast<const core::byte*>(payload_a.data()),
                               payload_a.size());
    packer.set_memory_dest(&out_a);
    packer.compress();

    // Second session on the SAME instance, this time via internal buffer +
    // memory source. Before the fix external_buf_ was still true and load
    // was a no-op → out_b would decompress to zeros.
    std::vector<core::byte> out_b;
    packer.begin_archive(nullptr, 3, 0x200000);
    packer.set_external_buffer(reinterpret_cast<const core::byte*>(payload_b.data()),
                               payload_b.size());
    packer.set_memory_dest(&out_b);
    packer.compress();

    std::vector<core::byte> dec_a, dec_b;
    Decompressor50 unpacker;
    bool ok_a = unpacker.decompress_to_vector(out_a.data(), out_a.size(), dec_a);
    Decompressor50 unpacker_b;
    bool ok_b = unpacker_b.decompress_to_vector(out_b.data(), out_b.size(), dec_b);
    assert(ok_a && ok_b);
    assert(dec_a.size() == payload_a.size());
    assert(dec_b.size() == payload_b.size());
    assert(std::memcmp(dec_a.data(), payload_a.data(), payload_a.size()) == 0);
    assert(std::memcmp(dec_b.data(), payload_b.data(), payload_b.size()) == 0);

    std::cout << "[PASS] Compressor50 reuse across sessions after set_external_buffer\n";
}

// Regression (found on a real 16.7 MB source-code file): a match whose source
// region wraps the window end AND whose first chunk is clamped to the window
// edge (chunk = win_size_ - src) leaves src re-entered BEHIND dst for the
// remainder. The per-match gap computed before the chunked loop is then stale
// by a full window, the next memcpy overtakes its own source, and memmove's
// as-if-through-temp semantics substitute a stale pre-match byte for the
// just-written one — exactly one wrong byte in the output. Geometry: win
// 0x20000; pattern P (0x1C bytes, period 0x1A self-overlap) occurs at W-1 and
// W+0x19 only, so the match at win_pos_ = 0x19 has dist 0x1A, src = W-1, first
// chunk = 1, and the final byte (k=0x1B) must copy the just-written win[0x1A]
// (= P[1]), not the stale filler byte planted at abs 0x1A.
void test_decompressor50_wrapped_src_stale_gap() {
    const size_t WIN_SIZE = 0x20000;
    const size_t PERIOD = 0x1A;
    const size_t PATTERN_SIZE = 0x1C; // > PERIOD: the match self-overlaps
    const size_t LEAD = 0x19;         // literals after the wrap: win_pos_ = LEAD
    const size_t TAIL_SIZE = 16;
    std::mt19937 rng(0xD00DADu);
    std::vector<core::byte> src(WIN_SIZE + LEAD + PATTERN_SIZE + TAIL_SIZE);
    // Leading filler is 40-byte periodic: the compressor finds matches there
    // continuously, keeping fail_count_ low so the FailCount skip heuristic
    // never suppresses the chain search at the pattern (with random filler the
    // heuristic goes to a 1-in-32 probe stride and skips the bug shape).
    std::vector<core::byte> block(40);
    for (auto& b : block) b = static_cast<core::byte>(rng() & 0xFF);
    for (size_t i = 0; i < WIN_SIZE - 1; ++i) src[i] = block[i % block.size()];

    std::vector<core::byte> pat(PERIOD);
    for (size_t i = 0; i < PERIOD; ++i) pat[i] = static_cast<core::byte>(rng() & 0xFF);
    // Matched pattern P = one period + the first two bytes again (28 bytes) so
    // the match self-overlaps by PERIOD bytes. The file region [W-1,
    // W-1+PERIOD+PATTERN_SIZE) is pat repeated with period PERIOD, so P occurs
    // at W-1 and at W+LEAD (= W-1+PERIOD) and nowhere else.
    std::vector<core::byte> big(PATTERN_SIZE);
    for (size_t i = 0; i < PATTERN_SIZE; ++i) big[i] = pat[i % PERIOD];
    for (size_t k = 0; k < PERIOD + PATTERN_SIZE; ++k) src[WIN_SIZE - 1 + k] = pat[k % PERIOD];
    // Poison the stale byte the old bug would surface: pre-match win[0x1A]
    // (abs 0x1A, inside the periodic filler) must differ from the correct
    // just-written value P[1].
    src[0x1A] = static_cast<core::byte>(big[1] ^ 0xFF);

    size_t occurrences = 0;
    for (size_t i = 0; i + PATTERN_SIZE <= src.size(); ++i) {
        if (std::memcmp(src.data() + i, big.data(), PATTERN_SIZE) == 0) ++occurrences;
    }
    assert(occurrences == 2);

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(src.data(), src.size(), compressed, 5, WIN_SIZE));

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker(WIN_SIZE);
    assert(unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed));
    assert(decompressed.size() == src.size());
    assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);

    std::cout << "[PASS] Decompressor50 wrapped-source stale-gap match roundtrip (" << src.size()
              << " -> " << compressed.size() << " bytes)\n";
}

#include "../../src/core/cpu.hpp"

void test_cpu_features() {
    const auto& cpu = core::get_cpu_features();
    std::cout << "[PASS] CPU Detection:";
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (cpu.sse2) std::cout << " SSE2";
    if (cpu.avx2) std::cout << " AVX2";
    if (cpu.aes_ni) std::cout << " AES-NI";
    if (cpu.pclmulqdq) std::cout << " PCLMUL";
    if (cpu.sha_ni) std::cout << " SHA-NI";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (cpu.neon) std::cout << " NEON";
    if (cpu.arm_crc32) std::cout << " CRC32";
    if (cpu.arm_aes) std::cout << " AES";
    if (cpu.arm_sha2) std::cout << " SHA2";
    if (cpu.arm_pmull) std::cout << " PMULL";
#endif
    std::cout << "\n";
}

// R12: verify build_lengths handles large→small→large alphabet transitions
// correctly after the shrink heuristic fires (capacity ≥4× needed → shrink_to_fit).
void test_huffman_builder_shrink() {
    std::cout << "  test_huffman_builder_shrink..." << std::flush;

    // Step 1: build with a large alphabet (512 symbols) to inflate thread_local vectors
    const core::uint32 LARGE = 512;
    std::vector<core::uint32> freq_large(LARGE, 0);
    std::vector<core::byte> len_large(LARGE, 0);
    for (core::uint32 i = 0; i < LARGE; ++i) freq_large[i] = i + 1;
    HuffmanBuilder::build_lengths(freq_large.data(), LARGE, 15, len_large.data());

    // Verify: every symbol with non-zero freq must get a non-zero length
    for (core::uint32 i = 0; i < LARGE; ++i) {
        assert(len_large[i] > 0 && len_large[i] <= 15);
    }

    // Step 2: call repeatedly with a small alphabet (16 symbols, <LARGE/4)
    // This should trigger the shrink heuristic on the second call
    const core::uint32 SMALL = 16;
    std::vector<core::uint32> freq_small(SMALL, 0);
    std::vector<core::byte> len_small(SMALL, 0);
    for (core::uint32 i = 0; i < SMALL; ++i) freq_small[i] = (i + 1) * 10;

    for (int rep = 0; rep < 5; ++rep) {
        std::memset(len_small.data(), 0, SMALL);
        HuffmanBuilder::build_lengths(freq_small.data(), SMALL, 15, len_small.data());
        for (core::uint32 i = 0; i < SMALL; ++i) {
            assert(len_small[i] > 0 && len_small[i] <= 15);
        }
    }

    // Step 3: go back to large — vectors must re-grow correctly
    std::memset(len_large.data(), 0, LARGE);
    HuffmanBuilder::build_lengths(freq_large.data(), LARGE, 15, len_large.data());
    for (core::uint32 i = 0; i < LARGE; ++i) {
        assert(len_large[i] > 0 && len_large[i] <= 15);
    }

    // Step 4: verify round-trip via build_codes
    std::vector<core::uint32> codes(LARGE, 0);
    HuffmanBuilder::build_codes(len_large.data(), LARGE, codes.data());
    // Kraft inequality: sum of 2^(-len) must equal 1 for a complete code
    double kraft = 0;
    for (core::uint32 i = 0; i < LARGE; ++i) {
        if (len_large[i] > 0) {
            kraft += 1.0 / (1 << len_large[i]);
        }
    }
    assert(kraft > 0.99 && kraft <= 1.001);

    std::cout << " OK\n";
}
void test_forced_scalar_filters() {
    std::vector<core::byte> buf_simd(65536);
    core::uint32 seed = 42;
    for (size_t i = 0; i < buf_simd.size(); ++i) {
        seed = seed * 1103515245 + 12345;
        buf_simd[i] = static_cast<core::byte>((seed >> 16) & 0xFF);
    }
    // inject some matches
    buf_simd[12] = 0xE8;
    buf_simd[13] = 0x01;
    buf_simd[14] = 0x02;
    buf_simd[15] = 0x03;
    buf_simd[16] = 0x04;
    buf_simd[100] = 0xE9;
    buf_simd[101] = 0xFF;
    buf_simd[102] = 0xFF;
    buf_simd[103] = 0xFF;
    buf_simd[104] = 0xFF;
    buf_simd[503] = 0xEB; // For ARM

    std::vector<core::byte> buf_scalar = buf_simd;

    Filters50::apply_e8(buf_simd.data(), buf_simd.size(), 0x1234, true);
    Filters50::apply_e8_scalar(buf_scalar.data(), buf_scalar.size(), 0x1234, true);
    assert(buf_simd == buf_scalar);

    std::vector<core::byte> buf_arm_simd = buf_simd;
    std::vector<core::byte> buf_arm_scalar = buf_scalar;
    Filters50::apply_arm(buf_arm_simd.data(), buf_arm_simd.size(), 0x1234);
    Filters50::apply_arm_scalar(buf_arm_scalar.data(), buf_arm_scalar.size(), 0x1234);
    assert(buf_arm_simd == buf_arm_scalar);

    std::cout << "[PASS] RAR 5.0 Forced-Scalar Filter vs SIMD Bit-for-Bit\n";
}

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
size_t get_peak_rss() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.PeakWorkingSetSize;
    }
    return 0;
}
#else
size_t get_peak_rss() {
    return 0;
}
#endif

// Regression: match_length_simd must count every byte of [i, cap). With
// caps of 9..15 (mod 16), the old AVX2 tail compared only the final 8 bytes
// at cap-8 and returned cap while bytes [i, cap-8) were unverified.
void test_match_length_tail_caps() {
    alignas(32) core::byte p[128];
    alignas(32) core::byte q[128];
    for (core::uint32 cap = 1; cap <= 96; ++cap) {
        // Fully equal pair: must return cap exactly.
        std::memset(p, 0x5A, sizeof(p));
        std::memcpy(q, p, sizeof(p));
        assert(arch::match_length_simd(p, q, cap) == cap);
        assert(arch::match_length_simd(q, p, cap) == cap);

        // Mismatch in the final block region: must spot it.
        std::memcpy(q, p, sizeof(p));
        q[cap - 1] = static_cast<core::byte>(p[cap - 1] ^ 1);
        assert(arch::match_length_simd(p, q, cap) == cap - 1);
        assert(arch::match_length_simd(q, p, cap) == cap - 1);

        // The dangerous zone: caps with cap mod 16 in [9..15] had an
        // unverified gap between the SIMD blocks and the old 8-byte tail.
        // Any mismatch inside it must give the exact mismatch offset.
        for (size_t diff_at = 32; diff_at <= cap - 1; ++diff_at) {
            std::memcpy(q, p, sizeof(p));
            q[diff_at] = static_cast<core::byte>(p[diff_at] ^ 0xFF);
            size_t got = arch::match_length_simd(p, q, cap);
            assert(got == diff_at);
        }
    }
    std::cout << "[PASS] match_length tail covers every byte of the cap\n";
}

void test_decompressor_lazy_alloc() {
    size_t initial_rss = get_peak_rss();

    // Construct with 1 GiB window, but don't call decompress.
    Decompressor50 dec(1ULL * 1024 * 1024 * 1024);

    size_t new_rss = get_peak_rss();
    if (initial_rss > 0) {
        assert(new_rss - initial_rss < 500 * 1024 * 1024);
    }

#if !defined(__EMSCRIPTEN__) && !defined(__wasm__) && !defined(_M_IX86) && !defined(__i386__)
    // 64-bit platforms: test lazy instantiation with 2G, 4G, 8G, 16G, 64G windows
    const core::uint64 large_windows[] = {2ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024,
                                          8ULL * 1024 * 1024 * 1024, 16ULL * 1024 * 1024 * 1024,
                                          64ULL * 1024 * 1024 * 1024};
    for (core::uint64 w : large_windows) {
        Decompressor50 dec_large(static_cast<size_t>(w));
        assert(!dec_large.is_dictionary_too_large());
        assert(dec_large.last_error() == DecompressErrorCode::Ok);
    }
#endif

    Decompressor50 dec_huge(Decompressor50::ALLOC_LIMIT + 1);
    assert(dec_huge.is_dictionary_too_large());
    assert(dec_huge.last_error() == DecompressErrorCode::DictionaryTooLarge);

    core::byte dummy_src[10] = {0};
    bool ok = dec_huge.decompress(dummy_src, 10, 10);
    assert(!ok);
    assert(dec_huge.is_dictionary_too_large());

    std::cout << "[PASS] RAR 5.0 Decompressor Lazy Allocation\n";
}

void test_bit_reader_get_bits64() {
    std::vector<core::byte> buf(32);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<core::byte>((i * 17 + 0x2B) & 0xFF);
    }

    // Test reading every bit count from 33 to 64
    for (unsigned int bits = 33; bits <= 64; ++bits) {
        // Reference: bit-by-bit
        core::uint64 expected = 0;
        BitReader r_ref(buf.data(), buf.size());
        for (unsigned int b = 0; b < bits; ++b) {
            expected = (expected << 1) | r_ref.get_bits(1);
        }

        BitReader r_test(buf.data(), buf.size());
        core::uint64 actual = r_test.get_bits64(bits);
        assert(actual == expected);
        assert(r_test.bit_pos() == bits);
    }

    // Test reading unaligned 33..56 bit values (after initial 1..7 offset bits)
    for (unsigned int pre = 1; pre <= 7; ++pre) {
        for (unsigned int bits = 33; bits <= 56; ++bits) {
            core::uint64 expected = 0;
            BitReader r_ref(buf.data(), buf.size());
            r_ref.get_bits(pre);
            for (unsigned int b = 0; b < bits; ++b) {
                expected = (expected << 1) | r_ref.get_bits(1);
            }

            BitReader r_test(buf.data(), buf.size());
            r_test.get_bits(pre);
            core::uint64 actual = r_test.get_bits64(bits);
            assert(actual == expected);
            assert(r_test.bit_pos() == pre + bits);
        }
    }

    // Explicit 64-bit value check
    core::byte raw64[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    BitReader r64(raw64, 8);
    assert(r64.get_bits64(64) == 0x0123456789ABCDEFULL);

    std::cout << "[PASS] BitReader get_bits64 across 33-64 bit values\n";
}

void test_extra_distance_slot_decoding() {
    core::byte test_stream[16] = {0xA5, 0x5A, 0xF0, 0x0F, 0x33, 0xCC, 0x55, 0xAA,
                                  0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

    // Simulate slot 76 extra distance read (33 bits)
    BitReader reader76(test_stream, sizeof(test_stream));
    core::uint32 dist_slot = 76;
    core::uint32 d_bits = dist_slot / 2 - 1; // 37
    assert(d_bits == 37);
    assert(d_bits > 36);
    core::uint32 extra_bits = d_bits - 4; // 33 bits
    assert(extra_bits == 33);

    core::uint64 extra = reader76.get_bits64(extra_bits);
    assert((extra >> 32) != 0);

    core::uint64 distance = 1;
    distance += static_cast<core::uint64>(2 | (dist_slot & 1)) << d_bits;
    distance += extra << 4;
    assert(distance > (4ULL * 1024 * 1024 * 1024));

    // Simulate slot 78 extra distance read (34 bits)
    BitReader reader78(test_stream, sizeof(test_stream));
    dist_slot = 78;
    d_bits = dist_slot / 2 - 1; // 38
    assert(d_bits == 38);
    extra_bits = d_bits - 4; // 34 bits
    assert(extra_bits == 34);

    extra = reader78.get_bits64(extra_bits);
    assert((extra >> 32) != 0);
    distance = 1;
    distance += static_cast<core::uint64>(2 | (dist_slot & 1)) << d_bits;
    distance += extra << 4;
    assert(distance > (4ULL * 1024 * 1024 * 1024));

    std::cout << "[PASS] Extra-distance slots 68-79 64-bit distance decoding (> 4 GiB)\n";
}

namespace openrar::compress {
class Compressor50TestAccess {
public:
    static bool is_large_window(const Compressor50& c) { return c.is_large_window_; }
    static size_t head_size(const Compressor50& c) { return c.head_.size(); }
    static size_t prev_size(const Compressor50& c) { return c.prev_.size(); }
    static size_t head64_size(const Compressor50& c) { return c.head64_.size(); }
    static size_t prev64_size(const Compressor50& c) { return c.prev64_.size(); }

    static void setup_mock_large_window(Compressor50& c, core::uint64 max_dist,
                                        size_t win_mask_size) {
        c.reset_state();
        c.is_large_window_ = true;
        c.win_size_ = win_mask_size;
        c.max_dist_ = max_dist;
        c.head64_.assign(Compressor50::HASH_SIZE, static_cast<core::uint64>(-1));
        c.prev64_.assign(win_mask_size, static_cast<core::uint64>(-1));
    }

    static void insert_pos(Compressor50& c, core::uint64 pos) { c.insert_position(pos); }

    static core::int64 reconstruct_pos(Compressor50& c, core::uint64 pos, core::uint32 trunc) {
        return c.reconstruct_pos(pos, trunc);
    }

    static core::uint64 get_head64(const Compressor50& c, size_t idx) { return c.head64_[idx]; }
    static core::uint64 get_prev64(const Compressor50& c, size_t idx) { return c.prev64_[idx]; }
    static core::uint32 calc_hash(Compressor50& c, core::uint64 pos) { return c.calc_hash(pos); }

    static void set_test_buffer(Compressor50& c, const core::byte* data, size_t size,
                                core::uint64 pos_base, core::uint64 src_loaded) {
        c.buf_data_ = data;
        c.buf_size_ = size;
        c.pos_base_ = pos_base;
        c.src_loaded_ = src_loaded;
    }
};
} // namespace openrar::compress

void test_compressor50_large_window_match_finding() {
    // 1. Verify standard window (<= 4 GiB) allocates 32-bit tables and leaves 64-bit tables empty
    {
        Compressor50 c;
        c.begin_archive(nullptr, 3, 2 * 1024 * 1024);
        assert(!Compressor50TestAccess::is_large_window(c));
        assert(Compressor50TestAccess::head_size(c) == 131072);
        assert(Compressor50TestAccess::prev_size(c) == 2 * 1024 * 1024);
        assert(Compressor50TestAccess::head64_size(c) == 0);
        assert(Compressor50TestAccess::prev64_size(c) == 0);

        // 32-bit reconstruct_pos cannot reach candidates > 4 GiB back:
        core::uint64 pos = 6ULL * 1024 * 1024 * 1024 + 500;
        core::uint32 trunc = 500;
        core::int64 cand = Compressor50TestAccess::reconstruct_pos(c, pos, trunc);
        assert(cand != 500); // Truncation in 32-bit reconstruct_pos prevents reaching 500
    }

    // 2. Verify 64-bit match finding across > 4 GiB horizons and hash chains
    {
        Compressor50 c;
        const size_t MOCK_WIN_MASK_SIZE = 65536;                  // 64K entries
        const core::uint64 MAX_DIST = 16ULL * 1024 * 1024 * 1024; // 16 GiB reach
        Compressor50TestAccess::setup_mock_large_window(c, MAX_DIST, MOCK_WIN_MASK_SIZE);

        assert(Compressor50TestAccess::is_large_window(c));
        assert(Compressor50TestAccess::head_size(c) == 0);
        assert(Compressor50TestAccess::prev_size(c) == 0);
        assert(Compressor50TestAccess::head64_size(c) == 131072);
        assert(Compressor50TestAccess::prev64_size(c) == MOCK_WIN_MASK_SIZE);

        core::byte test_buf[64] = {'T', 'E', 'S', 'T', 0};

        // First insertion at pos1 = 200
        const core::uint64 pos1 = 200;
        Compressor50TestAccess::set_test_buffer(c, test_buf, 64, pos1, pos1 + 10);
        core::uint32 h = Compressor50TestAccess::calc_hash(c, pos1);
        Compressor50TestAccess::insert_pos(c, pos1);
        assert(Compressor50TestAccess::get_head64(c, h) == pos1);

        // Second insertion at pos2 = 5 GiB + 200 (distance = 5 GiB > 4 GiB)
        const core::uint64 pos2 = 5ULL * 1024 * 1024 * 1024 + 200;
        Compressor50TestAccess::set_test_buffer(c, test_buf, 64, pos2, pos2 + 10);
        Compressor50TestAccess::insert_pos(c, pos2);

        // Head now points to pos2, and prev64_ chain links pos2 to pos1
        assert(Compressor50TestAccess::get_head64(c, h) == pos2);
        assert(Compressor50TestAccess::get_prev64(c, pos2 & (MOCK_WIN_MASK_SIZE - 1)) == pos1);

        // Verify distance calculations across the 64-bit chain:
        // Candidate at head (pos2) is distance 6 GiB from pos3 = 11 GiB + 200
        const core::uint64 pos3 = 11ULL * 1024 * 1024 * 1024 + 200;
        core::uint64 cand1 = Compressor50TestAccess::get_head64(c, h);
        assert(cand1 == pos2);
        assert(pos3 - cand1 == 6ULL * 1024 * 1024 * 1024);
        assert(pos3 - cand1 > 4ULL * 1024 * 1024 * 1024);
        assert(pos3 - cand1 <= MAX_DIST);

        // Chained candidate in prev64_ (pos1) is distance 11 GiB from pos3
        core::uint64 cand2 =
            Compressor50TestAccess::get_prev64(c, cand1 & (MOCK_WIN_MASK_SIZE - 1));
        assert(cand2 == pos1);
        assert(pos3 - cand2 == 11ULL * 1024 * 1024 * 1024);
        assert(pos3 - cand2 > 4ULL * 1024 * 1024 * 1024);
        assert(pos3 - cand2 <= MAX_DIST);
    }

    std::cout << "[PASS] Compressor50 64-bit large window match-finding horizon (> 4 GiB)\n";
}


// Test 6: Verify BC table RLE encoding for sparse trees.
// A pure 0x00 file, and a patterned file with a 64 KiB embedded run.
void test_compressor50_all_zeros_8mib() {
    constexpr size_t WIN_SIZE = 0x200000;
    constexpr size_t DATA_SIZE = 8 * 1024 * 1024; // 8 MiB

    // Case A: Pure 0x00 filled (maximum compression, dist=1 runs)
    {
        std::vector<core::byte> src(DATA_SIZE, 0x00);
        std::vector<core::byte> compressed;
        bool ok = Compressor50::compress_buffer(src.data(), src.size(), compressed, 3, WIN_SIZE);
        assert(ok);
        assert(compressed.size() > 4);

        std::vector<core::byte> decompressed;
        Decompressor50 unpacker;
        bool dec_ok =
            unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
        assert(dec_ok);
        assert(decompressed.size() == src.size());
        assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);
    }

    // Case B: Embedded 64 KiB 0xAA run in patterned data
    {
        auto src = make_patterned_data(DATA_SIZE);
        size_t run_offset = 1830000; // Place it near the 1.8MB mark
        size_t run_size = 65536;     // 64 KiB
        std::memset(src.data() + run_offset, 0xAA, run_size);

        std::vector<core::byte> compressed;
        bool ok = Compressor50::compress_buffer(src.data(), src.size(), compressed, 3, WIN_SIZE);
        assert(ok);
        assert(compressed.size() > 4);

        std::vector<core::byte> decompressed;
        Decompressor50 unpacker;
        bool dec_ok =
            unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
        assert(dec_ok);
        assert(decompressed.size() == src.size());
        bool match = true;
        for (size_t i = 0; i < src.size(); i++) {
            if (decompressed[i] != src[i]) {
                std::cout << "MISMATCH AT OFFSET " << i << " (src=" << (int)src[i]
                          << " dec=" << (int)decompressed[i] << ")\n"
                          << std::flush;
                std::cout << "SRC near mismatch: ";
                for (size_t k = (i > 16 ? i - 16 : 0); k < std::min(src.size(), i + 16); k++) {
                    std::cout << (int)src[k] << " ";
                }
                std::cout << "\nDEC near mismatch: ";
                for (size_t k = (i > 16 ? i - 16 : 0); k < std::min(decompressed.size(), i + 16);
                     k++) {
                    std::cout << (int)decompressed[k] << " ";
                }
                std::cout << "\n" << std::flush;
                match = false;
                break;
            }
        }
        assert(match);
    }

    std::cout << "[PASS] Compressor50 sparse BD table RLE encoding (8 MiB zeros)\n";
}

void test_compressor50_boundary_sizes() {
    constexpr size_t WIN_SIZE = 0x200000;
    const size_t sizes[] = {
        0, 1, 100, WIN_SIZE, WIN_SIZE + 1, WIN_SIZE + 0x400000, WIN_SIZE + 0x400001};
    for (size_t sz : sizes) {
        if (sz == 0) continue;
        auto src = make_patterned_data(sz);
        std::vector<core::byte> compressed;
        bool ok = Compressor50::compress_buffer(src.data(), src.size(), compressed, 3, WIN_SIZE);
        assert(ok);
        assert(compressed.size() > 4);

        std::vector<core::byte> decompressed;
        Decompressor50 unpacker;
        bool dec_ok =
            unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
        assert(dec_ok);
        assert(decompressed.size() == src.size());
        assert(std::memcmp(decompressed.data(), src.data(), src.size()) == 0);
    }
    std::cout << "[PASS] Compressor50 Boundary Sizes Coverage\n";
}

void test_compressor50_filestream_roundtrip() {
    constexpr size_t DATA_SIZE = 8 * 1024 * 1024;
    constexpr size_t WIN_SIZE = 0x200000;
    auto src_data = make_patterned_data(DATA_SIZE);

    std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "rar_test_filestream.bin";
    {
        io::FileStream out;
        assert(out.open(temp_file, io::FileMode::CreateAlways));
        assert(out.write(src_data.data(), src_data.size()) == src_data.size());
        out.close();
    }

    std::vector<core::byte> compressed;
    {
        io::FileStream in;
        assert(in.open(temp_file, io::FileMode::ReadOnly));
        Compressor50 packer;
        packer.init(&in, nullptr, 3, DATA_SIZE, WIN_SIZE);
        packer.set_memory_dest(&compressed);
        core::int64 packed = packer.compress();
        assert(packed > 0);
        assert(packer.source_bytes_loaded() == DATA_SIZE);
        in.close();
    }
    assert(compressed.size() > 4);

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker;
    bool dec_ok = unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
    assert(dec_ok);
    assert(decompressed.size() == DATA_SIZE);
    assert(std::memcmp(decompressed.data(), src_data.data(), DATA_SIZE) == 0);

    std::filesystem::remove(temp_file);
    std::cout << "[PASS] Compressor50 FileStream Streaming Roundtrip (8 MiB)\n";
}

void test_compressor50_truncated_source() {
    constexpr size_t ORIGINAL_SIZE = 1024 * 1024; // 1 MiB
    auto src_data = make_patterned_data(ORIGINAL_SIZE);

    std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "rar_test_truncated.bin";
    {
        io::FileStream out;
        assert(out.open(temp_file, io::FileMode::CreateAlways));
        assert(out.write(src_data.data(), src_data.size()) == src_data.size());
        out.close();
    }

    // Open for reading
    io::FileStream in;
    assert(in.open(temp_file, io::FileMode::ReadOnly));

    // Truncate the file via a second stream opened with CreateAlways to 100 KiB
    {
        io::FileStream trunc_out;
        if (trunc_out.open(temp_file, io::FileMode::CreateAlways)) {
            constexpr size_t TRUNCATED_SIZE = 100 * 1024;
            trunc_out.write(src_data.data(), TRUNCATED_SIZE);
            trunc_out.close();
        }
    }

    std::vector<core::byte> compressed;
    Compressor50 packer;
    packer.init(&in, nullptr, 3, ORIGINAL_SIZE, 0x200000);
    packer.set_memory_dest(&compressed);
    core::int64 result = packer.compress();

    // Verification (L13): a source shorter than its declared size must fail
    // the compression outright — pre-fix compress() kept emitting literals
    // from the uninitialized tail of buf_ and reported success.
    assert(result < 0);
    assert(packer.source_bytes_loaded() < ORIGINAL_SIZE);
    in.close();

    std::filesystem::remove(temp_file);
    std::cout << "[PASS] Compressor50 Truncated Source Protection\n";
}

void test_compressor50_streaming_feed_large_chunks() {
    constexpr size_t DATA_SIZE = 5 * 1024 * 1024; // 5 MiB
    auto src_data = make_patterned_data(DATA_SIZE);

    StreamEncoder encoder(3, 0x200000); // 2 MiB dict

    size_t chunk_size = 3 * 1024 * 1024; // 3 MiB chunk (> dict size)
    size_t offset = 0;
    while (offset < DATA_SIZE) {
        size_t len = std::min(chunk_size, DATA_SIZE - offset);
        bool ok = encoder.feed(src_data.data() + offset, len);
        assert(ok);
        offset += len;
    }

    std::vector<core::byte> compressed;
    bool fin_ok = encoder.finish(compressed);
    assert(fin_ok);
    assert(compressed.size() > 4);

    std::vector<core::byte> decompressed;
    Decompressor50 unpacker;
    bool dec_ok = unpacker.decompress_to_vector(compressed.data(), compressed.size(), decompressed);
    assert(dec_ok);
    assert(decompressed.size() == DATA_SIZE);
    assert(std::memcmp(decompressed.data(), src_data.data(), DATA_SIZE) == 0);

    std::cout << "[PASS] Compressor50 Streaming Feed Large Chunks\n";
}

void test_chunking_invariance() {
    std::cout << "  - test_chunking_invariance... ";
    constexpr size_t DATA_SIZE = 100 * 1024;
    auto src_data = make_patterned_data(DATA_SIZE);

    // Baseline compress
    std::vector<core::byte> comp_base;
    bool ok = Compressor50::compress_buffer(src_data.data(), src_data.size(), comp_base);
    assert(ok);

    // Chunked compress
    std::vector<core::byte> comp_chunked;
    StreamEncoder encoder(3, 0x100000); // 1 MB dict
    size_t offset = 0;
    while (offset < DATA_SIZE) {
        size_t len = (offset % 7919) + 1; // prime sized chunks
        if (offset + len > DATA_SIZE) len = DATA_SIZE - offset;
        encoder.feed(src_data.data() + offset, len);
        offset += len;
    }
    encoder.finish(comp_chunked);

    // Baseline decompress
    std::vector<core::byte> dec_base;
    Decompressor50 dec1;
    dec1.decompress_to_vector(comp_base.data(), comp_base.size(), dec_base);
    assert(dec_base.size() == DATA_SIZE);
    assert(std::memcmp(dec_base.data(), src_data.data(), DATA_SIZE) == 0);

    // Verify chunked encode decompresses perfectly
    std::vector<core::byte> dec_chunked_enc;
    Decompressor50 dec2;
    dec2.decompress_to_vector(comp_chunked.data(), comp_chunked.size(), dec_chunked_enc);
    assert(dec_chunked_enc.size() == DATA_SIZE);
    assert(std::memcmp(dec_chunked_enc.data(), src_data.data(), DATA_SIZE) == 0);

    // Chunked decompress of comp_base
    std::vector<core::byte> dec_chunked_dec;
    Decompressor50 dec3;
    size_t in_offset = 0;
    BitReader::InputCallback cb = [&](core::byte* buf, size_t max_size) -> size_t {
        if (in_offset >= comp_base.size()) return 0;
        size_t len = (in_offset % 313) + 1; // weird chunk sizes
        if (len > max_size) len = max_size;
        if (in_offset + len > comp_base.size()) len = comp_base.size() - in_offset;
        std::memcpy(buf, comp_base.data() + in_offset, len);
        in_offset += len;
        return len;
    };

    auto flush_cb = [&](const core::byte* data, size_t size) -> bool {
        dec_chunked_dec.insert(dec_chunked_dec.end(), data, data + size);
        return true;
    };
    bool dec3_ok = dec3.decompress(cb, comp_base.size(), DATA_SIZE, false, flush_cb);
    assert(dec3_ok && dec_chunked_dec.size() == DATA_SIZE);
    assert(std::memcmp(dec_chunked_dec.data(), src_data.data(), DATA_SIZE) == 0);

    std::cout << "OK\n" << std::flush;
}

void test_recompress_stability() {
    std::cout << "  - test_recompress_stability... ";
    constexpr size_t DATA_SIZE = 100 * 1024;
    auto src_data = make_patterned_data(DATA_SIZE);

    std::vector<core::byte> comp1;
    Compressor50::compress_buffer(src_data.data(), src_data.size(), comp1);

    std::vector<core::byte> dec1;
    Decompressor50 unpacker1;
    unpacker1.decompress_to_vector(comp1.data(), comp1.size(), dec1);

    std::vector<core::byte> comp2;
    Compressor50::compress_buffer(dec1.data(), dec1.size(), comp2);

    assert(comp1.size() == comp2.size());
    assert(std::memcmp(comp1.data(), comp2.data(), comp1.size()) == 0);

    std::cout << "OK\n" << std::flush;
}

void test_truncation_sweep() {
    std::cout << "  - test_truncation_sweep... ";
    constexpr size_t DATA_SIZE = 10 * 1024;
    auto src_data = make_patterned_data(DATA_SIZE);

    std::vector<core::byte> comp;
    Compressor50::compress_buffer(src_data.data(), src_data.size(), comp);

    for (size_t trunc = 1; trunc < comp.size(); trunc += 64) {
        std::vector<core::byte> dec;
        Decompressor50 unpacker;
        // It should either succeed (partial) or fail, but NEVER crash
        unpacker.decompress_to_vector(comp.data(), trunc, dec);
    }
    std::cout << "OK\n" << std::flush;
}

// Ported from the removed tests/unit/stream_encoder_tests.cpp, which
// targeted the old StreamEncoder read()/finish() API and was never
// registered in CMake (dead code). The property it pinned is kept:
// feeding the same input in any chunk schedule must produce
// byte-identical output to a one-shot feed.
void test_stream_encoder_chunking_invariance() {
    constexpr size_t DATA_SIZE = 5 * 1024 * 1024;
    constexpr size_t WIN_SIZE = 4 * 1024 * 1024;
    auto src = make_patterned_data(DATA_SIZE);

    std::vector<core::byte> one_shot;
    {
        StreamEncoder enc(3, WIN_SIZE);
        assert(enc.feed(src.data(), src.size()));
        assert(enc.finish(one_shot));
    }
    assert(!one_shot.empty());

    auto chunked = [&](const std::vector<size_t>& sizes, const char* name) {
        StreamEncoder enc(3, WIN_SIZE);
        std::vector<core::byte> out;
        size_t off = 0, i = 0;
        while (off < DATA_SIZE) {
            size_t len = (sizes.size() == 1) ? sizes[0] : sizes[i++ % sizes.size()];
            if (off + len > DATA_SIZE) len = DATA_SIZE - off;
            assert(enc.feed(src.data() + off, len));
            off += len;
        }
        assert(enc.finish(out));
        assert(out.size() == one_shot.size());
        assert(std::memcmp(out.data(), one_shot.data(), out.size()) == 0);
        std::cout << "    - " << name << " OK\n";
    };

    chunked({1}, "1-byte chunks");
    chunked({17}, "17-byte chunks");
    chunked({4096}, "4096-byte chunks");
    chunked({WIN_SIZE + 1024}, "> window-size chunks");
    chunked({256 * 1024}, "256 KiB chunks");

    std::mt19937 rng(1337);
    std::vector<size_t> random_chunks;
    for (int i = 0; i < 1000; ++i) random_chunks.push_back(rng() % 32768 + 1);
    chunked(random_chunks, "randomized seeded chunks");

    std::cout << "[PASS] StreamEncoder chunking invariance (byte-identical output)\n";
}

// ── Phase 2: Pre-processing filter tests ─────────────────────────────────────

// Build a synthetic x86-style binary: MZ+PE header → CALL E8 instructions with
// relative offsets pointing to nearby addresses. WinRAR's filter heuristic
// should detect this as E8 via the PE machine type.
static std::vector<core::byte> make_x86_binary(size_t n_calls) {
    std::vector<core::byte> data(n_calls * 5 + 256, 0x90); // NOP sled
    // MZ header stub
    data[0] = 'M';
    data[1] = 'Z';
    core::write_le32(data.data() + 0x3C, 64); // PE header at offset 64
    // PE header
    data[64] = 'P';
    data[65] = 'E';
    data[66] = 0;
    data[67] = 0;
    core::write_le16(data.data() + 68, 0x8664); // x86_64 machine
    // Insert CALL (E8) instructions with local relative addresses
    for (size_t i = 0; i < n_calls; ++i) {
        size_t off = 128 + i * 5;
        if (off + 5 > data.size()) break;
        data[off] = 0xE8;
        // Local relative call within +/- 16 MB
        core::int32 rel = static_cast<core::int32>((i * 1234567) % 0x01000000u);
        core::write_le32(data.data() + off + 1, static_cast<core::uint32>(rel));
    }
    return data;
}

// Build a synthetic ARM binary: fake ELF + 32-bit ARM BL instructions
static std::vector<core::byte> make_arm_binary(size_t n_branches) {
    // word-aligned size
    size_t data_size = std::max<size_t>((128 + n_branches * 4 + 255) & ~3u, 512);
    std::vector<core::byte> data(data_size, 0);
    // ELF header with ARM machine type
    data[0] = 0x7F;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 1;                                // 32-bit
    core::write_le16(data.data() + 0x12, 0x28); // EM_ARM
    // Insert BL instructions: high byte 0xEB, low 24 bits = local offset within +-2MB
    for (size_t i = 0; i < n_branches; ++i) {
        size_t off = 128 + i * 4;
        if (off + 4 > data.size()) break;
        core::uint32 instr = 0xEB000000u | (static_cast<core::uint32>(i * 37) & 0x0007FFFFu);
        core::write_le32(data.data() + off, instr);
    }
    return data;
}

// Build a synthetic 16-bit stereo PCM WAV
static std::vector<core::byte> make_wav_pcm(size_t n_samples) {
    size_t data_bytes = n_samples * 4; // 16-bit stereo = 4 bytes per sample
    size_t file_size = 44 + data_bytes;
    std::vector<core::byte> data(file_size, 0);
    // RIFF header
    std::memcpy(data.data(), "RIFF", 4);
    core::write_le32(data.data() + 4, static_cast<core::uint32>(file_size - 8));
    std::memcpy(data.data() + 8, "WAVEfmt ", 8);
    core::write_le32(data.data() + 16, 16);        // fmt chunk size
    core::write_le16(data.data() + 20, 1);         // PCM format
    core::write_le16(data.data() + 22, 2);         // 2 channels (stereo)
    core::write_le32(data.data() + 24, 44100);     // sample rate
    core::write_le32(data.data() + 28, 44100 * 4); // byte rate
    core::write_le16(data.data() + 32, 4);         // block align
    core::write_le16(data.data() + 34, 16);        // bits per sample
    std::memcpy(data.data() + 36, "data", 4);
    core::write_le32(data.data() + 40, static_cast<core::uint32>(data_bytes));
    // Generate a sine-like waveform (correlated samples → good for delta)
    for (size_t i = 0; i < n_samples; ++i) {
        int16_t val = static_cast<int16_t>((i * 317) % 32768);
        core::write_le16(data.data() + 44 + i * 4, static_cast<core::uint16>(val));
        core::write_le16(data.data() + 44 + i * 4 + 2, static_cast<core::uint16>(val / 2));
    }
    return data;
}

void test_filter_detect_e8_binary() {
    std::cout << "[+] test_filter_detect_e8_binary" << std::endl;
    auto bin = make_x86_binary(200);
    core::uint8 channels = 0;
    FilterType ft = Filters50::detect_filter(bin.data(), bin.size(), channels);
    assert(ft == FilterType::E8);
    assert(channels == 1);
    std::cout << "    - E8 detection from PE header: OK" << std::endl;
}

void test_filter_detect_arm_binary() {
    std::cout << "[+] test_filter_detect_arm_binary" << std::endl;
    auto bin = make_arm_binary(200);
    core::uint8 channels = 0;
    FilterType ft = Filters50::detect_filter(bin.data(), bin.size(), channels);
    assert(ft == FilterType::Arm);
    assert(channels == 1);
    std::cout << "    - ARM detection from ELF header: OK" << std::endl;
}

void test_filter_detect_delta_wav() {
    std::cout << "[+] test_filter_detect_delta_wav" << std::endl;
    auto wav = make_wav_pcm(8000);
    core::uint8 channels = 0;
    FilterType ft = Filters50::detect_filter(wav.data(), wav.size(), channels);
    assert(ft == FilterType::Delta);
    assert(channels == 4); // 2ch * 16bit / 8 = 4 bytes stride
    std::cout << "    - Delta detection from WAV header: OK (channels=" << (int)channels << ")"
              << std::endl;
}

void test_filter_detect_disable_all() {
    std::cout << "[+] test_filter_detect_disable_all" << std::endl;
    auto bin = make_x86_binary(200);
    core::uint8 channels = 0;
    FilterConfig cfg;
    cfg.mode = FilterMode::DisableAll;
    FilterType ft = Filters50::detect_filter(bin.data(), bin.size(), channels, cfg);
    assert(ft == FilterType::None);
    std::cout << "    - DisableAll bypasses detection: OK" << std::endl;
}

void test_detect_filter_hostile_pe_offset() {
    std::cout << "[+] test_detect_filter_hostile_pe_offset" << std::endl;
    // Crafted "MZ" payload whose e_lfanew sits near UINT32_MAX: the old 32-bit
    // guard (pe_off + 6 < size) wrapped around, passed, and read ~4 GiB out of
    // bounds (SIGSEGV repro). The 64-bit guard must reject the probe.
    std::vector<core::byte> data(256, 0x00);
    data[0] = 'M';
    data[1] = 'Z';
    core::write_le32(data.data() + 0x3C, 0xFFFFFFFCu);
    FilterConfig cfg; // Auto
    core::uint8 channels = 0;
    FilterType ft = Filters50::detect_filter(data.data(), data.size(), channels, cfg);
    assert(ft == FilterType::None);
    // Boundary values of the wraparound range must also be rejected cleanly.
    for (core::uint32 bad_off : {0xFFFFFFFAu, 0xFFFFFFFBu, 0xFFFFFFFDu, 0xFFFFFFFFu}) {
        core::write_le32(data.data() + 0x3C, bad_off);
        ft = Filters50::detect_filter(data.data(), data.size(), channels, cfg);
        assert(ft == FilterType::None);
    }
    std::cout << "    - Hostile e_lfanew rejected without OOB: OK" << std::endl;
}

void test_stream_encoder_decoder_filter_roundtrip() {
    std::cout << "[+] test_stream_encoder_decoder_filter_roundtrip" << std::endl;
    // The v1.21.1 regression: an active E8 filter with 1 MiB regions and
    // encoder blocks every 0x80000 input bytes means regions routinely span
    // block boundaries. The per-block decode path (StreamDecoder) must carry
    // pending regions across calls; it previously aborted mid-stream.
    auto data = make_x86_binary(600000);
    data.resize(3 * 1024 * 1024, 0x90);

    for (int method : {1, 3, 5}) {
        compress::StreamEncoder enc(method, 4 * 1024 * 1024);
        std::vector<core::byte> packed;
        const size_t kFeed = 65536;
        for (size_t off = 0; off < data.size(); off += kFeed) {
            size_t n = std::min(kFeed, data.size() - off);
            assert(enc.feed(data.data() + off, n));
        }
        assert(enc.finish(packed));
        assert(!packed.empty());

        // Reference: whole-stream decode (single decompress call) must match.
        Decompressor50 dec_ref(4 * 1024 * 1024);
        std::vector<core::byte> reference;
        assert(dec_ref.decompress_to_vector(packed.data(), packed.size(), reference, false));
        assert(reference.size() == data.size());
        assert(std::memcmp(reference.data(), data.data(), data.size()) == 0);

        // Regression target: per-block decode through StreamDecoder.
        compress::StreamDecoder sdec(4 * 1024 * 1024, method);
        std::vector<core::byte> out;
        for (size_t off = 0; off < packed.size(); off += kFeed) {
            size_t n = std::min(kFeed, packed.size() - off);
            assert(sdec.feed(packed.data() + off, n));
        }
        assert(sdec.finish(out));
        assert(out.size() == data.size());
        assert(std::memcmp(out.data(), data.data(), data.size()) == 0);
    }
    std::cout << "    - StreamEncoder→StreamDecoder with spanning E8 regions: OK (m1/m3/m5)"
              << std::endl;
}

void test_stream_decoder_defense() {
    std::cout << "[+] test_stream_decoder_defense" << std::endl;
    // Zero window must clamp to a usable default, never construct a
    // decompressor with win_size_ == 0 (undefined window arithmetic).
    compress::StreamDecoder zdec(0, 3);
    std::vector<core::byte> out;
    // Empty stream: nothing decoded and no LastBlock flag -> fail closed.
    assert(!zdec.finish(out));

    // Truncated stream: valid blocks but the LastBlock-framed final block
    // never arrives -> finish() must fail instead of passing the payload.
    auto data = make_patterned_data(1 << 20);
    compress::StreamEncoder enc(3, 1024 * 1024);
    std::vector<core::byte> packed;
    assert(enc.feed(data.data(), data.size()));
    assert(enc.finish(packed));
    assert(packed.size() > 16);
    compress::StreamDecoder tdec(1024 * 1024, 3);
    assert(tdec.feed(packed.data(), packed.size() / 2));
    assert(!tdec.finish(out));
    std::cout << "    - Zero-window clamp and truncated-stream rejection: OK" << std::endl;
}

void test_filter_e8_roundtrip() {
    std::cout << "[+] test_filter_e8_roundtrip" << std::endl;
    auto original = make_x86_binary(500);
    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(original.data(), original.size(), compressed, 3,
                                         4 * 1024 * 1024));
    assert(!compressed.empty());
    Decompressor50 dec(4 * 1024 * 1024);
    assert(dec.last_error() == DecompressErrorCode::Ok);
    std::vector<core::byte> decompressed;
    assert(dec.decompress_to_vector(compressed.data(), compressed.size(), decompressed, false));
    assert(decompressed.size() == original.size());
    assert(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);
    std::cout << "    - E8 compress→decompress roundtrip: OK (" << original.size() << " → "
              << compressed.size() << ")" << std::endl;
}

void test_filter_arm_roundtrip() {
    std::cout << "[+] test_filter_arm_roundtrip" << std::endl;
    auto original = make_arm_binary(500);
    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(original.data(), original.size(), compressed, 3,
                                         4 * 1024 * 1024));
    assert(!compressed.empty());
    Decompressor50 dec(4 * 1024 * 1024);
    assert(dec.last_error() == DecompressErrorCode::Ok);
    std::vector<core::byte> decompressed;
    assert(dec.decompress_to_vector(compressed.data(), compressed.size(), decompressed, false));
    assert(decompressed.size() == original.size());
    assert(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);
    std::cout << "    - ARM compress→decompress roundtrip: OK (" << original.size() << " → "
              << compressed.size() << ")" << std::endl;
}

void test_filter_delta_roundtrip() {
    std::cout << "[+] test_filter_delta_roundtrip" << std::endl;
    auto original = make_wav_pcm(16000);
    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(original.data(), original.size(), compressed, 3,
                                         4 * 1024 * 1024));
    assert(!compressed.empty());
    Decompressor50 dec(4 * 1024 * 1024);
    assert(dec.last_error() == DecompressErrorCode::Ok);
    std::vector<core::byte> decompressed;
    assert(dec.decompress_to_vector(compressed.data(), compressed.size(), decompressed, false));
    assert(decompressed.size() == original.size());
    assert(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);
    std::cout << "    - Delta compress→decompress roundtrip: OK (" << original.size() << " → "
              << compressed.size() << ")" << std::endl;
}

void test_filter_improves_compression() {
    std::cout << "[+] test_filter_improves_compression" << std::endl;
    auto original = make_x86_binary(2000);

    // With filters (auto-detect)
    std::vector<core::byte> with_filter;
    assert(Compressor50::compress_buffer(original.data(), original.size(), with_filter, 3,
                                         4 * 1024 * 1024));

    // Without filters (disabled)
    FilterConfig no_filter;
    no_filter.mode = FilterMode::DisableAll;
    std::vector<core::byte> without_filter;
    assert(Compressor50::compress_buffer(original.data(), original.size(), without_filter, 3,
                                         4 * 1024 * 1024, no_filter));

    std::cout << "    - With filter: " << with_filter.size() << " bytes" << std::endl;
    std::cout << "    - Without filter: " << without_filter.size() << " bytes" << std::endl;
    // Filter should improve or at least not hurt compression on x86 binaries
    assert(with_filter.size() <= without_filter.size());

    // Verify both decompress correctly
    Decompressor50 dec(4 * 1024 * 1024);
    std::vector<core::byte> dec_filtered, dec_unfiltered;
    assert(dec.decompress_to_vector(with_filter.data(), with_filter.size(), dec_filtered, false));
    assert(dec_filtered.size() == original.size());
    assert(std::memcmp(dec_filtered.data(), original.data(), original.size()) == 0);

    Decompressor50 dec2(4 * 1024 * 1024);
    assert(dec2.decompress_to_vector(without_filter.data(), without_filter.size(), dec_unfiltered,
                                     false));
    assert(dec_unfiltered.size() == original.size());
    assert(std::memcmp(dec_unfiltered.data(), original.data(), original.size()) == 0);

    std::cout << "    - Ratio improvement verified: OK" << std::endl;
}

void test_chunk_framing_spike() {
    std::cout << "[+] test_chunk_framing_spike" << std::endl;
    // 64 KiB payload divided into four 16 KiB independent chunks
    const size_t CHUNK_SIZE = 16 * 1024;
    const size_t NUM_CHUNKS = 4;
    const size_t TOTAL_SIZE = CHUNK_SIZE * NUM_CHUNKS;

    std::vector<core::byte> original(TOTAL_SIZE);
    for (size_t i = 0; i < TOTAL_SIZE; ++i) {
        original[i] = static_cast<core::byte>((i % 251) ^ (i / 17));
    }

    std::vector<core::byte> concatenated_stream;
    for (size_t c = 0; c < NUM_CHUNKS; ++c) {
        bool is_last = (c == NUM_CHUNKS - 1);
        Compressor50 packer;
        packer.begin_archive(nullptr, 3, 128 * 1024);
        packer.set_external_buffer(original.data() + c * CHUNK_SIZE, CHUNK_SIZE);
        packer.set_memory_dest(&concatenated_stream);
        core::int64 packed = packer.compress(is_last);
        assert(packed > 0);
    }

    // Unpack concatenated stream through standard Decompressor50
    Decompressor50 dec(128 * 1024);
    std::vector<core::byte> decompressed;
    bool ok = dec.decompress_to_vector(concatenated_stream.data(), concatenated_stream.size(),
                                       decompressed, false);
    assert(ok);
    assert(decompressed.size() == original.size());
    assert(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);

    // Wrap concatenated stream in full RAR5 archive container
    std::filesystem::path spike_arc = "build/spike_chunk_test.rar";
    std::filesystem::remove(spike_arc);

    archive::ArchiveMutator::PreparedAdd prep;
    prep.entry_name = "chunked_payload.bin";
    prep.payload = concatenated_stream;
    prep.fb.file_name = "chunked_payload.bin";
    prep.fb.unp_size = original.size();
    prep.fb.pack_size = concatenated_stream.size();
    crypto::Crc32 crc_c;
    crc_c.update(original.data(), original.size());
    prep.fb.data_crc32 = crc_c.get();
    prep.fb.has_crc32 = true;
    prep.fb.method = 3;
    prep.fb.win_size = 128 * 1024;
    prep.fb.attributes = 0x20;

    std::vector<archive::ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prep));
    assert(archive::ArchiveMutator::write_batch_add(spike_arc, batch));

    // Verify OpenRAR ArchiveReader tests and extracts successfully
    archive::ArchiveReader reader;
    assert(reader.open(spike_arc));
    assert(reader.entries().size() == 1);
    assert(reader.test_entry(reader.entries()[0]));
    std::vector<core::byte> extracted;
    assert(reader.extract_entry_to_memory(0, extracted, 1024 * 1024, {}) == 0);
    assert(extracted == original);
    reader.close();

    // Verify Official Reference UnRAR.exe if present on the host
    const char* unrar_path = "C:\\Program Files\\WinRAR\\UnRAR.exe";
    if (std::filesystem::exists(unrar_path)) {
        std::string unrar_cmd =
            std::string("\"\"") + unrar_path + "\" t -y \"" + spike_arc.string() + "\" > nul\"";
        int rc = std::system(unrar_cmd.c_str());
        assert(rc == 0 && "Official UnRAR.exe failed to test multi-block chunked RAR5 archive!");
        std::cout << "    - Official UnRAR.exe 7.20 verification: OK" << std::endl;
    }

    std::error_code ec;
    std::filesystem::remove(spike_arc, ec);

    std::cout << "    - 16 KiB chunk framing spike roundtrip: OK (" << original.size() << " -> "
              << concatenated_stream.size() << ")" << std::endl;
}

// ── v1.22.0: cross-implementation bit-exactness gate for the match finder ───
// Every implementation the running CPU supports must produce results
// identical to the scalar reference, across random data and every boundary
// size class (0, 1, word, SIMD-lane and multi-lane widths, plus tails). This
// is the roadmap's "Scalar == AVX2 == AVX-512 == NEON" gate: kernels are
// only exercised where the hardware supports them, so the gate validates
// exactly what a given machine can execute.
void test_match_length_bit_exactness() {
    std::cout << "[+] test_match_length_bit_exactness" << std::endl;
    const auto& cpu = core::get_cpu_features();
    std::vector<std::pair<const char*, arch::MatchFn>> impls;
    impls.push_back({"scalar", arch::match_length_scalar});
#if defined(OPENRAR_HAS_X86_SIMD)
    if (cpu.sse2) impls.push_back({"sse2", arch::match_length_sse2});
    if (cpu.avx2) impls.push_back({"avx2", arch::match_length_avx2});
#endif
#if defined(OPENRAR_HAS_X86_SIMD) && defined(OPENRAR_HAS_AVX512_KERNEL)
    if (cpu.avx512f) impls.push_back({"avx512", arch::match_length_avx512_kernel});
#endif
    assert(impls.size() >= 1);

    // Deterministic corpus: high-entropy noise (frequent mismatches), long
    // runs (long matches), and near-identical buffers with late first
    // differences at lane-boundary-sensitive offsets.
    std::vector<std::vector<core::byte>> corpus;
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byte(0, 255);
    {
        std::vector<core::byte> noise(4096);
        for (auto& b : noise) b = static_cast<core::byte>(byte(rng));
        corpus.push_back(noise);
        std::vector<core::byte> runs(4096, core::byte(0xAB));
        corpus.push_back(runs);
        std::vector<core::byte> shifted(noise);
        for (int d : {1, 7, 8, 32, 63, 64, 65, 255}) {
            shifted = noise;
            if (d < static_cast<int>(shifted.size())) {
                shifted[static_cast<size_t>(d)] = static_cast<core::byte>(
                    static_cast<uint8_t>(noise[static_cast<size_t>(d)]) ^ 0xFF);
            }
            corpus.push_back(shifted);
        }
    }

    const size_t caps[] = {0,  1,  2,  7,   8,   9,   15,  16,  17,   31,   32,  33,
                           63, 64, 65, 127, 128, 129, 255, 256, 1024, 4095, 4096};
    size_t checked = 0;
    for (const auto& buf : corpus) {
        for (const auto& other : corpus) {
            for (size_t cap : caps) {
                const size_t expect = arch::match_length_scalar(buf.data(), other.data(), cap);
                for (const auto& impl : impls) {
                    const size_t got = impl.second(buf.data(), other.data(), cap);
                    assert(got == expect && "SIMD match length diverged from scalar");
                }
                checked++;
            }
        }
    }
    std::cout << "    - " << impls.size() << " paths (";
    for (size_t k = 0; k < impls.size(); ++k) {
        std::cout << impls[k].first << (k + 1 < impls.size() ? "," : "");
    }
    std::cout << ") identical over " << checked << " cases: OK" << std::endl;
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "Running Clean-Room Milestone 6 Compression Verification...\n" << std::flush;
    test_cpu_features();
    std::cout << std::flush;
    test_match_length_bit_exactness();
    std::cout << std::flush;
    test_delta_filter();
    std::cout << std::flush;
    test_e8_filter();
    std::cout << std::flush;
    test_arm_filter();
    std::cout << std::flush;
    test_forced_scalar_filters();
    std::cout << std::flush;
    test_forward_filters();
    std::cout << std::flush;
    test_bit_reader_and_huffman();
    std::cout << std::flush;
    test_decompressor50();
    std::cout << std::flush;
    test_compressor50_roundtrip();
    std::cout << std::flush;
    test_compressor50_external_buffer_no_slide();
    std::cout << std::flush;
    test_decompressor50_post_wrap_overlap_match();
    std::cout << std::flush;
    test_decompressor50_slow_path_wildcopy_wrap();
    test_decompressor50_wrapped_src_stale_gap();
    std::cout << std::flush;
    test_compressor50_reset_after_external_buffer();
    std::cout << std::flush;
    test_compressor50_all_zeros_8mib();
    std::cout << std::flush;
    test_compressor50_boundary_sizes();
    std::cout << std::flush;
    test_compressor50_filestream_roundtrip();
    std::cout << std::flush;
    test_compressor50_truncated_source();
    std::cout << std::flush;
    test_compressor50_streaming_feed_large_chunks();
    std::cout << std::flush;
    test_chunking_invariance();
    std::cout << std::flush;
    test_stream_encoder_chunking_invariance();
    std::cout << std::flush;
    test_recompress_stability();
    std::cout << std::flush;
    test_truncation_sweep();
    std::cout << std::flush;
    test_archive_roundtrip();
    std::cout << std::flush;
    test_huffman_builder_shrink();
    std::cout << std::flush;
    test_huffman_builder_adversarial();
    std::cout << std::flush;
    test_match_length_tail_caps();
    std::cout << std::flush;
    test_decompressor_lazy_alloc();
    std::cout << std::flush;
    test_bit_reader_get_bits64();
    std::cout << std::flush;
    test_extra_distance_slot_decoding();
    std::cout << std::flush;
    test_compressor50_large_window_match_finding();
    std::cout << std::flush;
    test_filter_detect_e8_binary();
    std::cout << std::flush;
    test_filter_detect_arm_binary();
    std::cout << std::flush;
    test_filter_detect_delta_wav();
    std::cout << std::flush;
    test_filter_detect_disable_all();
    std::cout << std::flush;
    test_detect_filter_hostile_pe_offset();
    std::cout << std::flush;
    test_stream_encoder_decoder_filter_roundtrip();
    std::cout << std::flush;
    test_stream_decoder_defense();
    std::cout << std::flush;
    test_filter_e8_roundtrip();
    std::cout << std::flush;
    test_filter_arm_roundtrip();
    std::cout << std::flush;
    test_filter_delta_roundtrip();
    std::cout << std::flush;
    test_filter_improves_compression();
    std::cout << std::flush;
    test_chunk_framing_spike();
    std::cout << std::flush;
    std::cout << "All Milestone 6 Compression & Decompression Primitives PASSED!\n" << std::flush;
    return 0;
}
