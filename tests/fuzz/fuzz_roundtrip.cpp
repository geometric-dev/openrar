#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/core/types.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include <random>

using namespace openrar;
using namespace openrar::compress;

// Deterministic random generator
std::mt19937 rng(42);

std::vector<core::byte> generate_mutated_input() {
    std::vector<core::byte> data;
    int type = rng() % 5;
    size_t size = (rng() % 100000) + 1; // Up to 100KB
    data.resize(size);

    if (type == 0) {
        // Uniform random (incompressible)
        for (size_t i = 0; i < size; ++i) data[i] = static_cast<core::byte>(rng());
    } else if (type == 1) {
        // All zeros (sparse tables, huge RLE)
        std::memset(data.data(), 0, size);
    } else if (type == 2) {
        // Repetitive patterns
        size_t period = (rng() % 256) + 1;
        std::vector<core::byte> pattern(period);
        for (size_t i = 0; i < period; ++i) pattern[i] = static_cast<core::byte>(rng());
        for (size_t i = 0; i < size; ++i) data[i] = pattern[i % period];
    } else if (type == 3) {
        // Small alphabet
        core::byte a = static_cast<core::byte>(rng());
        core::byte b = static_cast<core::byte>(rng());
        for (size_t i = 0; i < size; ++i) data[i] = (rng() % 2 == 0) ? a : b;
    } else if (type == 4) {
        // Interleaved literal/match (targets run>=3 bounds)
        size_t i = 0;
        while (i < size) {
            // Run of identical bytes (e.g. zeros)
            size_t run = (rng() % 5) + 1;
            core::byte val = static_cast<core::byte>(rng() % 2 == 0 ? 0 : rng());
            for (size_t j = 0; j < run && i < size; ++j, ++i) {
                data[i] = val;
            }
            // Random bytes
            size_t rand_len = (rng() % 8) + 1;
            for (size_t j = 0; j < rand_len && i < size; ++j, ++i) {
                data[i] = static_cast<core::byte>(rng());
            }
        }
    }
    return data;
}

void fuzz_one(int iteration) {
    auto data = generate_mutated_input();
    std::vector<core::byte> compressed;
    bool comp_ok = Compressor50::compress_buffer(data.data(), data.size(), compressed);
    assert(comp_ok);

    std::vector<core::byte> decompressed;
    // Raw streams carry no dictionary-size header: the decoder window is
    // caller knowledge. compress_buffer() defaults to win_size 0x200000 (the
    // pow2 clamp only ever shrinks it), so the matching oracle window is
    // 0x200000. A default-constructed Decompressor50 runs a 1 MiB window,
    // which cannot decode default-window streams once match distances or
    // filter regions exceed 1 MiB (see docs/wasm-archive-spec.md interop
    // note; the archive/dll layers pass the recorded window explicitly).
    Decompressor50 dec(0x200000);
#ifdef OPENRAR_CROSS_VALIDATE
    dec.set_validation_source(data.data(), data.size());
#endif
    bool dec_ok = dec.decompress_to_vector(compressed.data(), compressed.size(), decompressed);

    if (!dec_ok || decompressed.size() != data.size() ||
        std::memcmp(decompressed.data(), data.data(), data.size()) != 0) {
        // Dump the failing case so the divergence is reproducible offline.
        size_t first_diff = data.size();
        if (dec_ok && decompressed.size() == data.size()) {
            for (size_t i = 0; i < data.size(); ++i) {
                if (decompressed[i] != data[i]) {
                    first_diff = i;
                    break;
                }
            }
        }
        std::fprintf(stderr, "ROUNDTRIP DIVERGENCE at iteration %d (size %zu, first_diff %zu)\n",
                     iteration, data.size(), first_diff);
        std::FILE* f = std::fopen("fuzz_divergence_input.bin", "wb");
        if (f) {
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
        }
        f = std::fopen("fuzz_divergence_compressed.bin", "wb");
        if (f) {
            std::fwrite(compressed.data(), 1, compressed.size(), f);
            std::fclose(f);
        }
        std::fflush(stderr);
        std::abort();
    }

    // Overhead bound
    assert(compressed.size() <= data.size() + 256 + data.size() / 16);
}

int main() {
    std::cout << "Running deterministic roundtrip fuzzer sweep...\n";
    const int num_iterations = 2000;
    for (int i = 0; i < num_iterations; ++i) {
        fuzz_one(i);
        if ((i + 1) % 100 == 0) {
            std::cout << "  Completed " << (i + 1) << " iterations...\n" << std::flush;
        }
    }
    std::cout << "Roundtrip fuzzer sweep PASSED.\n";
    return 0;
}
