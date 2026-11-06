// v2.2 research probe (not a test): per-region parser-selection ceiling.
//
// Question (docs/milestones/v2.1.0-implementation-plan.md §10 item 10): how much smaller
// would the output be if each region used the better of the greedy and the forward-arrival
// parser? The answer is the upper bound for per-block parser selection, and it sizes any G4
// target before one is written down.
//
// Method: compress the file twice through the shipping single-buffer path, once per parser,
// with the block probe on and blocks forced to a fixed input size (default 256 KiB), so both
// runs cut the same blocks to within one match length and block k of one run is directly
// comparable to block k of the other. (The shipped cut, 32768 tokens or 512 KiB, differs
// between the parsers, and re-binning unequal blocks onto a grid makes min() pick up smearing
// noise as if it were signal. An earlier version did that and over-stated the ceiling.)
// The ceiling for a decision unit of U blocks is sum over units of min(greedy, forward).
//
// What this estimate leaves out (so read it as an upper bound, not a forecast):
//   * Switching parsers between blocks perturbs hash-chain, rep-distance and Huffman state for
//     the blocks that follow, which neither pure run has to pay for.
//   * A real selector needs a signal that is available before the block is parsed, and a
//     wrong pick costs the difference, not zero.
//   * Forcing a fixed block size shifts both absolute sizes slightly from the CLI's; the
//     comparison between the parsers is what is being measured.
//
// Usage: blockceiling <file> [method=5] [win=0x800000] [--block=KiB] [--csv]
// Log-only: it reads the file and writes nothing.

#include "../src/compress/compressor50.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

// The only door into the encoder's probe state. The controls are private on Compressor50 and
// this class is its friend, so production code cannot reach them. Setting a fixed block size
// changes the emitted stream (it also lifts the token cap): this tool never writes an archive.
namespace openrar::compress {
class Compressor50ResearchAccess {
public:
    static void enable_block_probe(Compressor50& c, uint64_t fixed_block_input) {
        c.block_probe_on_ = true;
        c.block_probe_.clear();
        if (fixed_block_input != 0) {
            c.flush_input_limit_ = fixed_block_input;
            c.flush_token_limit_ = static_cast<size_t>(-1);
        }
    }
    static const std::vector<Compressor50::BlockProbeRecord>& records(const Compressor50& c) {
        return c.block_probe_;
    }
};
} // namespace openrar::compress

namespace {

struct Run {
    std::vector<Compressor50::BlockProbeRecord> blocks;
    uint64_t packed_total = 0;
    double seconds = 0.0;
    bool ok = false;
};

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value && value[0]) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

Run run_parser(const std::vector<core::byte>& data, int method, size_t win, bool forward,
               uint64_t block_input) {
    // The two switches are read in init_match_params(); set exactly one, clear the other.
    set_env("OPENRAR_NO_FORWARD_ARRIVAL", forward ? "" : "1");
    set_env("OPENRAR_ENABLE_FORWARD_ARRIVAL", forward ? "1" : "");

    Run r;
    Compressor50 packer;
    std::vector<core::byte> packed;
    packer.begin_archive(nullptr, method, win);
    packer.set_external_buffer(data.data(), data.size());
    packer.set_memory_dest(&packed);
    Compressor50ResearchAccess::enable_block_probe(packer, block_input);
    const auto t0 = std::chrono::steady_clock::now();
    const auto rc = packer.compress();
    const auto t1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.ok = rc >= 0 && !packed.empty();
    r.packed_total = packed.size();
    r.blocks = Compressor50ResearchAccess::records(packer);
    return r;
}

// Sum consecutive blocks into decision units of `unit` blocks.
std::vector<double> units(const Run& r, size_t n, size_t unit) {
    std::vector<double> out;
    for (size_t i = 0; i < n; i += unit) {
        double s = 0;
        for (size_t j = i; j < std::min(n, i + unit); ++j) {
            s += static_cast<double>(r.blocks[j].packed_bytes);
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: blockceiling <file> [method=5] [win=0x800000] [--block=KiB] [--csv]\n");
        return 2;
    }
    int method = 5;
    size_t win = 0x800000;
    uint64_t block_kib = 256;
    bool csv = false;
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--csv") {
            csv = true;
        } else if (a.rfind("--block=", 0) == 0) {
            block_kib = std::strtoull(a.c_str() + 8, nullptr, 0);
        } else if (positional == 0) {
            method = static_cast<int>(std::strtol(argv[i], nullptr, 0));
            ++positional;
        } else {
            win = static_cast<size_t>(std::strtoull(argv[i], nullptr, 0));
        }
    }
    if (block_kib == 0) {
        std::printf("--block must be > 0\n");
        return 2;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<core::byte> data((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

    const uint64_t block_input = block_kib << 10;
    const Run g = run_parser(data, method, win, false, block_input);
    const Run fa = run_parser(data, method, win, true, block_input);
    if (!g.ok || !fa.ok) {
        std::printf("COMPRESS FAILED (greedy ok=%d, forward ok=%d)\n", g.ok, fa.ok);
        return 1;
    }
    if (g.blocks.empty() || fa.blocks.empty()) {
        std::printf("no block records (vacuous run)\n");
        return 1;
    }

    // Both runs must cut (nearly) the same blocks, or the comparison is not like for like.
    const size_t n = std::min(g.blocks.size(), fa.blocks.size());
    uint64_t max_drift = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t a = g.blocks[i].input_end, b = fa.blocks[i].input_end;
        max_drift = std::max(max_drift, a > b ? a - b : b - a);
    }
    const bool aligned = g.blocks.size() == fa.blocks.size() && max_drift <= 4096;

    const double mib = static_cast<double>(data.size()) / (1024.0 * 1024.0);
    std::printf("file=%s bytes=%zu method=-m%d win=0x%zx block=%lluKiB\n", argv[1], data.size(),
                method, win, (unsigned long long)block_kib);
    std::printf("greedy : packed=%llu blocks=%zu  %.2fs  %.2f MB/s\n",
                (unsigned long long)g.packed_total, g.blocks.size(), g.seconds, mib / g.seconds);
    std::printf("forward: packed=%llu blocks=%zu  %.2fs  %.2f MB/s\n",
                (unsigned long long)fa.packed_total, fa.blocks.size(), fa.seconds,
                mib / fa.seconds);
    std::printf("whole-file delta (forward - greedy) = %+lld B\n",
                (long long)fa.packed_total - (long long)g.packed_total);
    std::printf("block alignment: %zu vs %zu blocks, max boundary drift %llu B -> %s\n",
                g.blocks.size(), fa.blocks.size(), (unsigned long long)max_drift,
                aligned ? "OK" : "NOT ALIGNED (ceiling below is unreliable)");

    std::printf("\n%-10s %6s %12s %12s %12s %12s %9s %9s\n", "unit", "units", "greedy", "forward",
                "best-of", "gain", "gain %", "fwd wins");
    for (size_t unit : {size_t(1), size_t(4), size_t(16)}) {
        const auto gu = units(g, n, unit);
        const auto fu = units(fa, n, unit);
        double sg = 0, sf = 0, sb = 0;
        size_t wins = 0;
        for (size_t i = 0; i < gu.size(); ++i) {
            sg += gu[i];
            sf += fu[i];
            sb += std::min(gu[i], fu[i]);
            if (fu[i] < gu[i]) ++wins;
        }
        const double base = std::min(sg, sf);
        const std::string label = std::to_string(unit * block_kib) + "KiB";
        std::printf("%-10s %6zu %12.0f %12.0f %12.0f %12.0f %8.3f%% %4zu/%zu\n", label.c_str(),
                    gu.size(), sg, sf, sb, base - sb, base > 0 ? 100.0 * (base - sb) / base : 0.0,
                    wins, gu.size());
    }

    if (csv) {
        std::printf("\nblock,start_MiB,greedy_B,forward_B,delta_B\n");
        uint64_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            std::printf("%zu,%.3f,%llu,%llu,%+lld\n", i, static_cast<double>(start) / (1 << 20),
                        (unsigned long long)g.blocks[i].packed_bytes,
                        (unsigned long long)fa.blocks[i].packed_bytes,
                        (long long)fa.blocks[i].packed_bytes - (long long)g.blocks[i].packed_bytes);
            start = g.blocks[i].input_end;
        }
    }
    return 0;
}
