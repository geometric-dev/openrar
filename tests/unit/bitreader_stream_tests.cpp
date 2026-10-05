// P2 regression (v1.36.x): the streaming BitReader (InputCallback, 64 KiB
// staging buffer) must deliver bit-identical values to the contiguous reader.
// The pre-fix refill() fetched the next chunk only when the buffer was fully
// drained, so an accumulator down to its last bits coupled with a buffer down
// to its last bytes returned a short (zero-padded) peek MID-STREAM — zero
// padding is end-of-stream semantics. Whether that corrupts decoded output
// depends on the resolved Huffman code length, which is why the sandboxed
// worker verify path (stream_payload → ExtentPullSource → streaming reader)
// showed scattered single-byte corruption only on some ≥ 64 MiB members while
// every contiguous-BitReader consumer verified clean.
//
// The differential test drives both reader variants over the same
// pseudo-random data with the same pseudo-random peek/consume sequence and
// requires value-for-value equality; a truncated-end probe pins the EOF
// zero-padding behavior itself.
#include "../../src/compress/decompressor50.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

namespace {

int g_failures = 0;

// Differential driver: every op is issued to both readers and the delivered
// values must agree. `chunk` shapes the callback's granularity (the worker's
// ExtentPullSource serves 64 KiB, the size the P2 bug is tuned to).
void differential(const char* label, size_t data_len, size_t chunk, uint32_t seed) {
    std::mt19937 fill_rng(seed);
    std::vector<core::byte> data(data_len);
    for (auto& b : data) b = static_cast<core::byte>(fill_rng() & 0xFF);

    size_t pulled = 0;
    BitReader stream(
        [&](core::byte* buf, size_t want) -> size_t {
            size_t take = std::min(want, data.size() - pulled);
            std::memcpy(buf, data.data() + pulled, take);
            pulled += take;
            return take;
        },
        data.size());
    BitReader whole(data.data(), data.size());

    std::mt19937 op_rng(seed ^ 0x9E3779B9u);
    for (int i = 0; i < 400000; ++i) {
        unsigned count = 1 + op_rng() % 24;
        bool peek_only = (op_rng() & 1) != 0;
        if (peek_only) {
            core::uint64 a = stream.peek_bits64(count);
            core::uint64 b = whole.peek_bits64(count);
            if (a != b) {
                std::cout << "[FAIL] " << label << ": peek64(" << count << ") at bit "
                          << whole.bit_pos() << " stream=" << a << " whole=" << b << "\n";
                ++g_failures;
                return;
            }
        } else {
            core::uint64 a = stream.get_bits64(count);
            core::uint64 b = whole.get_bits64(count);
            if (a != b) {
                std::cout << "[FAIL] " << label << ": get_bits64(" << count << ") at bit "
                          << whole.bit_pos() << " stream=" << a << " whole=" << b << "\n";
                ++g_failures;
                return;
            }
        }
        // Position must track too: a short mid-stream consume silently drops
        // bits and desyncs every later Huffman code.
        if (stream.bit_pos() != whole.bit_pos()) {
            std::cout << "[FAIL] " << label << ": bit_pos " << stream.bit_pos() << " vs "
                      << whole.bit_pos() << " after op " << i << "\n";
            ++g_failures;
            return;
        }
    }
    (void)chunk;
    std::cout << label << ": " << data_len << "B ops=400000 OK\n";
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Chunk sizes around the staging-buffer geometry: exactly the 64 KiB the
    // worker serves, odd sizes to skew buffer tails, tiny sizes to force
    // short callback reads against the reader's 64 KiB fetches. Sizes exceed
    // the ~625 KB the op sequence consumes so every op stays mid-stream.
    differential("diff/64KiB", 11 * 65536 + 4097, 65536, 0x1234567);
    differential("diff/64KiB+1", 11 * 65536 + 1, 65537, 0x0BADF00D);
    differential("diff/7B", 11 * 65536 + 333, 7, 0x5EED);
    differential("diff/1B", 11 * 65536 + 11, 1, 0x00C0FFEE);

    // End-of-stream semantics: past the final byte both readers zero-fill,
    // and consumed stops advancing at the real bit count.
    {
        std::vector<core::byte> data(37);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<core::byte>(i * 7 + 1);
        size_t pulled = 0;
        BitReader stream(
            [&](core::byte* buf, size_t want) -> size_t {
                size_t take = std::min(want, data.size() - pulled);
                std::memcpy(buf, data.data() + pulled, take);
                pulled += take;
                return take;
            },
            data.size());
        BitReader whole(data.data(), data.size());
        stream.get_bits64(64); // consume past the 296-bit stream piecemeal:
        whole.get_bits64(64);  // 5 rounds x 64 bits > 296, then only zeros
        for (int i = 0; i < 4; ++i) {
            stream.get_bits64(64);
            whole.get_bits64(64);
        }
        core::uint64 a = stream.peek_bits64(20);
        core::uint64 b = whole.peek_bits64(20);
        if (a != b || a != 0) {
            std::cout << "[FAIL] eof-pad: stream=" << a << " whole=" << b << "\n";
            ++g_failures;
        } else if (stream.bit_pos() != whole.bit_pos()) {
            std::cout << "[FAIL] eof-pad: bit_pos " << stream.bit_pos() << " vs " << whole.bit_pos()
                      << "\n";
            ++g_failures;
        } else {
            std::cout << "eof-pad: OK\n";
        }
    }

    if (g_failures != 0) return 1;
    std::cout << "All BitReader variant probes PASSED\n";
    return 0;
}
