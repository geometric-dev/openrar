// Inflate fuzzer (v1.29 M1) — the RFC 1951 decoder is decode-critical new
// code; the nightly harness exercises its block/Huffman/cap paths here.
// Crash- and hang-free is the contract: any InflateError return is fine.
#include "../../src/compress/inflate.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    // Cap output at 100x input or 8 MiB so bombs cannot stall the runner.
    const core::uint64 max_out =
        std::min<core::uint64>(static_cast<core::uint64>(size) * 100, 8ULL * 1024 * 1024);

    Inflate inf(max_out);
    const core::byte* in = reinterpret_cast<const core::byte*>(data);
    size_t pos = 0;
    auto input = [&](core::byte* buf, size_t max_size) -> size_t {
        const size_t take = size - pos < max_size ? size - pos : max_size;
        std::memcpy(buf, in + pos, take);
        pos += take;
        return take;
    };
    auto sink = [](const core::byte*, size_t) {
        return true;
    };
    (void)inf.decode(input, sink);
    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
// Standalone sweep for toolchains without libFuzzer (MSVC, gcc). Without
// this the target links as a plain executable with no entry point and fails
// with "undefined reference to main" - which is how the nightly job broke:
// this file used to expose only LLVMFuzzerTestOneInput, and unlike its
// sibling harnesses it also lacked the libFuzzer link flags in CMakeLists.
int main() {
    // Deterministic seeds, shaped like real DEFLATE streams (dynamic block,
    // stored block, truncated stream, garbage) plus a short sweep.
    const char* kSeeds[] = {
        "\x78\x9c\x4b\x4c\x4a\x06\x00\x02\x4d\x01\x27", // fixed-Huffman text
        "\x78\x01\x05\x00\xfa\xff\x11\x00\x00",         // stored block
        "\x78\x9c\x4b\x4c\x4a",                         // truncated stream
        "\x78\x9c",                                     // header only
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03",     // gzip magic, not deflate
    };
    for (const char* seed : kSeeds) {
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(seed), std::strlen(seed));
    }
    // Byte-at-a-time sweep: every prefix of the largest seed.
    const char* walk = kSeeds[0];
    const size_t walk_len = std::strlen(walk);
    for (size_t n = 1; n <= walk_len; ++n) {
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(walk), n);
    }
    return 0;
}
#endif // !OPENRAR_USE_LIBFUZZER
