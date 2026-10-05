// v1.37.0 — intra-entry parallel decode driver tests.
// The core gate: MT (scout + range workers) output is byte-identical to the
// sequential decoder's, on every input, every thread count. Plan:
// docs/v1.37.0-implementation-plan.md (negative tests named there).
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/parallel_decode.hpp"

#include <cassert>
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

std::vector<core::byte> pseudo_text(size_t n, uint32_t seed) {
    // Word-sampling text: compressible enough to hit G3 at modest sizes.
    std::mt19937 rng(seed);
    std::vector<std::string> vocab;
    for (int i = 0; i < 600; ++i) vocab.push_back("w" + std::to_string(i));
    for (const auto& w : std::vector<std::string>{"the", "quick", "brown", "window", "archive",
                                                  "decode", "parallel", "range"}) {
        vocab.push_back(w);
    }
    std::vector<core::byte> out;
    out.reserve(n + 64);
    std::string line;
    while (out.size() < n) {
        line += vocab[rng() % vocab.size()];
        if (line.size() > 72) {
            line += '\n';
            out.insert(out.end(), line.begin(), line.end());
            line.clear();
        } else {
            line += ' ';
        }
    }
    out.resize(n);
    return out;
}

bool roundtrip_both(const std::vector<core::byte>& data, size_t win_size, unsigned threads,
                    std::vector<core::byte>& out_par, std::string& err) {
    std::vector<core::byte> packed;
    if (!Compressor50::compress_buffer(data.data(), data.size(), packed, 3, win_size)) {
        err = "compress_buffer failed";
        return false;
    }
    // Sequential reference.
    std::vector<core::byte> out_seq;
    Decompressor50 seq(win_size);
    if (!seq.decompress_to_vector(packed.data(), packed.size(), out_seq)) {
        err = "sequential decode failed";
        return false;
    }
    if (out_seq != data) {
        err = "sequential roundtrip mismatch (pre-existing)";
        return false;
    }
    // Parallel path (gates forced via the threads override).
    std::string forced = "OPENRAR_PARALLEL_DECODE_THREADS=" + std::to_string(threads);
    _putenv(forced.c_str());
    ParallelDecodeDiag diag;
    out_par.clear();
    auto cb = [&](const core::byte* p, size_t n) -> bool {
        out_par.insert(out_par.end(), p, p + n);
        return true;
    };
    if (!decode_entry(packed.data(), packed.size(), data.size(), win_size, cb, &diag)) {
        err = "decode_entry failed (diag engaged=" + std::to_string(diag.engaged) +
              " ranges=" + std::to_string(diag.ranges) + ")";
        return false;
    }
    if (!diag.engaged) {
        err = "driver did not engage (test requires engagement)";
        return false;
    }
    return true;
}

void test_identity_threads() {
    const size_t sizes[] = {24u * 1024 * 1024}; // pack ≈ 7-8 MB: G3 at 2 workers
    for (size_t n : sizes) {
        auto data = pseudo_text(n, 0x137);
        for (unsigned threads : {2u, 3u, 4u, 8u}) {
            std::vector<core::byte> out_par;
            std::string err;
            if (!roundtrip_both(data, 0x200000, threads, out_par, err)) {
                std::cout << "[FAIL] identity threads=" << threads << ": " << err << "\n";
                std::exit(1);
            }
            if (out_par != data) {
                size_t d = 0;
                while (d < std::min(out_par.size(), data.size()) && out_par[d] == data[d]) ++d;
                std::cout << "[FAIL] identity threads=" << threads << ": first divergence at " << d
                          << " (" << (d > 16 ? d - 16 : 0) << "..)\n";
                std::exit(1);
            }
            std::cout << "[PASS] mt/st byte-identity threads=" << threads << " (" << n << " B)\n";
        }
    }
}

void test_gate_matrix() {
    // G6-equivalent on native: solid entries never engage (checked here via
    // the decision function's contract; the WASM leg compiles it out).
    // Kill switch.
    _putenv("OPENRAR_NO_PARALLEL_DECODE=1");
    assert(!should_use_parallel_decode(100 * 1024 * 1024, 64 * 1024 * 1024, false));
    _putenv("OPENRAR_NO_PARALLEL_DECODE=");
    // Solid entry.
    assert(!should_use_parallel_decode(100 * 1024 * 1024, 64 * 1024 * 1024, true));
    // Budget: pack + dest > 1 GiB.
    assert(!should_use_parallel_decode(600 * 1024 * 1024, 600 * 1024 * 1024, false));
    // Amortization: pack below 4 MiB per worker (2 workers -> 8 MiB).
    _putenv("OPENRAR_PARALLEL_DECODE_THREADS=2");
    assert(!should_use_parallel_decode(7 * 1024 * 1024, 64 * 1024 * 1024, false));
    assert(should_use_parallel_decode(9 * 1024 * 1024, 64 * 1024 * 1024, false));
    _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");
    std::cout << "[PASS] gate matrix\n";
}

} // namespace

void test_block_by_block_state() {
    // The pinpoint probe: for EVERY block boundary, run decode_range for that
    // single block starting from the scout's checkpoint; the output must
    // match the sequential decode's corresponding span. A mismatch means the
    // scout's checkpoint state (rep distances, tables, position) is wrong.
    auto data = pseudo_text(24u * 1024 * 1024, 0x137);
    std::vector<core::byte> packed;
    if (!Compressor50::compress_buffer(data.data(), data.size(), packed, 3, 0x200000)) {
        std::cout << "[FAIL] probe: compress\n";
        std::exit(1);
    }
    Decompressor50 seq(0x200000);
    std::vector<core::byte> out_seq;
    if (!seq.decompress_to_vector(packed.data(), packed.size(), out_seq)) {
        std::cout << "[FAIL] probe: sequential\n";
        std::exit(1);
    }
    Decompressor50 scout(0x200000);
    Decompressor50::ScoutRecord rec;
    if (!scout.scout_member(packed.data(), packed.size(), data.size(), rec, 0)) {
        std::cout << "[FAIL] probe: scout\n";
        std::exit(1);
    }
    // A null sync: single-block ranges never wait (no cross-range sources
    // within one block... except back-references BEFORE the block start,
    // which resolve inside the shared buffer — provided by out_seq's bytes).
    struct NullSync final : Decompressor50::RangeSync {
        bool cancelled() const override { return false; }
    } sync;
    std::vector<core::byte> buf(out_seq.size(), 0);
    // Redundant-overlap probe: each block decodes from its overlap checkpoint
    // (last checkpoint at or before block_out - dict), writing only its own
    // span — exactly the driver's per-block mechanism.
    for (size_t bi = 0; bi + 1 < rec.checkpoints.size(); ++bi) {
        const auto& cp = rec.checkpoints[bi];
        size_t span = rec.checkpoints[bi + 1].output_pos - cp.output_pos;
        // Overlap checkpoint: last checkpoint with output_pos <= block_out - win.
        size_t floor_pos = cp.output_pos >= 0x200000 ? cp.output_pos - 0x200000 : 0;
        size_t ovl = 0;
        for (size_t c = rec.checkpoints.size(); c-- > 0;) {
            if (rec.checkpoints[c].output_pos <= floor_pos) {
                ovl = c;
                break;
            }
        }
        Decompressor50 w(0x200000);
        size_t written = 0;
        if (!w.decode_range(rec, packed.data(), packed.size(),
                            static_cast<uint32_t>(rec.checkpoints[ovl].next_block), bi,
                            rec.checkpoints[ovl], buf.data(), data.size(), cp.output_pos, &sync,
                            &written)) {
            std::cout << "[FAIL] probe: block " << bi
                      << " decode_range failed: " << w.last_error_message() << "\n";
            std::exit(1);
        }
        size_t expect_written =
            span + (cp.output_pos - rec.checkpoints[ovl].output_pos); // overlap + span
        if (written != expect_written) {
            std::cout << "[FAIL] probe: block " << bi << " wrote " << written << " expect "
                      << expect_written << "\n";
            std::exit(1);
        }
        if (std::memcmp(buf.data() + cp.output_pos, out_seq.data() + cp.output_pos, span) != 0) {
            size_t d = 0;
            while (cp.output_pos + d < out_seq.size() &&
                   buf[cp.output_pos + d] == out_seq[cp.output_pos + d]) {
                ++d;
            }
            std::cout << "[FAIL] probe: block " << bi << " (out " << cp.output_pos << ") "
                      << "diverges at " << cp.output_pos + d << "\n";
            for (size_t k = (d > 8 ? d - 8 : 0); k < d + 8 && k < out_seq.size(); ++k) {
                char mark = k == d ? '*' : ' ';
                std::cout << "  " << mark << " seq=" << std::hex << (int)out_seq[k]
                          << " got=" << (int)buf[k] << std::dec << " (abs " << k << ")\n";
            }
            std::exit(1);
        }
    }
    std::cout << "[PASS] block-by-block scout-state equivalence (" << rec.blocks.size()
              << " blocks)\n";
}

int main() {
    test_gate_matrix();
    if (const char* mode = std::getenv("PD_PROBE")) {
        if (mode[0] == '1') {
            test_block_by_block_state();
            return 0;
        }
    }
    test_identity_threads();
    std::cout << "All parallel_decode_tests PASSED\n";
    return 0;
}
