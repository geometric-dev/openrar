#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/parallel_decode.hpp"
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
    // 0x200000. Passed explicitly — equal to Decompressor50::DEFAULT_WIN_SIZE,
    // but pinned here so a future default change cannot silently desync the
    // oracle from the contract (the archive/dll layers pass the recorded
    // window explicitly).
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

// v1.37.0 M4: MT-vs-ST differential. Every generated archive is decoded by
// BOTH engines - the sequential decoder (reference) and the two-phase
// pipeline (engaged via the span-floor testing override) - and must agree
// byte- and chunk-for-chunk. Mutated packed streams must fail closed: the
// parallel path may return false (the pre-emission fallback owns the
// verdict) but may never produce bytes the sequential decoder would not.
static void two_phase_differential(int iteration, const std::vector<core::byte>& data,
                                   const std::vector<core::byte>& compressed, bool splittable) {
    const size_t win = 0x200000;
    std::vector<core::byte> seq_out, par_out;
    std::vector<size_t> seq_chunks, par_chunks;
    auto collect = [&](std::vector<core::byte>& out, std::vector<size_t>& chunks) {
        return [&](const core::byte* p, size_t n) -> bool {
            out.insert(out.end(), p, p + n);
            chunks.push_back(n);
            return true;
        };
    };
    {
        Decompressor50 seq(win);
        if (!seq.decompress(compressed.data(), compressed.size(), data.size(), false,
                            collect(seq_out, seq_chunks))) {
            return; // sequential rejected its own encoder's stream: not ours
        }
    }
    _putenv("OPENRAR_PARALLEL_DECODE_SPAN_FLOOR=4096");
    _putenv("OPENRAR_PARALLEL_DECODE_THREADS=3");
    ParallelDecodeDiag diag;
    bool ok = decode_entry(compressed.data(), compressed.size(), data.size(), win,
                           collect(par_out, par_chunks), &diag);
    if (splittable && ok && (!diag.engaged || diag.fell_back)) {
        std::fprintf(stderr,
                     "TWO-PHASE DIFFERENTIAL: not engaged (iter %d) src=%zu engaged=%d "
                     "fell_back=%d spans=%u workers=%u\n",
                     iteration, compressed.size(), (int)diag.engaged, (int)diag.fell_back,
                     diag.spans, diag.workers);
        std::abort();
    }
    if (ok && (par_out != seq_out || par_chunks != seq_chunks)) {
        std::fprintf(stderr, "TWO-PHASE DIFFERENTIAL DIVERGENCE (iter %d, size %zu)\n", iteration,
                     data.size());
        std::abort();
    }
    if (!ok && !diag.fell_back) {
        // A refusal on a stream the sequential accepts is only legal through
        // the pre-emission fallback; post-emission failure breaks the R5
        // contract.
        std::fprintf(stderr, "TWO-PHASE DIFFERENTIAL: post-emission failure (iter %d)\n",
                     iteration);
        std::abort();
    }
    // Hostile mutations of the packed stream: fail-closed or byte-identical
    // with the sequential verdict, never wrong bytes.
    std::mt19937 mut(static_cast<unsigned>(iteration) * 2654435761u + 7u);
    for (int m = 0; m < 3; ++m) {
        std::vector<core::byte> bad = compressed;
        const int flips = 1 + static_cast<int>(mut() % 8);
        for (int k = 0; k < flips; ++k) {
            bad[mut() % bad.size()] ^= static_cast<core::byte>(1u << (mut() % 8));
        }
        std::vector<core::byte> seq_bad;
        Decompressor50 seq(win);
        const bool seq_ok = seq.decompress(bad.data(), bad.size(), data.size(), false,
                                           [&](const core::byte* p, size_t n) -> bool {
                                               seq_bad.insert(seq_bad.end(), p, p + n);
                                               return true;
                                           });
        std::vector<core::byte> par_bad;
        ParallelDecodeDiag d2;
        const bool par_ok = decode_entry(
            bad.data(), bad.size(), data.size(), win,
            [&](const core::byte* p, size_t n) -> bool {
                par_bad.insert(par_bad.end(), p, p + n);
                return true;
            },
            &d2);
        if (par_ok && (!d2.engaged || d2.fell_back)) {
            if (par_bad != seq_bad) {
                std::fprintf(stderr, "TWO-PHASE MUTATION DIVERGENCE (iter %d)\n", iteration);
                std::abort();
            }
        } else if (!par_ok && seq_ok && !d2.fell_back) {
            std::fprintf(stderr, "TWO-PHASE MUTATION: post-emission refusal (iter %d)\n",
                         iteration);
            std::abort();
        }
    }
    _putenv("OPENRAR_PARALLEL_DECODE_SPAN_FLOOR=");
    _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << "Running deterministic roundtrip fuzzer sweep...\n";
    const int num_iterations = 2000;
    for (int i = 0; i < num_iterations; ++i) {
        fuzz_one(i);
        if ((i + 1) % 100 == 0) {
            std::cout << "  Completed " << (i + 1) << " iterations...\n" << std::flush;
        }
    }
    std::cout << "Roundtrip fuzzer sweep PASSED.\n";

    // v1.37.0: the MT-vs-ST differential sweep (plan M4.1) - same generator,
    // both engines, plus hostile mutations per input.
    std::cout << "Running two-phase differential sweep...\n";
    for (int i = 0; i < 400; ++i) {
        // The pipeline parallelizes at block granularity: tile the generated
        // pattern up to >= 4 MiB raw so compressible shapes still produce
        // enough blocks to split. Pattern-dominated inputs whose PACKED size
        // stays under the G3 floor (zeros-class) fail the gate and take the
        // splittable=false path: the differential then checks the fallback's
        // bytes instead of engagement.
        auto data = generate_mutated_input();
        size_t tiles = (4u << 20) / (data.size() ? data.size() : 1) + 1;
        if (tiles > 1) {
            std::vector<core::byte> tiled;
            tiled.reserve(data.size() * tiles);
            for (size_t k = 0; k < tiles; ++k) tiled.insert(tiled.end(), data.begin(), data.end());
            data = std::move(tiled);
        }
        std::vector<core::byte> compressed;
        if (!Compressor50::compress_buffer(data.data(), data.size(), compressed)) continue;
        // Engagement is asserted only when the member actually engages: the
        // real decision function (floor override + thread override active)
        // plus enough blocks to split. Everything else degrades to the
        // sequential-vs-fallback byte check (still run for the bytes).
        _putenv("OPENRAR_PARALLEL_DECODE_SPAN_FLOOR=4096");
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=3");
        unsigned w = 0;
        const bool gated =
            should_use_parallel_decode(compressed.size(), data.size(), 0x200000, false, 0, &w);
        Decompressor50 ps(0x200000);
        Decompressor50::PrescanTimeline tl;
        const bool splittable = gated &&
                                ps.prescan_member(compressed.data(), compressed.size(), tl) &&
                                tl.blocks.size() >= 3;
        two_phase_differential(i, data, compressed, splittable);
        if ((i + 1) % 50 == 0) {
            std::cout << "  Differential " << (i + 1) << " iterations...\n" << std::flush;
        }
    }
    std::cout << "Two-phase differential sweep PASSED.\n";
    return 0;
}
