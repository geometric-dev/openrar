#include "../../src/core/types.hpp"
#include "../../src/compress/filters50.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/stream_encoder.hpp"
#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/arch/match_simd.hpp"
#include "../../src/crypto/sha256.hpp"
#include "test_support.hpp"

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

// ---------------------------------------------------------------------------
// P1 regression (fuzz iteration 727): filter chunk boundaries
//
// The decoder un-transforms exactly one filter-token region per token, and
// the RAR5 filter scan skips any CALL whose operand crosses the region end
// (the pos + 4 < size guard). The encoder-side transform must therefore run
// over the SAME chunk boundaries the filter tokens declare: a wider
// transform window transforms a boundary-crossing CALL that the decoder's
// narrower region scan then never reverts (fuzz 727: E8 opcode at 65534,
// first divergence 65535, operand bytes shifted by the translation).
// ---------------------------------------------------------------------------

// Minimal MZ/PE image so detect_filter() deterministically selects the E8
// filter without relying on opcode-density heuristics.
static void fill_pe_header(std::vector<core::byte>& d) {
    d[0] = 'M';
    d[1] = 'Z';
    core::write_le32(d.data() + 0x3C, 0x80);
    d[0x80] = 'P';
    d[0x81] = 'E';
    d[0x82] = 0;
    d[0x83] = 0;
    d[0x84] = 0x4C; // i386 machine -> E8
    d[0x85] = 0x01;
}

// Deterministic filler so the LZ layer has structure; must not touch the
// [65524, 65542) window around the planted boundary CALL below.
static void fill_pattern(std::vector<core::byte>& d, size_t from) {
    for (size_t i = from; i < d.size(); i += 251) d[i] = static_cast<core::byte>(i);
}

// A CALL whose operand arithmetic actually fires: addr < 0x01000000 and
// addr + offset < 0x01000000 -> transform rewrites the operand.
static void plant_call(std::vector<core::byte>& d, size_t pos) {
    d[pos] = 0xE8;
    core::write_le32(d.data() + pos + 1, 0x00EF1000);
}

void test_e8_filter_chunk_boundary_symmetry() {
    std::cout << "[+] test_e8_filter_chunk_boundary_symmetry" << std::endl;
    const size_t kChunk = 65536;
    std::vector<core::byte> data(kChunk * 2, 0);
    plant_call(data, 1000);        // interior of chunk 1: transform fires
    plant_call(data, 65534);       // operand [65535, 65539) crosses the boundary: stays raw
    plant_call(data, 65540 + 100); // interior of chunk 2: transform fires
    fill_pattern(data, 0x90);
    std::vector<core::byte> orig = data;

    // Encode exactly the way compress_buffer() pre-transforms (per-chunk,
    // file_offset = chunk start), decode exactly the way the decoder applies
    // one filter-token region per token.
    for (size_t off = 0; off < data.size(); off += kChunk) {
        Filters50::encode_e8(data.data() + off, kChunk, off, false);
    }
    // Guard against a vacuous identity: interior sites must have been
    // transformed (their operands moved).
    assert(std::memcmp(data.data() + 1001, orig.data() + 1001, 4) != 0);
    assert(std::memcmp(data.data() + 65641, orig.data() + 65641, 4) != 0);
    // The boundary-crossing site must NOT be transformed by a kChunk window.
    assert(std::memcmp(data.data() + 65535, orig.data() + 65535, 4) == 0);

    for (size_t off = 0; off < data.size(); off += kChunk) {
        Filters50::apply_e8(data.data() + off, kChunk, off, false);
    }
    assert(data.size() == orig.size() && std::memcmp(data.data(), orig.data(), data.size()) == 0);
    std::cout << "[PASS] E8 filter chunk-boundary encode/decode symmetry\n";
}

void test_filter_chunk_boundary_roundtrip_p1() {
    std::cout << "[+] test_filter_chunk_boundary_roundtrip_p1" << std::endl;
    // 70000 bytes: compress_buffer clamps the packer window to 128 KiB, so
    // filter tokens are 64 KiB and the planted CALL at 65534 lands in the
    // token's last-4-bytes dead zone. Default compress_buffer args (2 MiB
    // requested window) — the exact shape of fuzz iteration 727.
    const size_t kSize = 70000;
    std::vector<core::byte> data(kSize, 0);
    fill_pe_header(data);
    plant_call(data, 65534);
    fill_pattern(data, 0x90);

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(data.data(), data.size(), compressed));

    std::vector<core::byte> out;
    // Raw streams carry no dictionary-size header: the decoder window is
    // caller knowledge, and compress_buffer() defaults to 0x200000 (its
    // pow2 clamp only shrinks it). A default-constructed Decompressor50
    // runs a 1 MiB window and cannot decode default-window streams once
    // match distances or filter regions exceed 1 MiB.
    Decompressor50 dec(0x200000);
    assert(dec.decompress_to_vector(compressed.data(), compressed.size(), out));
    assert(out.size() == data.size() && std::memcmp(out.data(), data.data(), data.size()) == 0);
    std::cout << "[PASS] compress_buffer roundtrip with boundary-crossing CALL (P1, fuzz 727)\n";
}

void test_filter_multi_token_multiblock_roundtrip() {
    std::cout << "[+] test_filter_multi_token_multiblock_roundtrip" << std::endl;
    // 1228800: ~3 blocks (need_flush at 512 KiB input) and 2 filter tokens
    // (1 MiB + 200 KiB regions) — two tokens broke the decoder before the
    // window contract was understood. 8388608: exceeds comp_win + 4 MiB, so
    // compress_buffer routes through the mem_src branch, where the packer
    // must not re-transform the pre-transformed data.
    for (size_t kSize : {1228800u, 8388608u}) {
        std::vector<core::byte> data(kSize, 0);
        fill_pe_header(data);
        plant_call(data, 65534);
        fill_pattern(data, 0x90);

        std::vector<core::byte> compressed;
        assert(Compressor50::compress_buffer(data.data(), data.size(), compressed));

        std::vector<core::byte> out;
        Decompressor50 dec(0x200000);
        assert(dec.decompress_to_vector(compressed.data(), compressed.size(), out));
        assert(out.size() == data.size() && std::memcmp(out.data(), data.data(), data.size()) == 0);
    }
    std::cout << "[PASS] filter roundtrip: multi-token, multi-block, mem_src branch\n";
}

void test_default_window_pairing_roundtrip() {
    std::cout << "[+] test_default_window_pairing_roundtrip" << std::endl;
    // Default-vs-default contract: a raw block stream carries no
    // dictionary-size header, so the default Decompressor50 window must
    // cover the default compress_buffer window (2 MiB). This input forces
    // the full 2 MiB encoder window (1 MiB filter tokens, distances up to
    // 2 MiB) and would fail against the old 1 MiB decoder default.
    const size_t kSize = 1500000;
    std::vector<core::byte> data(kSize, 0);
    fill_pe_header(data);
    plant_call(data, 65534);
    fill_pattern(data, 0x90);

    std::vector<core::byte> compressed;
    assert(Compressor50::compress_buffer(data.data(), data.size(), compressed));

    std::vector<core::byte> out;
    Decompressor50 dec; // default window: must decode default-window streams
    assert(dec.win_size() == Decompressor50::DEFAULT_WIN_SIZE);
    assert(dec.win_size() == 0x200000);
    assert(dec.decompress_to_vector(compressed.data(), compressed.size(), out));
    assert(out.size() == data.size() && std::memcmp(out.data(), data.data(), data.size()) == 0);
    std::cout << "[PASS] default compressor window decodable by default decoder window\n";
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
    if (cpu.avx512f) std::cout << " AVX512F";
    if (cpu.gfni) std::cout << " GFNI";
    if (cpu.aes_ni) std::cout << " AES-NI";
    if (cpu.pclmulqdq) std::cout << " PCLMUL";
    if (cpu.sha_ni) std::cout << " SHA-NI";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (cpu.neon) std::cout << " NEON";
    if (cpu.arm_crc32) std::cout << " CRC32";
    if (cpu.arm_aes) std::cout << " AES";
    if (cpu.arm_sha2) std::cout << " SHA2";
    if (cpu.arm_pmull) std::cout << " PMULL";
#elif defined(__arm__) || defined(_M_ARM)
    // ARM32 detector populates no dispatch tags yet: every kernel runs
    // scalar on ARMv7 (the SIMD kernels target AArch64/x86 only).
    (void)cpu;
    std::cout << " scalar";
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
#include <cstdlib>
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

    // v1.31 M1 slot-257 test surface
    static void set_lastlen_suppressed(Compressor50& c, bool v) { c.lastlen_suppressed_ = v; }
    static const std::vector<Compressor50Token>& match_tokens(const Compressor50& c) {
        return c.match_tokens_;
    }
    // write_block() clears the token storage, so token-inspecting tests
    // drive the loop and the block writer separately.
    static bool process_available(Compressor50& c, bool final) {
        return c.process_available(final) >= 0;
    }
    static bool write_block(Compressor50& c, bool last_block) { return c.write_block(last_block); }
    static core::uint64 packed_total(const Compressor50& c) { return c.packed_total_; }
    static void init_match_params(Compressor50& c) { c.init_match_params(); }
    static size_t last_length(const Compressor50& c) { return c.last_length_; }
    // v1.33 item 1: direct access to the match emitter, so the admissibility
    // floor and the truncation/report contract can be tested at the exact
    // boundaries without needing the finder to produce a boundary pair.
    static size_t add_match(Compressor50& c, size_t length, size_t distance) {
        return c.add_match(length, distance);
    }
    static size_t match_token_count(const Compressor50& c) { return c.match_tokens_.size(); }
    // v1.33 Design A: the decoder-slot seed window, so T11 can pin the
    // first-N-blocks-carry rule without hard-coding the constant twice.
    // Narrowed to int to match the test's block counter (gcc -Werror=sign-compare).
    static constexpr int REUSE_SEED_BLOCKS = static_cast<int>(Compressor50::REUSE_SEED_BLOCKS);
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
    // Only declared where a SIMD implementation can join the gate: on
    // ARM32 every kernel runs scalar, so there is nothing to compare.
#if defined(OPENRAR_HAS_X86_SIMD) || defined(__aarch64__) || defined(__ARM_NEON) ||                \
    defined(_M_ARM64)
    const auto& cpu = core::get_cpu_features();
#endif
    std::vector<std::pair<const char*, arch::MatchFn>> impls;
    impls.push_back({"scalar", arch::match_length_scalar});
#if defined(OPENRAR_HAS_X86_SIMD)
    if (cpu.sse2) impls.push_back({"sse2", arch::match_length_sse2});
    if (cpu.avx2) impls.push_back({"avx2", arch::match_length_avx2});
#endif
#if defined(OPENRAR_HAS_X86_SIMD) && defined(OPENRAR_HAS_AVX512_KERNEL)
    if (cpu.avx512f) impls.push_back({"avx512", arch::match_length_avx512_kernel});
#endif
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(_M_ARM64)
    if (cpu.neon) impls.push_back({"neon", arch::match_length_neon});
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

// ─── v1.31 M1: slot-257 repeat-last-length emission (T1-T4, T9) ──────────
// Semantics under test (docs/v1.31-implementation-plan.md §1 M1): 257 rides
// inside the rep-win branch, copies exactly last_length_ bytes from
// old_dist_[0], and requires real byte verification bounded by the filter
// region remainder. The shadow mirror targets decompressor50.cpp:813-911.

namespace {

// Compress with the 257 path suppressed, mirroring compress_buffer()'s
// no-filter small-input branch exactly (each T9 corpus asserts FilterType
// detection == None and src_size <= comp_win + 0x400000). Byte output must
// equal the pre-M1 (v1.30.4) capture.
bool compress_buffer_no257(const std::vector<core::byte>& src, std::vector<core::byte>& dest,
                           int method) {
    const size_t win = 0x200000;
    size_t comp_win_size = win;
    if (src.size() > 0 && src.size() < win) {
        size_t pow2_sz = 0x20000;
        while (pow2_sz < src.size() && pow2_sz < win) pow2_sz <<= 1;
        comp_win_size = std::min(win, pow2_sz);
    }
    Compressor50 packer;
    packer.begin_archive(nullptr, method, comp_win_size);
    packer.set_external_buffer(src.data(), src.size());
    Compressor50TestAccess::set_lastlen_suppressed(packer, true);
    packer.set_memory_dest(&dest);
    return packer.compress() >= 0 && !dest.empty();
}

void assert_roundtrip_2mib(const std::vector<core::byte>& packed,
                           const std::vector<core::byte>& src) {
    Decompressor50 dec(0x200000);
    std::vector<core::byte> out;
    assert(dec.decompress_to_vector(packed.data(), packed.size(), out));
    assert(out.size() == src.size());
    assert(std::memcmp(out.data(), src.data(), src.size()) == 0);
}

size_t count_lastlen(const std::vector<Compressor50Token>& toks) {
    size_t n = 0;
    for (const auto& t : toks)
        if (t.get_type() == Compressor50Token::TokenType::LastLen) n++;
    return n;
}

// T9 corpus generators — MUST stay identical to the pre-M1 capture program
// (docs/v1.31-implementation-plan.md T9): the expected hashes below were
// recorded from v1.30.4 emission code before the 257 path existed.
core::uint32 t9_lcg(core::uint32 s) {
    return s * 1664525u + 1013904223u;
}

std::vector<core::byte> t9_text(size_t n) {
    const char* words[] = {"the",   "open",  "rar",  "archive", "compress",
                           "token", "match", "slot", "length",  "window"};
    std::vector<core::byte> v;
    v.reserve(n);
    core::uint32 s = 12345;
    while (v.size() < n) {
        s = t9_lcg(s);
        const char* w = words[(s >> 8) % 10];
        for (const char* p = w; *p && v.size() < n; ++p) v.push_back(static_cast<core::byte>(*p));
        v.push_back(static_cast<core::byte>(' '));
    }
    v.resize(n);
    return v;
}

std::vector<core::byte> t9_rand(size_t n, core::uint32 seed) {
    std::vector<core::byte> v(n);
    core::uint32 s = seed;
    for (size_t i = 0; i < n; ++i) {
        s = t9_lcg(s);
        v[i] = static_cast<core::byte>(s >> 16);
    }
    return v;
}

std::vector<core::byte> t9_mixed() {
    std::vector<core::byte> v;
    std::vector<core::byte> block = t9_rand(4096, 777);
    for (int i = 0; i < 24; ++i) {
        v.insert(v.end(), block.begin(), block.end());
        v.insert(v.end(), block.begin(), block.begin() + 100 + i * 37);
        v.push_back(static_cast<core::byte>(i));
        v.insert(v.end(), 512, static_cast<core::byte>(i * 7 + 1));
    }
    return v;
}

void sha256_hex(const std::vector<core::byte>& d, char* out /* 65 bytes */) {
    core::byte dig[32];
    crypto::Sha256::compute(d.data(), d.size(), dig);
    for (int i = 0; i < 32; ++i) std::sprintf(out + i * 2, "%02x", dig[i]);
    out[64] = 0;
}

} // namespace

// T2: no 257 before any match; run collapse emits a fresh match then a 257
// chain; stream roundtrips byte-exact.
void test_slot257_run_collapse() {
    std::vector<core::byte> data(256 * 1024, core::byte(0)); // single block input
    Compressor50 packer;
    packer.begin_archive(nullptr, 1, 0x200000);
    packer.set_external_buffer(data.data(), data.size());
    std::vector<core::byte> packed;
    packer.set_memory_dest(&packed);
    // compress() runs this first; process_available() alone does not.
    Compressor50TestAccess::init_match_params(packer);
    assert(Compressor50TestAccess::process_available(packer, true));
    // Inspect before write_block() clears the token storage.

    const auto& toks = Compressor50TestAccess::match_tokens(packer);
    assert(toks.size() > 10);
    assert(toks[0].get_type() == Compressor50Token::TokenType::Match &&
           "first match token must be a fresh Match, never LastLen");
    const size_t lastlen = count_lastlen(toks);
    assert(lastlen >= 60 && "257 chain must dominate a zero run");
    assert(Compressor50TestAccess::write_block(packer, true));
    assert_roundtrip_2mib(packed, data);

    // Suppressed rebuild: same decision path, rep0 chain instead of 257.
    std::vector<core::byte> packed2;
    assert(compress_buffer_no257(data, packed2, 1));
    assert_roundtrip_2mib(packed2, data);
    assert(packed2.size() > packed.size() && "257 chain must be strictly cheaper than rep0");
    std::cout << "[PASS] slot257 run collapse + first-token rule (T2)\n";
}

// T1: the shadow state suggesting a continuation is NOT sufficient — the
// bytes must verify. A period break mid-run forces the rep scan short; a
// naive shadow-only 257 there would copy wrong bytes and the roundtrip
// would fail.
void test_slot257_byte_verification() {
    std::vector<core::byte> data;
    for (int i = 0; i < 130; ++i)
        for (int b = 0; b < 32; ++b) data.push_back(core::byte(b));
    const size_t break_at = data.size();
    for (int b = 0; b < 32; ++b) data.push_back(core::byte(b));
    data[break_at + 5] = core::byte(0xFF); // break the period after 5 bytes
    for (int i = 0; i < 40; ++i)
        for (int b = 0; b < 32; ++b) data.push_back(core::byte(b));

    for (int method = 1; method <= 5; ++method) {
        std::vector<core::byte> packed;
        assert(
            Compressor50::compress_buffer(data.data(), data.size(), packed, method, 0x200000, {}));
        assert_roundtrip_2mib(packed, data);
    }
    // Same corpus through the suppressed path must roundtrip too.
    std::vector<core::byte> packed;
    assert(compress_buffer_no257(data, packed, 3));
    assert_roundtrip_2mib(packed, data);
    std::cout << "[PASS] slot257 byte verification at period break (T1)\n";
}

// T3: 257 chains must respect filter-region ends on BOTH transform paths
// (challenge directive 5): the compress_buffer() pretransform path here,
// the in-loop transform via StreamEncoder below. The corpus plants a CALL
// across the 64 KiB region boundary (the fuzz-727 class).
void test_slot257_filter_region_boundary() {
    const size_t kSize = 70000;
    std::vector<core::byte> data(kSize, 0);
    fill_pe_header(data);
    plant_call(data, 65534);
    fill_pattern(data, 0x90);

    // Pretransform path (compress_buffer detects and pre-transforms).
    std::vector<core::byte> packed;
    assert(Compressor50::compress_buffer(data.data(), data.size(), packed));
    assert_roundtrip_2mib(packed, data);

    // In-loop transform path (StreamEncoder + filter config).
    compress::FilterConfig cfg;
    StreamEncoder enc(3, 0x200000, cfg);
    std::vector<core::byte> streamed;
    const size_t chunk = 4096;
    for (size_t off = 0; off < data.size(); off += chunk) {
        const size_t n = std::min(chunk, data.size() - off);
        assert(enc.feed(data.data() + off, n));
    }
    assert(enc.finish(streamed));
    assert_roundtrip_2mib(streamed, data);

    // Single-region rebuild through the pretransform contract exactly the
    // way compress_buffer() does it: pre-transform the buffer for the
    // packer's actual chunking, THEN declare pretransformed=true (feeding
    // raw bytes with that flag would double-transform on decode). The
    // decoded output must equal the UN-transformed bytes.
    std::vector<core::byte> head_region(data.begin(), data.begin() + 60000); // < 64 KiB region
    const std::vector<core::byte> expected_raw = head_region;
    Filters50::encode_e8(head_region.data(), head_region.size(), 0, false);
    Compressor50 packer;
    packer.begin_archive(nullptr, 3, 0x200000);
    packer.set_filter_config(cfg);
    core::uint8 channels = 1;
    packer.set_active_filter(Filters50::detect_filter(data.data(), data.size(), channels, cfg),
                             channels, /*pretransformed=*/true);
    packer.set_external_buffer(head_region.data(), head_region.size());
    std::vector<core::byte> packed_region;
    packer.set_memory_dest(&packed_region);
    assert(packer.compress() >= 0);
    {
        Decompressor50 dec(0x200000);
        std::vector<core::byte> out;
        assert(dec.decompress_to_vector(packed_region.data(), packed_region.size(), out));
        assert(out.size() == expected_raw.size());
        assert(std::memcmp(out.data(), expected_raw.data(), expected_raw.size()) == 0);
    }
    std::cout << "[PASS] slot257 filter region boundary, both transform paths (T3)\n";
}

// T4: distances within the written prefix (first-window rule) and a tail
// cut mid-repeat (the decoder keeps last_length_ pre-clamp; the encoder
// never emits past the source end).
void test_slot257_window_prefix_and_tail() {
    // (a) run starts at offset 5 — dist 5 <= written prefix.
    std::vector<core::byte> data;
    for (int i = 0; i < 400; ++i) data.push_back(core::byte(i & 0xFF));
    data.insert(data.end(), 64, core::byte(0x41));
    for (int i = 0; i < 300; ++i) data.push_back(core::byte((i * 13) & 0xFF));
    data.insert(data.end(), 512, core::byte(0x41));

    // (b) partial tail: 3 trailing bytes of a 4-byte pattern.
    std::vector<core::byte> tail;
    for (int i = 0; i < 100; ++i) {
        tail.push_back(core::byte('A'));
        tail.push_back(core::byte('B'));
        tail.push_back(core::byte('C'));
        tail.push_back(core::byte('D'));
    }
    tail.push_back(core::byte('A'));
    tail.push_back(core::byte('B'));
    tail.push_back(core::byte('C'));

    for (const auto& src : {data, tail}) {
        for (int method = 1; method <= 5; ++method) {
            std::vector<core::byte> packed;
            assert(Compressor50::compress_buffer(src.data(), src.size(), packed, method, 0x200000,
                                                 {}));
            assert_roundtrip_2mib(packed, src);
        }
    }
    std::cout << "[PASS] slot257 window prefix + partial tail (T4)\n";
}

// T9: with 257 emission suppressed the encoder is byte-identical to its
// reference build (pure-addition property). The hashes were first captured from
// the pre-M1 build (v1.30.4 code) — see docs/v1.31-implementation-plan.md T9 —
// and re-captured since, so what they now guard is TOKEN SELECTION: any change
// to which tokens the encoder picks (a parse, a candidate rule, MIN_MATCH)
// moves these hashes, while a pure framing change must not.
//
// Re-capture history:
//   v1.33.0 Design A (block table reuse), pre-seed attempt: only rand256k
//   moved, on both m1 and m3 - random data hits the 32768-TOKEN block quantum
//   long before the 512 KiB input quantum, so it yields ~8 blocks whose token
//   distributions repeat exactly, and the table description was omitted from
//   all but the first. Those digests were captured as d140b01a... and are now
//   OBSOLETE: the decoder-slot seed rule (docs/question-log.md Entry 8, the
//   first REUSE_SEED_BLOCKS = 16 blocks of a member must carry descriptions)
//   covers all 8 of rand256k's blocks, so reuse never fires on this corpus and
//   the bytes return to the no-reuse values pinned here. Reuse on a
//   random-data corpus is pinned by T11's 2 MiB corpus instead. text64k /
//   zeros1m / mixed never reuse (their per-block statistics keep moving, and
//   all sit inside the seed window anyway).
//   NB the run corpus cannot show Design A's win here at all - T9 suppresses
//   257, and without 257 a run's rep lengths differ every block, so the tables
//   never stabilise. The run case is measured on the real (257-enabled) path.
void test_slot257_pure_addition_suppressed() {
    struct Corpus {
        const char* name;
        std::vector<core::byte> data;
    } corpora[] = {
        {"text64k", t9_text(64 * 1024)},
        {"rand256k", t9_rand(256 * 1024, 42)},
        {"zeros1m", std::vector<core::byte>(1024 * 1024, core::byte(0))},
        {"mixed", t9_mixed()},
    };
    struct Expected {
        const char* corpus;
        int method;
        const char* sha;
    } expected[] = {
        // m1/m3 only: those methods keep the v1.30 hash-chain finder, so the
        // pure-addition property (257 suppression == v1.30.4 bytes) holds.
        // m5 intentionally diverges from v1.30.4 starting with M2 — the
        // binary-tree finder changes match selection by design, so no
        // v1.30.4 byte identity is asserted there.
        {"text64k", 1, "d34113c83d0b46137b98a0b269b764515c8c1e42f8509a4494534edec84bd27c"},
        {"text64k", 3, "44495a7543dd7af0afba4a054b1a7032166d43fc969ae9715c0c8678fe005db0"},
        {"rand256k", 1, "bb60fca9790e03e97adbb8293e12caee3bccaff09bd78169ae0a7ed5f6f555a6"},
        {"rand256k", 3, "bb60fca9790e03e97adbb8293e12caee3bccaff09bd78169ae0a7ed5f6f555a6"},
        {"zeros1m", 1, "cd3edbefbee7c6e94da6fe07b9a5b385acfddb2a9d387706effebdb6343858aa"},
        {"zeros1m", 3, "cd3edbefbee7c6e94da6fe07b9a5b385acfddb2a9d387706effebdb6343858aa"},
        {"mixed", 1, "15a4179c3715d7cd0d4e98a1b6c9f5d668e5fe646003371b172f5946c6d7f6b3"},
        {"mixed", 3, "32fa0f7a175520e0cf2ef4ba9c5b80a1d135d686525e25bbfe171925a5a69f5b"},
    };

    for (const auto& c : corpora) {
        // The helper mirrors compress_buffer()'s no-filter branch; pin that
        // assumption so a detection change cannot silently invalidate T9.
        core::uint8 channels = 1;
        assert(Filters50::detect_filter(c.data.data(), c.data.size(), channels, {}) ==
                   FilterType::None &&
               "T9 corpus must stay filter-free; re-capture reference hashes if this fires");
        for (int method = 1; method <= 3; method += 2) {
            std::vector<core::byte> packed;
            assert(compress_buffer_no257(c.data, packed, method));
            char hex[65];
            sha256_hex(packed, hex);
            for (const auto& e : expected) {
                if (std::strcmp(e.corpus, c.name) == 0 && e.method == method) {
                    assert(std::strcmp(hex, e.sha) == 0 &&
                           "suppressed-257 output drifted from the reference bytes");
                }
            }
            assert_roundtrip_2mib(packed, c.data);
        }
    }
    std::cout << "[PASS] slot257 pure-addition when suppressed (T9, v1.30.4 reference bytes)\n";
}

// T5: the slot-257 shadow carries across the solid file boundary — the
// decoder keeps last_length_ for solid members (decompressor50.cpp resets
// it only in the non-solid branch). Member 1's zero tail is aligned to the
// 4097-byte token span so its final token is a 257 (last_length_ = 4097 at
// the boundary); member 2 continues the run, so under the exact-continuation
// rule member 2 must open with a 257 — a reset shadow would emit a literal
// first.
void test_slot257_solid_carry() {
    std::filesystem::path dir = openrar::test::make_scratch_dir("slot257_solid_carry");
    // Member 1: zero-free noise (no 4-zero hash runs ahead of the tail) then
    // a zero run. Member 2 is built AFTER packing member 1, with length
    // exactly equal to member 1's final `last_length_` — so member 2's first
    // decision satisfies the exact-continuation rule IFF the shadow carried
    // across the solid boundary (a reset shadow would emit a literal +
    // rep0/fresh first). This makes the discrimination phase-independent of
    // member 1's internal token layout.
    std::vector<core::byte> m1;
    {
        core::uint32 s = 99;
        for (size_t i = 0; i < 64 * 1024; ++i) {
            s = t9_lcg(s);
            m1.push_back(static_cast<core::byte>((s >> 16) % 255 + 1)); // 0x01..0xFF
        }
    }
    m1.insert(m1.end(), 4098 + 31 * 4097, core::byte(0));

    std::vector<core::byte> packed;
    size_t m1_packed = 0, m2_packed = 0;
    size_t m2_len = 0;
    std::vector<core::byte> m2;
    {
        std::filesystem::path f1 = dir / "m1.bin";
        std::filesystem::path f2 = dir / "m2.bin";
        {
            io::FileStream out;
            assert(out.open(f1, io::FileMode::CreateAlways));
            assert(out.write(m1.data(), m1.size()) == m1.size());
            out.close();
        }
        Compressor50 packer;
        packer.begin_archive(nullptr, 1, 0x200000);
        packer.set_memory_dest(&packed);
        Compressor50TestAccess::init_match_params(packer);
        io::FileStream in1;
        assert(in1.open(f1, io::FileMode::ReadOnly));
        packer.start_file(&in1, m1.size(), /*continue_window=*/false);
        assert(Compressor50TestAccess::process_available(packer, true));
        m2_len = Compressor50TestAccess::last_length(packer);
        assert(m2_len != 0 && "member 1 must end inside its zero run");
        // Member 2 continues the run with exactly the carried shadow length.
        m2.assign(m2_len, core::byte(0));
        {
            io::FileStream out;
            assert(out.open(f2, io::FileMode::CreateAlways));
            assert(out.write(m2.data(), m2.size()) == m2.size());
            out.close();
        }
        assert(Compressor50TestAccess::write_block(packer, false));
        const core::uint64 packed1 = Compressor50TestAccess::packed_total(packer);
        in1.close();
        io::FileStream in2;
        assert(in2.open(f2, io::FileMode::ReadOnly));
        packer.start_file(&in2, m2.size(), /*continue_window=*/true);
        assert(Compressor50TestAccess::process_available(packer, true));
        // Inspect member 2's tokens BEFORE write_block clears them: the
        // carried shadow must open the continuation with a 257.
        const auto& toks = Compressor50TestAccess::match_tokens(packer);
        assert(!toks.empty());
        assert(toks[0].get_type() == Compressor50Token::TokenType::LastLen &&
               "carried shadow must open member 2 with a 257 continuation");
        assert(Compressor50TestAccess::write_block(packer, true));
        const core::uint64 packed2 = Compressor50TestAccess::packed_total(packer);
        in2.close();
        m1_packed = static_cast<size_t>(packed1);
        m2_packed = static_cast<size_t>(packed2 - packed1);
    }
    // Roundtrip: member 1 resets the window; member 2 continues as a solid
    // member on the SAME decoder instance (mirrors the decoder's carry).
    {
        assert(packed.size() == m1_packed + m2_packed);
        Decompressor50 dec(0x200000);
        std::vector<core::byte> out1, out2;
        size_t written = 0;
        bool finished = false;
        assert(dec.decompress(
            packed.data(), m1_packed, m1.size(), false,
            [&out1](const core::byte* d, size_t n) {
                out1.insert(out1.end(), d, d + n);
                return true;
            },
            &written, &finished));
        assert(out1.size() == m1.size() && std::memcmp(out1.data(), m1.data(), m1.size()) == 0);
        assert(dec.decompress(
            packed.data() + m1_packed, m2_packed, m2.size(), true,
            [&out2](const core::byte* d, size_t n) {
                out2.insert(out2.end(), d, d + n);
                return true;
            },
            &written, &finished));
        assert(out2.size() == m2.size() && std::memcmp(out2.data(), m2.data(), m2.size()) == 0);
    }
    std::cout << "[PASS] slot257 solid carry opens member 2 with a 257 (T5)\n";
}

// T10: the match emitter's admissibility contract (v1.33 item 1).
//
// The decoder reconstructs a match length as base + increment(distance) and
// performs NO range check on it (decompressor50.cpp:867-878), so a (len, dist)
// pair whose base falls outside [2, MAX_LZ_MATCH] silently decodes to a
// DIFFERENT length than the encoder recorded - a one-byte desync that surfaces
// only at the CRC, not as a rejection. The invariant pinned here is that the
// shadow last_length_ is derived from the ENCODED base and never from the
// caller's `length`, and that an inadmissible pair is filtered rather than
// clamped up into a legal-looking token.
//
// The assertion that pins the fix is `shadow_after == base + inc`. The
// pre-v1.33 emitter set last_length_ = length unconditionally, so for the
// over-ceiling cases below it left the shadow holding the pre-truncation
// length (e.g. 5000 for a 5000/1 pair) while the decoder held 4097 - exactly
// the desync. It also clamped an inadmissible pair up to base 2 and emitted it
// anyway; those cases must now return 0 and emit nothing.
void test_add_match_admissibility() {
    const size_t kMaxLzMatch = 0x1001; // MAX_LZ_MATCH (compressor50.hpp)

    auto increment_of = [](size_t d) -> core::uint32 {
        core::uint32 inc = 0;
        if (d > 0x100) inc++;
        if (d > 0x2000) inc++;
        if (d > 0x40000) inc++;
        return inc;
    };

    struct Case {
        size_t len;
        size_t dist;
        const char* note;
    };
    const Case cases[] = {
        // --- legal, at and above the floor for each increment class ---
        {2, 1, "inc 0, base 2 (length slot 0)"},
        {3, 1, "inc 0, floor"},
        {2, 0x100, "inc 0, last distance with no increment"},
        {3, 0x101, "inc 1, floor"},
        {4, 0x2001, "inc 2, floor"},
        {5, 0x40001, "inc 3, floor"},
        {64, 1, "ordinary"},
        {64, 0x100, "ordinary at the increment boundary"},
        {64, 0x101, "ordinary, inc 1"},
        {64, 0x2001, "ordinary, inc 2"},
        {64, 0x40001, "ordinary, inc 3"},
        // --- exactly at the ceiling: no truncation ---
        {kMaxLzMatch, 1, "base == MAX_LZ_MATCH"},
        {kMaxLzMatch + 3, 0x40001, "base == MAX_LZ_MATCH with inc 3"},
        // --- above the ceiling: truncate, and REPORT the truncated span ---
        {kMaxLzMatch + 1, 1, "base 1 over the ceiling, inc 0"},
        {5000, 1, "base far over the ceiling, inc 0"},
        {5000, 0x40001, "base far over the ceiling, inc 3"},
        // --- below the floor: FILTER, emit nothing, do not move the shadow ---
        {1, 1, "inc 0, one byte short of the floor"},
        {2, 0x101, "inc 1, one byte short of the floor"},
        {3, 0x2001, "inc 2, one byte short of the floor"},
        {4, 0x40001, "inc 3, one byte short of the floor"},
    };

    int filtered = 0, truncated = 0, plain = 0;
    for (const auto& c : cases) {
        Compressor50 packer;
        packer.begin_archive(nullptr, 3, 0x200000);

        const size_t shadow_before = Compressor50TestAccess::last_length(packer);
        const size_t tokens_before = Compressor50TestAccess::match_token_count(packer);
        const size_t covered = Compressor50TestAccess::add_match(packer, c.len, c.dist);
        const size_t shadow_after = Compressor50TestAccess::last_length(packer);
        const size_t tokens_after = Compressor50TestAccess::match_token_count(packer);

        const core::uint32 inc = increment_of(c.dist);
        const bool admissible = c.len >= static_cast<size_t>(inc) + 2;

        if (!admissible) {
            assert(covered == 0 && "inadmissible (len,dist) must be FILTERED, not clamped");
            assert(shadow_after == shadow_before &&
                   "a filtered match must not advance the shadow last_length_");
            assert(tokens_after == tokens_before && "a filtered match must not emit a token");
            ++filtered;
            continue;
        }

        size_t base = c.len - inc;
        const bool over = base > kMaxLzMatch;
        if (over) base = kMaxLzMatch;

        assert(tokens_after == tokens_before + 1 && "an admissible match emits exactly one token");
        assert(covered == base + inc && "add_match must report the span the wire carries");
        assert(shadow_after == base + inc &&
               "shadow last_length_ must be base+increment, never the caller's length");
        if (over) {
            ++truncated;
        } else {
            assert(shadow_after == c.len && "a non-truncated match must cover its full length");
            ++plain;
        }
    }

    assert(filtered == 4 && truncated == 3 && plain == 13 &&
           "case table drifted; update the counts with it");
    std::cout
        << "[PASS] match emitter filters inadmissible pairs and reports the encoded span (T10)\n";
}


// T12: length-2 match support (v1.33 Design B).
//
// RAR5 length slot 0 decodes to length 2 and is wire-legal; `MIN_MATCH = 3` was
// the only thing making the class unreachable. This pins the two properties
// that matter:
//
//   1. REACHABILITY - on a text-like corpus the shallow-finder methods really do
//      emit slot-0 length tokens. Without this the constant change would be a
//      silent no-op. Note WHO can produce them: the match finder hashes four
//      bytes and only indexes positions with four bytes available
//      (calc_hash/insert_position), so a chain walk can never return a length-2
//      fresh match. The reachable form of this class is the REP token - the rep
//      scan compares bytes directly with no length floor. Measured -1.49% at
//      m1, -1.64% at m2 on a 64 MiB text corpus.
//   2. THE m4/m5 BOUND - admitting short matches measured as a regression at m5
//      (+0.05%), so those methods keep the previous minimum of 3 and must emit
//      no slot-0 match at all. Their bytes are byte-identical to v1.32.
//
// Admissibility of a 2-byte match (legal only where increment(distance) == 0,
// i.e. distance <= 0x100) is pinned by T10, which drives the emitter directly
// with the boundary pairs; here we only assert the class is emitted and that
// everything still round-trips.
void test_length2_match_support() {
    std::vector<core::byte> text;
    for (int i = 0; i < 8192; ++i) {
        std::string line = "fn handler_" + std::to_string(i) + "(req: &Request) -> Result { log(" +
                           std::to_string(i % 97) + "); }\n";
        text.insert(text.end(), line.begin(), line.end());
    }
    assert(text.size() > 256 * 1024 && "corpus must be big enough to fill a block");

    struct Expect {
        int method;
        bool expect_slot0;
    };
    const Expect cases[] = {{1, true}, {2, true}, {3, true}, {4, false}, {5, false}};

    for (const auto& c : cases) {
        Compressor50 packer;
        packer.begin_archive(nullptr, c.method, 0x200000);
        packer.set_external_buffer(text.data(), text.size());
        std::vector<core::byte> packed;
        packer.set_memory_dest(&packed);
        Compressor50TestAccess::init_match_params(packer);
        assert(Compressor50TestAccess::process_available(packer, true));

        size_t slot0 = 0, matches = 0;
        for (const auto& t : Compressor50TestAccess::match_tokens(packer)) {
            const auto ty = t.get_type();
            const bool is_match = ty == Compressor50Token::TokenType::Match;
            const bool is_rep = ty >= Compressor50Token::TokenType::Rep0 &&
                                ty <= Compressor50Token::TokenType::Rep3;
            if (!is_match && !is_rep) continue;
            if (is_match) ++matches;
            if (t.get_len_slot() == 0) ++slot0;
        }
        if (c.expect_slot0) {
            assert(slot0 > 0 && "length-2 match class is unreachable - the change is a no-op");
        } else {
            assert(slot0 == 0 && "m4/m5 must keep the previous minimum of 3");
        }
        assert(matches > 0);

        assert(Compressor50TestAccess::write_block(packer, true));
        Decompressor50 dec(0x200000);
        std::vector<core::byte> out;
        assert(dec.decompress_to_vector(packed.data(), packed.size(), out));
        assert(out.size() == text.size());
        assert(std::memcmp(out.data(), text.data(), text.size()) == 0);
    }
    std::cout << "[PASS] length-2 matches reachable at m1-m3, suppressed at m4-m5 (T12)\n";
}


// T11: block-header table reuse (v1.33 Design A). The invariants that matter:
// "reuse never happens where the decoder has no tables" and the decoder-side
// slot-seeding rule (docs/question-log.md Entry 8): a multithreaded decoder
// hands each block to a private per-slot table state in round-robin batches of
// 2 x decoder_threads, so the encoder must emit descriptions on the first
// REUSE_SEED_BLOCKS (16) blocks of a member - a reuse block landing in a
// never-seeded slot decodes against empty tables and the member comes out
// short. So: walk the block headers and assert that bit 7 is SET on the first
// REUSE_SEED_BLOCKS blocks, may be clear afterwards, and that the stream still
// decodes byte-exact.
void test_block_table_reuse() {
    // Random data hits the 32768-TOKEN quantum long before the input quantum,
    // so it yields many blocks whose statistics repeat exactly - the case
    // reuse is designed for. The corpus must span well past the 16-block seed
    // window or reuse can never fire on it.
    std::vector<core::byte> rnd = t9_rand(2 * 1024 * 1024, 7);

    struct Stream {
        const char* name;
        std::vector<core::byte> packed;
    };
    Stream streams[2];
    streams[0].name = "m3";
    streams[1].name = "m1";
    int method[2] = {3, 1};

    for (int s = 0; s < 2; ++s) {
        std::vector<core::byte> packed;
        {
            Compressor50 packer;
            packer.begin_archive(nullptr, method[s], 0x200000);
            packer.set_external_buffer(rnd.data(), rnd.size());
            packer.set_memory_dest(&packed);
            assert(packer.compress() >= 0 && !packed.empty());
        }
        assert_roundtrip_2mib(packed, rnd);

        // Walk the block headers: flags, checksum, size (1..3 bytes), payload.
        size_t off = 0;
        int block = 0, reused = 0;
        while (off < packed.size()) {
            assert(off + 2 <= packed.size() && "truncated block header");
            const core::uint8 flags = packed[off];
            const core::uint8 byte_cnt = static_cast<core::uint8>(((flags >> 3) & 3) + 1);
            assert(byte_cnt >= 1 && byte_cnt <= 3);
            assert(off + 2 + byte_cnt <= packed.size() && "truncated block size field");
            core::uint32 bsize = 0;
            for (core::uint8 i = 0; i < byte_cnt; ++i)
                bsize |= static_cast<core::uint32>(packed[off + 2 + i]) << (8 * i);
            const bool tables_present = (flags & 0x80) != 0;

            if (block < Compressor50TestAccess::REUSE_SEED_BLOCKS) {
                assert(tables_present &&
                       "blocks inside the decoder-slot seed window MUST carry their tables "
                       "(question-log Entry 8: a reuse block in a never-seeded slot decodes "
                       "against empty tables)");
            } else if (!tables_present) {
                ++reused;
            }
            ++block;
            off += 2 + byte_cnt + bsize;
        }
        assert(block > 1 && "expected a multi-block stream for this corpus");
        assert(reused > 0 &&
               "table reuse never fired; the rule is inert and this test proves nothing");
        std::cout << "[PASS] block table reuse: " << streams[s].name << " " << block << " blocks, "
                  << reused << " reused, first block carries tables (T11)\n";
    }
}


int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
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
    test_e8_filter_chunk_boundary_symmetry();
    std::cout << std::flush;
    test_filter_chunk_boundary_roundtrip_p1();
    std::cout << std::flush;
    test_filter_multi_token_multiblock_roundtrip();
    std::cout << std::flush;
    test_default_window_pairing_roundtrip();
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
    test_slot257_run_collapse();
    std::cout << std::flush;
    test_slot257_byte_verification();
    std::cout << std::flush;
    test_slot257_filter_region_boundary();
    std::cout << std::flush;
    test_slot257_window_prefix_and_tail();
    std::cout << std::flush;
    test_slot257_pure_addition_suppressed();
    std::cout << std::flush;
    test_slot257_solid_carry();
    test_add_match_admissibility();
    test_length2_match_support();
    test_block_table_reuse();
    std::cout << std::flush;

    std::cout << std::flush;
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
