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

void fuzz_one() {
    auto data = generate_mutated_input();
    std::vector<core::byte> compressed;
    bool comp_ok = Compressor50::compress_buffer(data.data(), data.size(), compressed);
    assert(comp_ok);

    std::vector<core::byte> decompressed;
    Decompressor50 dec;
#ifdef OPENRAR_CROSS_VALIDATE
    dec.set_validation_source(data.data(), data.size());
#endif
    bool dec_ok = dec.decompress_to_vector(compressed.data(), compressed.size(), decompressed);

    assert(dec_ok);
    assert(decompressed.size() == data.size());
    assert(std::memcmp(decompressed.data(), data.data(), data.size()) == 0);

    // Overhead bound
    assert(compressed.size() <= data.size() + 256 + data.size() / 16);
}

int main() {
    std::cout << "Running deterministic roundtrip fuzzer sweep...\n";
    const int num_iterations = 2000;
    for (int i = 0; i < num_iterations; ++i) {
        fuzz_one();
        if ((i + 1) % 100 == 0) {
            std::cout << "  Completed " << (i + 1) << " iterations...\n" << std::flush;
        }
    }
    std::cout << "Roundtrip fuzzer sweep PASSED.\n";
    return 0;
}
