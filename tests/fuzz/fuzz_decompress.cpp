#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <random>

using namespace openrar;
using namespace openrar::compress;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    Decompressor50 dec;
    // Timeout/hang protection: cap output size to 100x input size or 10MB
    size_t max_output = std::min<size_t>(size * 100, 10 * 1024 * 1024);

    // We don't care if it succeeds or fails, just that it doesn't crash or hang.
    // Decompress into the internal window without a flush callback.
    dec.decompress(data, size, max_output);

    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
// Deterministic random generator for standalone mode
std::mt19937 rng_dec(43);

void mutate_stream(std::vector<core::byte>& stream) {
    if (stream.empty()) return;

    int num_mutations = (rng_dec() % 10) + 1;
    for (int i = 0; i < num_mutations; ++i) {
        size_t idx = rng_dec() % stream.size();
        int type = rng_dec() % 4;

        if (type == 0) {
            // Bit flip
            stream[idx] ^= (1 << (rng_dec() % 8));
        } else if (type == 1) {
            // Byte overwrite
            stream[idx] = static_cast<core::byte>(rng_dec());
        } else if (type == 2) {
            // Insert random byte
            stream.insert(stream.begin() + idx, static_cast<core::byte>(rng_dec()));
        } else if (type == 3) {
            // Drop byte
            stream.erase(stream.begin() + idx);
        }
    }
}

int main() {
    std::cout << "Running deterministic decoder fuzzer sweep...\n";

    // Generate some valid streams first
    std::vector<std::vector<core::byte>> valid_streams;
    for (int i = 0; i < 100; ++i) {
        std::vector<core::byte> src(1000 + (rng_dec() % 5000));
        for (size_t j = 0; j < src.size(); ++j) src[j] = static_cast<core::byte>(rng_dec());
        std::vector<core::byte> comp;
        Compressor50::compress_buffer(src.data(), src.size(), comp);
        valid_streams.push_back(comp);
    }

    const int num_iterations = 2000;
    for (int i = 0; i < num_iterations; ++i) {
        std::vector<core::byte> stream = valid_streams[rng_dec() % valid_streams.size()];

        // Mutate
        if (rng_dec() % 10 != 0) {
            mutate_stream(stream);
        }

        // Truncate sometimes
        if (rng_dec() % 5 == 0 && !stream.empty()) {
            stream.resize((rng_dec() % stream.size()) + 1);
        }

        LLVMFuzzerTestOneInput(stream.data(), stream.size());

        if ((i + 1) % 100 == 0) {
            std::cout << "  Completed " << (i + 1) << " iterations...\n" << std::flush;
        }
    }
    std::cout << "Decoder fuzzer sweep PASSED.\n";
    return 0;
}
#endif
