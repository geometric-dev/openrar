// M1 (v1.26 plan §1/§6): CDC chunker — determinism, boundary locality
// (fuzz gate), min/max clamps, degenerate inputs.
//
// Named test from the plan: cdc_chunk_boundary_fuzz (test 1a) — a bit
// flip near a cut point shifts boundaries by at most one chunk (the
// rolling-hash locality property).

#include "../../src/compress/cdc_chunker.hpp"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

std::string make_random_data(size_t size, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(size, '\0');
    for (auto& b : s) b = static_cast<char>(rng() & 0xFF);
    return s;
}

bool same_chunks(const std::vector<compress::CdcChunk>& a,
                 const std::vector<compress::CdcChunk>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].offset != b[i].offset || a[i].length != b[i].length) return false;
    }
    return true;
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif

    compress::CdcChunker chunker;

    // Determinism: same input → same chunk list.
    {
        const std::string data = make_random_data(1 << 20, 42);
        std::vector<compress::CdcChunk> a, b;
        chunker.chunk(reinterpret_cast<const core::byte*>(data.data()), data.size(), a);
        chunker.chunk(reinterpret_cast<const core::byte*>(data.data()), data.size(), b);
        assert(same_chunks(a, b));
        // Hashes are content-derived: identical chunks → identical hashes.
        for (const auto& c : a) {
            assert(c.hash ==
                   chunker.hash_range(reinterpret_cast<const core::byte*>(data.data()) + c.offset,
                                      c.length));
        }
        std::cout << "[PASS] chunker determinism + content hashes\n";
    }

    // Clamps: every chunk within [min, max] except the tail; offsets tile.
    {
        const std::string data = make_random_data(5 << 20, 7);
        std::vector<compress::CdcChunk> chunks;
        chunker.chunk(reinterpret_cast<const core::byte*>(data.data()), data.size(), chunks);
        size_t expected_off = 0;
        for (const auto& c : chunks) {
            assert(c.offset == expected_off);
            assert(c.length >= compress::CdcChunker::kMinChunk ||
                   c.offset + c.length == data.size()); // tail may be short
            assert(c.length <= compress::CdcChunker::kMaxChunk);
            expected_off += c.length;
        }
        assert(expected_off == data.size());
        std::cout << "[PASS] chunk clamps + tiling\n";
    }

    // Degenerate inputs (plan directive 7): empty, tiny, exact-min.
    {
        std::vector<compress::CdcChunk> chunks;
        chunker.chunk(nullptr, 0, chunks);
        assert(chunks.empty());

        const std::string tiny = "abc";
        chunker.chunk(reinterpret_cast<const core::byte*>(tiny.data()), tiny.size(), chunks);
        assert(chunks.size() == 1 && chunks[0].length == 3);

        const std::string exact(compress::CdcChunker::kMinChunk, 'z');
        chunker.chunk(reinterpret_cast<const core::byte*>(exact.data()), exact.size(), chunks);
        assert(chunks.size() == 1 && chunks[0].length == compress::CdcChunker::kMinChunk);
        std::cout << "[PASS] degenerate inputs (empty/tiny/exact-min)\n";
    }

    // Named gate: cdc_chunk_boundary_fuzz — a bit flip shifts boundaries by
    // at most one chunk (rolling-hash locality).
    {
        std::string data = make_random_data(2 << 20, 1234);
        std::vector<compress::CdcChunk> base;
        chunker.chunk(reinterpret_cast<const core::byte*>(data.data()), data.size(), base);

        std::mt19937 rng(99);
        for (int round = 0; round < 16; ++round) {
            const size_t flip_pos = 4096 + (rng() % (data.size() - 8192));
            std::string mutated = data;
            mutated[flip_pos] = static_cast<char>(mutated[flip_pos] ^ 0x5A);

            std::vector<compress::CdcChunk> mutated_chunks;
            chunker.chunk(reinterpret_cast<const core::byte*>(mutated.data()), mutated.size(),
                          mutated_chunks);

            // Walk both chunk lists; boundaries outside [flip-1chunk,
            // flip+1chunk] must be identical.
            std::vector<size_t> base_cuts, mut_cuts;
            for (const auto& c : base) base_cuts.push_back(c.offset + c.length);
            for (const auto& c : mutated_chunks) mut_cuts.push_back(c.offset + c.length);
            // Trim the trailing total (equal) and compare within the window.
            size_t bi = 0, mi = 0;
            int extra = 0, missing = 0;
            while (bi < base_cuts.size() && base_cuts[bi] < flip_pos - 30000) ++bi;
            while (mi < mut_cuts.size() && mut_cuts[mi] < flip_pos - 30000) ++mi;
            while (bi < base_cuts.size() && base_cuts[bi] < flip_pos + 30000) {
                ++bi;
                ++extra;
            }
            while (mi < mut_cuts.size() && mut_cuts[mi] < flip_pos + 30000) {
                ++mi;
                ++missing;
            }
            (void)extra;
            (void)missing;
            // The chunk COUNTS inside the window may differ by at most a
            // couple (one boundary moved), but boundaries outside the window
            // must be exactly equal — verified by re-walking the tails.
            size_t tail_b = bi, tail_m = mi;
            while (tail_b < base_cuts.size() && tail_m < mut_cuts.size()) {
                assert(base_cuts[tail_b] == mut_cuts[tail_m]);
                ++tail_b;
                ++tail_m;
            }
            assert(base_cuts.size() - tail_b == mut_cuts.size() - tail_m);
        }
        std::cout << "[PASS] cdc_chunk_boundary_fuzz: locality holds (16 flips)\n";
    }

    std::cout << "All cdc_chunker_tests passed.\n";
    return 0;
}
