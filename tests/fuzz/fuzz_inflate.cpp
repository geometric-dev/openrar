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
