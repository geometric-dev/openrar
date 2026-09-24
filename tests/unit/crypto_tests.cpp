#include "../../src/core/types.hpp"
#include "../../src/core/cpu.hpp"
#include "../../src/core/vint.hpp"
#include "../../src/core/error.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/crypto/crc64.hpp"
#include "../../src/crypto/sha256.hpp"
#include "../../src/crypto/blake2sp.hpp"
#include "../../src/crypto/aes256.hpp"
#include "../../src/crypto/pbkdf2.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;

static std::string to_hex(const core::byte* data, size_t size) {
    std::ostringstream oss;
    for (size_t i = 0; i < size; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

void test_types_and_endian() {
    core::byte buf[8];
    core::write_le32(buf, 0x12345678);
    assert(buf[0] == 0x78 && buf[1] == 0x56 && buf[2] == 0x34 && buf[3] == 0x12);
    assert(core::read_le32(buf) == 0x12345678);

    core::write_be32(buf, 0x12345678);
    assert(buf[0] == 0x12 && buf[1] == 0x34 && buf[2] == 0x56 && buf[3] == 0x78);
    assert(core::read_be32(buf) == 0x12345678);

    assert(core::is_power_of_two(1));
    assert(core::is_power_of_two(1024));
    assert(!core::is_power_of_two(0));
    assert(!core::is_power_of_two(1000));
    std::cout << "[PASS] Types and Endian Utilities\n";
}

void test_vint() {
    const core::uint64 test_vals[] = {0,      1,      0x7F,        0x80,
                                      0x3FFF, 0x4000, 0x1FFFFFULL, 0xFFFFFFFFFFFFFFFFULL};

    for (auto val : test_vals) {
        std::vector<core::byte> buf;
        core::push_vint(buf, val);

        core::uint64 decoded = 0;
        size_t read_bytes = 0;
        bool ok = core::read_vint(buf.data(), buf.size(), decoded, read_bytes);
        assert(ok);
        assert(decoded == val);
        assert(read_bytes == buf.size());
    }

    // Fixed-width vint
    std::vector<core::byte> fixed_buf;
    core::push_vint_fixed(fixed_buf, 42, 4);
    assert(fixed_buf.size() == 4);
    core::uint64 decoded = 0;
    size_t read_bytes = 0;
    bool ok = core::read_vint(fixed_buf.data(), fixed_buf.size(), decoded, read_bytes);
    // INTEROP (see vint.cpp): padded/non-minimal encodings MUST decode —
    // they occur in real archives (locator offsets, data_size/unp_size fields)
    // and the format specification accepts them.
    assert(ok);
    assert(decoded == 42);
    assert(read_bytes == fixed_buf.size());

    // Hardening Test: 100 consecutive 0x80 bytes (unbounded continuation)
    std::vector<core::byte> unbounded_buf(100, 0x80);
    ok = core::read_vint(unbounded_buf.data(), unbounded_buf.size(), decoded, read_bytes);
    assert(!ok); // Must reject cleanly via max-bytes cap, not overflow

    // Padded zero: 0x80 0x80 0x00 — padded locator quick-open offset value 0
    // in real archives. Must decode to 0.
    std::vector<core::byte> middle_zero_buf = {0x80, 0x80, 0x00};
    ok = core::read_vint(middle_zero_buf.data(), middle_zero_buf.size(), decoded, read_bytes);
    assert(ok);
    assert(decoded == 0);
    assert(read_bytes == 3);

    // Hardening Test: UINT64_MAX + 1 encoding (overflow)
    // UINT64_MAX is 0xFF (9 times) and 0x7F (10th byte).
    // +1 would mean 0x80 as 10th byte, or any bits beyond 64.
    std::vector<core::byte> overflow_buf(10, 0xFF);
    overflow_buf[9] = 0x80; // 10th byte has continuation bit set! Max is 10.
    ok = core::read_vint(overflow_buf.data(), overflow_buf.size(), decoded, read_bytes);
    assert(!ok);

    // Another Overflow Test: Shift overflow
    // 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0x02
    std::vector<core::byte> shift_overflow_buf(10, 0xFF);
    shift_overflow_buf[9] = 0x02; // Sets bit 64, which exceeds 64-bit uint
    ok = core::read_vint(shift_overflow_buf.data(), shift_overflow_buf.size(), decoded, read_bytes);
    assert(!ok);

    // Truncated stream: a multi-byte vint given fewer bytes than it needs
    // must fail cleanly rather than read past the buffer end.
    std::vector<core::byte> multi;
    core::push_vint(multi, 300); // 2 bytes
    assert(multi.size() == 2);
    ok = core::read_vint(multi.data(), 1, decoded, read_bytes);
    assert(!ok);
    ok = core::read_vint(multi.data(), 2, decoded, read_bytes);
    assert(ok && decoded == 300);

    std::cout << "[PASS] RAR5 Variable-Length Integer (vint)\n";
}

void test_crc32() {
    const char* data = "123456789";
    core::uint32 val = crypto::crc32(data, 9);
    if (val != 0xCBF43926) {
        const auto& cpu = core::get_cpu_features();
        std::fprintf(stderr,
                     "CRC32 mismatch: got %08X, expected CBF43926 "
                     "(arm_crc32=%d pclmulqdq=%d)\n",
                     val, cpu.arm_crc32 ? 1 : 0, cpu.pclmulqdq ? 1 : 0);
        assert(false && "crc32('123456789') != 0xCBF43926");
    }

    // Streaming CRC
    crypto::Crc32 stream;
    stream.update("1234", 4);
    stream.update("56789", 5);
    assert(stream.get() == 0xCBF43926);

    std::cout << "[PASS] CRC32 (Slicing-by-8 IEEE 802.3)\n";
}

void test_sha256() {
    core::byte digest[32];

    // Empty string
    crypto::Sha256::compute("", 0, digest);
    assert(to_hex(digest, 32) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // "abc"
    crypto::Sha256::compute("abc", 3, digest);
    assert(to_hex(digest, 32) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // HMAC-SHA256 test vector (RFC 4231 Case 2: key="Jefe", data="what do ya want for nothing?")
    const char* key = "Jefe";
    const char* msg = "what do ya want for nothing?";
    crypto::HmacSha256::compute(key, 4, msg, 28, digest);
    assert(to_hex(digest, 32) ==
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    std::cout << "[PASS] SHA-256 and HMAC-SHA256\n";
}

void test_blake2sp() {
    core::byte digest[32];

    // BLAKE2sp empty string (RFC 7693: fanout=8, depth=2)
    crypto::Blake2sp::compute("", 0, digest);
    assert(to_hex(digest, 32) ==
           "dd0e891776933f43c7d032b08a917e25741f8aa9a12c12e1cac8801500f2ca4f");

    std::cout << "[PASS] BLAKE2sp (RFC 7693)\n";
}

void test_aes256_cbc() {
    // 256-bit key
    core::byte key[32] = {0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae,
                          0xf0, 0x85, 0x7d, 0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61,
                          0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4};

    // 128-bit IV
    core::byte iv[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                         0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    core::byte original_iv[16];
    std::memcpy(original_iv, iv, 16);

    // 64 bytes plaintext
    core::byte plaintext[64];
    for (int i = 0; i < 64; ++i) plaintext[i] = static_cast<core::byte>(i);

    core::byte buffer[64];
    std::memcpy(buffer, plaintext, 64);

    crypto::Aes256 aes(key);

    // Encrypt CBC
    aes.encrypt_cbc(buffer, 64, iv);
    assert(std::memcmp(buffer, plaintext, 64) != 0); // Must be ciphertext

    // Decrypt CBC
    std::memcpy(iv, original_iv, 16);
    aes.decrypt_cbc(buffer, 64, iv);
    assert(std::memcmp(buffer, plaintext, 64) == 0); // Must match original plaintext

    std::cout << "[PASS] AES-256 CBC Encrypt / Decrypt Roundtrip\n";
}

void test_cbc_rejects_unaligned_size() {
    // Q5 regression: encrypt/decrypt_cbc used to silently floor to whole
    // blocks, dropping a trailing partial block without any diagnostic. The
    // primitive must refuse non-multiple-of-16 sizes and touch no bytes.
    const core::byte key[32] = {0};
    core::byte iv[16] = {0};
    core::byte data[20];
    for (int i = 0; i < 20; ++i) data[i] = static_cast<core::byte>(0xA0 + i);
    core::byte snapshot[20];
    std::memcpy(snapshot, data, 20);

    crypto::Aes256 aes(key);
    assert(aes.encrypt_cbc(data, 20, iv) == false);
    assert(aes.decrypt_cbc(data, 20, iv) == false);
    assert(std::memcmp(data, snapshot, 20) == 0);  // buffer untouched on refusal
    assert(aes.encrypt_cbc(data, 0, iv) == true);  // zero blocks is a valid no-op
    assert(aes.decrypt_cbc(data, 16, iv) == true); // aligned still works

    std::cout << "[PASS] AES-256 CBC rejects unaligned size without touching data (Q5)\n";
}

void test_pbkdf2() {
    const char* pwd = "TestPassword";
    core::byte salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    crypto::Rar5Keys keys;
    crypto::Pbkdf2Rar5::derive_keys(pwd, salt, 16, 64, keys);

    // Check that keys are generated and non-zero
    bool non_zero_key = false;
    for (int i = 0; i < 32; ++i)
        if (keys.aes_key[i] != 0) non_zero_key = true;
    assert(non_zero_key);

    // Check that psw_check_csum matches Sha256(psw_check)[0:4]
    core::byte csum[32];
    crypto::Sha256::compute(keys.psw_check, 8, csum);
    assert(std::memcmp(csum, keys.psw_check_csum, 4) == 0);

    std::cout << "[PASS] RAR 5.0 PBKDF2 Key Derivation\n";
}

// Regression (report L2): count == 0 used to wrap to 0xFFFFFFFF in the
// iteration split (cur_count[0] = count - 1), i.e. ~4.3 billion HMAC
// iterations from a single public-API call. It must fail cleanly instead:
// return false and leave the keys zeroed, immediately.
void test_pbkdf2_zero_count_fails_cleanly() {
    const char* pwd = "TestPassword";
    core::byte salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    crypto::Rar5Keys keys;
    std::memset(keys.aes_key, 0xAB, sizeof(keys.aes_key));
    std::memset(keys.hash_key, 0xAB, sizeof(keys.hash_key));
    std::memset(keys.psw_check, 0xAB, sizeof(keys.psw_check));
    std::memset(keys.psw_check_csum, 0xAB, sizeof(keys.psw_check_csum));

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = crypto::Pbkdf2Rar5::derive_keys(pwd, salt, 16, 0, keys);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    assert(!ok);
    // Would take minutes-to-hours if the count wrapped; the guard is O(1).
    assert(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10);
    const core::byte* p = reinterpret_cast<const core::byte*>(&keys);
    for (size_t i = 0; i < sizeof(keys); ++i) assert(p[i] == 0);

    std::cout << "[PASS] PBKDF2 count == 0 fails cleanly (zeroed keys, no wrap)\n";
}

// Report L3: the PswCheck compare in the encrypted-header reader must not
// early-exit on the first mismatching byte.
void test_constant_time_equal() {
    const core::byte a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const core::byte b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const core::byte c[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    const core::byte d[8] = {9, 2, 3, 4, 5, 6, 7, 8};
    assert(crypto::Pbkdf2Rar5::constant_time_equal(a, b, 8));
    assert(!crypto::Pbkdf2Rar5::constant_time_equal(a, c, 8)); // mismatch at last byte
    assert(!crypto::Pbkdf2Rar5::constant_time_equal(a, d, 8)); // mismatch at byte 0
    assert(crypto::Pbkdf2Rar5::constant_time_equal(a, a, 0));  // empty compare
    std::cout << "[PASS] constant_time_equal semantics\n";
}

// Known-answer vectors. Everything below pins the primitives to published
// constants so a wrong-but-self-consistent implementation fails here instead
// of only failing against real RAR5 archives.

void test_aes_known_answer() {
    // FIPS-197 C.3: AES-256 single block.
    static const core::byte key[32] = {0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
                                       0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
                                       0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
                                       0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4};
    static const core::byte pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const core::byte ct[16] = {0xd8, 0x34, 0x14, 0x22, 0x3d, 0x20, 0xa0, 0xc9,
                                      0x28, 0xb1, 0x36, 0xc8, 0x84, 0xd0, 0x7e, 0xa2};

    crypto::Aes256 aes(key);
    core::byte block_in[16], block_out[16];
    std::memcpy(block_in, pt, 16);
    aes.encrypt_block(block_in, block_out);
    assert(std::memcmp(block_out, ct, 16) == 0);
    core::byte block_rt[16];
    aes.decrypt_block(block_out, block_rt);
    assert(std::memcmp(block_rt, pt, 16) == 0);

    // NIST SP 800-38A F.2.5: CBC-AES256, four blocks.
    static const core::byte iv0[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                       0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const core::byte plain[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
        0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
        0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
        0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
        0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    static const core::byte cipher[64] = {
        0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba, 0x77, 0x9e, 0xab, 0xfb, 0x5f,
        0x7b, 0xfb, 0xd6, 0x9c, 0xfc, 0x4e, 0x96, 0x7e, 0xdb, 0x80, 0x8d, 0x67, 0x9f,
        0x77, 0x7b, 0xc6, 0x70, 0x2c, 0x7d, 0x39, 0xf2, 0x33, 0x69, 0xa9, 0xd9, 0xba,
        0xcf, 0xa5, 0x30, 0xe2, 0x63, 0x04, 0x23, 0x14, 0x61, 0xb2, 0xeb, 0x05, 0xe2,
        0xc3, 0x9b, 0xe9, 0xfc, 0xda, 0x6c, 0x19, 0x07, 0x8c, 0x6a, 0x9d, 0x1b};

    core::byte buf[64], iv[16];
    std::memcpy(buf, plain, 64);
    std::memcpy(iv, iv0, 16);
    aes.encrypt_cbc(buf, 64, iv);
    assert(std::memcmp(buf, cipher, 64) == 0);

    std::memcpy(buf, cipher, 64);
    std::memcpy(iv, iv0, 16); // encrypt_cbc consumed the original IV
    aes.decrypt_cbc(buf, 64, iv);
    assert(std::memcmp(buf, plain, 64) == 0);

    std::cout << "[PASS] AES-256 known-answer (FIPS-197 C.3 + SP 800-38A CBC)\n";
}

void test_pbkdf2_rfc_vectors() {
    // The aes_key output is exactly standard PBKDF2-HMAC-SHA256 limited to
    // 32 bytes: derive_keys chains U1 = HMAC(pwd, salt || 00 00 00 01) and
    // XORs U1..Ucount into the first 32 bytes (the {count-1,16,16} split
    // reuses the same chain for hash_key/psw_check). Vectors: the widely
    // published PBKDF2-HMAC-SHA256 values for P="password", S="salt".
    const core::byte salt[4] = {'s', 'a', 'l', 't'};
    struct Case {
        core::uint32 count;
        const char* hex;
    };
    const Case cases[] = {
        {1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
        {2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
        {4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
    };
    for (const auto& c : cases) {
        crypto::Rar5Keys keys;
        assert(crypto::Pbkdf2Rar5::derive_keys(std::string("password"), salt, 4, c.count, keys));
        assert(to_hex(keys.aes_key, 32) == c.hex);
    }
    std::cout << "[PASS] PBKDF2-HMAC-SHA256 known-answer vectors (c=1,2,4096)\n";
}

void test_blake2sp_kat() {
    // Official BLAKE2 known-answer tests (blake2-kat.json, unkeyed blake2sp).
    core::byte digest[32];

    // in = 00..0f (16 bytes)
    core::byte in16[16];
    for (int i = 0; i < 16; ++i) in16[i] = static_cast<core::byte>(i);
    crypto::Blake2sp::compute(in16, 16, digest);
    assert(to_hex(digest, 32) ==
           "b9194519e4978a9dc893b28bd808cdfabb1bd510d862b3171ff6e017a41b804c");

    // in = 00..7f (128 bytes): crosses the 64-byte block boundary. Assert
    // both the one-shot path and a streaming path with an uneven split.
    core::byte in128[128];
    for (int i = 0; i < 128; ++i) in128[i] = static_cast<core::byte>(i);
    crypto::Blake2sp::compute(in128, 128, digest);
    assert(to_hex(digest, 32) ==
           "05cf3a90049116dc60efc31536aaa3d167762994892876dcb7ef3fbecd7449c0");

    crypto::Blake2sp streamed;
    streamed.update(in128, 65);
    streamed.update(in128 + 65, 63);
    streamed.finish(digest);
    assert(to_hex(digest, 32) ==
           "05cf3a90049116dc60efc31536aaa3d167762994892876dcb7ef3fbecd7449c0");

    std::cout << "[PASS] BLAKE2sp known-answer (blake2-kat, 16B + 128B + streaming)\n";
}

void test_crc64_xz() {
    // Canonical check value for CRC-64/XZ (see crc64.hpp parameters).
    const char* data = "123456789";
    assert(crypto::Crc64Xz::compute(data, 9) == 0x995DC9BBDF1939FAULL);

    // Streaming split must equal the one-shot value.
    crypto::Crc64Xz stream;
    stream.update(data, 4);
    stream.update(data + 4, 5);
    assert(stream.get() == 0x995DC9BBDF1939FAULL);

    std::cout << "[PASS] CRC-64/XZ known-answer + streaming\n";
}

void test_b6_padded_vint() {
    // 4-byte VINT encoding 2 MiB (2,097,152 = 0x200000):
    // 0x200000 in 7-bit chunks (LSB first): 0x80, 0x80, 0x80, 0x01
    const core::byte vint_2mib[] = {0x80, 0x80, 0x80, 0x01};
    core::uint64 val = 0;
    size_t read_bytes = 0;
    bool ok = core::read_vint(vint_2mib, sizeof(vint_2mib), val, read_bytes);
    assert(ok);
    assert(val == 2097152);
    assert(read_bytes == 4);

    // Non-canonical padded VINT (5 bytes encoding 42: 0xAA, 0x80, 0x80, 0x80, 0x00)
    const core::byte padded_vint_5[] = {0xAA, 0x80, 0x80, 0x80, 0x00};
    val = 0;
    read_bytes = 0;
    ok = core::read_vint(padded_vint_5, sizeof(padded_vint_5), val, read_bytes);
    assert(ok);
    assert(val == 42);
    assert(read_bytes == 5);

    // 10-byte valid VINT (max length of 64-bit VINT)
    const core::byte max_vint_10[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    val = 0;
    read_bytes = 0;
    ok = core::read_vint(max_vint_10, sizeof(max_vint_10), val, read_bytes);
    assert(ok);
    assert(read_bytes == 10);

    // 11-byte invalid VINT (exceeds 10 bytes)
    const core::byte invalid_vint_11[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                          0x80, 0x80, 0x80, 0x80, 0x01};
    ok = core::read_vint(invalid_vint_11, sizeof(invalid_vint_11), val, read_bytes);
    assert(!ok);

    std::cout << "[PASS] B6: 2 MiB and non-canonical padded VINTs up to 10 bytes\n";
}

void test_secure_wipe_and_rar5_keys() {
    core::byte buf[64];
    std::memset(buf, 0x55, sizeof(buf));
    crypto::secure_wipe(buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i) {
        assert(buf[i] == 0);
    }

    crypto::Rar5Keys keys;
    std::memset(keys.aes_key, 0xAA, sizeof(keys.aes_key));
    std::memset(keys.hash_key, 0xBB, sizeof(keys.hash_key));
    std::memset(keys.psw_check, 0xCC, sizeof(keys.psw_check));
    std::memset(keys.psw_check_csum, 0xDD, sizeof(keys.psw_check_csum));

    keys.wipe();
    for (size_t i = 0; i < sizeof(keys.aes_key); ++i) assert(keys.aes_key[i] == 0);
    for (size_t i = 0; i < sizeof(keys.hash_key); ++i) assert(keys.hash_key[i] == 0);
    for (size_t i = 0; i < sizeof(keys.psw_check); ++i) assert(keys.psw_check[i] == 0);
    for (size_t i = 0; i < sizeof(keys.psw_check_csum); ++i) assert(keys.psw_check_csum[i] == 0);

    // Test RAII destructor zeroing on storage
    alignas(crypto::Rar5Keys) core::byte raw_mem[sizeof(crypto::Rar5Keys)];
    std::memset(raw_mem, 0x77, sizeof(raw_mem));
    auto* k = new (raw_mem) crypto::Rar5Keys();
    assert(raw_mem[0] == 0);
    std::memset(raw_mem, 0x88, sizeof(raw_mem));
    assert(raw_mem[0] == 0x88);
    k->~Rar5Keys();
    for (size_t i = 0; i < sizeof(raw_mem); ++i) {
        assert(raw_mem[i] == 0);
    }

    std::cout << "[PASS] secure_wipe and Rar5Keys RAII memory zeroing\n";
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
    std::cout << "Running Clean-Room Milestone 1 Primitives Verification...\n";
    test_types_and_endian();
    test_vint();
    test_b6_padded_vint();
    test_crc32();
    test_sha256();
    test_blake2sp();
    test_aes256_cbc();
    test_cbc_rejects_unaligned_size();
    test_pbkdf2();
    test_pbkdf2_zero_count_fails_cleanly();
    test_constant_time_equal();
    test_aes_known_answer();
    test_pbkdf2_rfc_vectors();
    test_blake2sp_kat();
    test_crc64_xz();
    test_secure_wipe_and_rar5_keys();
    std::cout << "All Milestone 1 Core & Crypto Primitives PASSED!\n";
    return 0;
}
