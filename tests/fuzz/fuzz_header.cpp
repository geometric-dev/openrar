// Fuzz harness for the RAR5 header PARSERS.
//
// Why this exists (2026-11 code-quality review, Tier-1 gap #1): every other
// archive harness feeds whole archives through read_block_raw(), which
// verifies the block CRC32 *before* the body is handed to a parser. A random
// mutation therefore has roughly a 2^-32 chance of producing a CRC-valid
// block, so parse_file_header() and friends effectively never ran under the
// fuzzer — and those functions hold the extra-record parsers (OWNER, XATTR,
// CRYPT, HTIME, REDIR, SUBDATA, and the main-header LOCATOR/METADATA
// records), which are the most branch-dense attacker-controlled code in the
// tree.
//
// This harness calls the parsers with the body bytes directly. The CRC gate
// is bypassed by construction, so mutations land in the parsing code.
//
// Contract: every parser must be TOTAL on arbitrary input — no crash, no
// UB (sanitizers), no unbounded allocation, no unbounded loop. Return value
// is irrelevant; only termination and memory safety are asserted.
//
// Lanes are not used: running all four parsers on every input is O(body)
// each and removes the routing blind spot that per-lane fuzzing leaves.

#include "../../src/format/header_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace openrar;

namespace {

void exercise_body(const core::byte* body, size_t size) {
    format::MainBlock main_block;
    format::FileBlock file_block;
    format::CryptBlock crypt_block;
    format::EndArcBlock end_block;

    // Each parser is called with the FULL input as its body, exactly as
    // read_block_raw() would after a successful CRC check. A parser that
    // rejects the bytes still has to have done so safely.
    format::HeaderReader::parse_main_header(body, size, main_block);
    format::HeaderReader::parse_file_header(body, size, file_block);
    format::HeaderReader::parse_crypt_header(body, size, crypt_block);
    format::HeaderReader::parse_end_header(body, size, end_block);
}

// The framing path, fed the same bytes. The input almost never carries a
// valid CRC32, so this returns HeaderCrcMismatch — which is the point: it
// exercises the size-vint recovery, the 2 MiB body cap and the short-read
// rejection without the fuzzer having to forge a checksum.
void exercise_framing(const core::byte* data, size_t size) {
    size_t offset = 0;
    core::uint64 type = 0;
    core::uint64 flags = 0;
    core::uint64 data_size = 0;
    std::vector<core::byte> out_body;
    format::HeaderReader::read_block_raw_mem(data, size, offset, type, flags, out_body, data_size);
}

void exercise(const uint8_t* data, size_t size) {
    const auto* body = reinterpret_cast<const core::byte*>(data);
    exercise_body(body, size);
    exercise_framing(body, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    // Cap the work per input so a single hostile body cannot dominate the
    // run; the parsers' own 2 MiB header cap is upstream of this harness.
    constexpr size_t MAX_BODY = 256 * core::KIB;
    if (size > MAX_BODY) size = MAX_BODY;
    exercise(data, size);
    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
// Standalone mode: deterministic sweep. Generates vint-heavy bodies (the
// shapes that matter for this parser: continuation runs, 10-byte maximal
// vints, declared lengths far beyond the buffer) plus uniformly random
// bytes, and optionally replays files named on the command line.
namespace {

void feed(const std::vector<uint8_t>& buf) {
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
}

// A pseudo-RAR5 body: plausible type/flags vints followed by noise. This is
// what a CRC-valid block would have carried, so the parsers get past their
// first few field reads and reach the extra-record loop.
std::vector<uint8_t> structured_body(std::mt19937& rng) {
    std::vector<uint8_t> buf;
    const uint8_t type = static_cast<uint8_t>(rng() % 5);
    buf.push_back(type);                              // HEAD_MAIN/FILE/SERVICE/CRYPT/ENDARC
    buf.push_back(static_cast<uint8_t>(rng() % 128)); // flags vint (1 byte, no continuation)
    if (rng() % 2) {
        // Declared extra size: sometimes huge (wraps if mishandled), sometimes tiny.
        const uint32_t decl =
            (rng() % 4 == 0) ? (0xFFFFFFFFu - (rng() % 8)) : static_cast<uint32_t>(rng() % 64);
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = static_cast<uint8_t>((decl >> shift) & 0x7F);
            if (shift + 7 < 35) b |= 0x80;
            buf.push_back(b);
        }
    }
    const size_t noise = rng() % 128;
    for (size_t i = 0; i < noise; ++i) buf.push_back(static_cast<uint8_t>(rng()));
    return buf;
}

void run_sweep() {
    std::mt19937 rng(0x5EED1234u);

    // 1. Structured bodies (reach the extra-record loop).
    for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> buf = structured_body(rng);
        feed(buf);
    }

    // 2. Maximal vints: 10 continuation bytes, then a terminator. Any parser
    //    that advances by "bytes_read on failure" or trusts the decoded value
    //    as a length is provoked here.
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> buf(static_cast<size_t>(rng() % 12) + 1, 0x80);
        buf.push_back(static_cast<uint8_t>(rng() % 0x40));
        for (size_t k = 0, n = rng() % 64; k < n; ++k) buf.push_back(static_cast<uint8_t>(rng()));
        feed(buf);
    }

    // 3. Uniformly random bodies across the size range.
    for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> buf(static_cast<size_t>(rng() % 4096));
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        feed(buf);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::FILE* f = std::fopen(argv[i], "rb");
            if (!f) {
                std::fprintf(stderr, "fuzz_header: cannot open %s\n", argv[i]);
                return 1;
            }
            std::vector<uint8_t> buf;
            uint8_t chunk[4096];
            size_t got;
            while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
                buf.insert(buf.end(), chunk, chunk + got);
            }
            std::fclose(f);
            if (!buf.empty()) feed(buf);
        }
        std::printf("fuzz_header: replayed %d file(s).\n", argc - 1);
        return 0;
    }

    std::printf("Running deterministic header-parser sweep...\n");
    run_sweep();
    std::printf("Header-parser sweep PASSED.\n");
    return 0;
}
#endif // OPENRAR_USE_LIBFUZZER
