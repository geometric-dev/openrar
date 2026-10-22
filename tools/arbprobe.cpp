// v1.43 M2 axis-5 — RAT-A: arbiter cost-pricing disagreement probe (log-only).
//
// The current arbiter picks the LONGEST match (monotone-max), comparing
// `rep_best_len + 1 > best.length` — raw coverage, never pricing the distance
// code. A rep match reuses a stored distance and therefore SKIPS the entire
// distance code, so a rep that covers 1–3 bytes LESS can be CHEAPER in bits
// than a long-distance fresh match — a choice the length-only rule can never
// make. This probe measures how often a cost-aware arbiter would DISAGREE with
// the current rule, and in which direction.
//
// Log-only: enabling it cannot change an emitted byte (the counters never feed
// back into len/dist/is_rep). The price model is an approximation; the point is
// the disagreement RATE and DIRECTION, which are robust to small model error.
//
// Drive the shipping single-buffer path (begin_archive + set_external_buffer +
// compress), same as h1probe, so the measurement reflects the real finder.

#include "../src/compress/compressor50.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

namespace {

std::vector<core::byte> make_text(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::string> vocab;
    for (int i = 0; i < 600; ++i) vocab.push_back("w" + std::to_string(i));
    for (const auto& w : std::vector<std::string>{"the", "quick", "brown", "window", "archive",
                                                  "decode", "parallel", "range", "repeat", "match"})
        vocab.push_back(w);
    std::vector<core::byte> out;
    out.reserve(n + 64);
    std::string line;
    while (out.size() < n) {
        line += vocab[rng() % vocab.size()];
        if (line.size() > 72) {
            line += '\n';
            out.insert(out.end(), line.begin(), line.end());
            line.clear();
        } else
            line += ' ';
    }
    out.resize(n);
    return out;
}
std::vector<core::byte> make_zeros(size_t n) {
    return std::vector<core::byte>(n, 0);
}
std::vector<core::byte> make_random(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<core::byte> out(n);
    for (auto& b : out) b = static_cast<core::byte>(rng());
    return out;
}

// Concession/contested-class corpus: an 8 KiB block repeated at distance
// 0x6000 (inc == 2 band), separated by pseudo-random filler — maximizes
// the contested positions and the far-band candidate population.
std::vector<core::byte> make_far_repeat(size_t copies) {
    std::vector<core::byte> block = make_random(8192, 4242);
    std::vector<core::byte> sep = make_random(16384, 1717);
    std::vector<core::byte> out;
    out.reserve(copies * (block.size() + sep.size()));
    for (size_t i = 0; i < copies; ++i) {
        out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), sep.begin(), sep.end());
    }
    return out;
}

void run(const char* name, const std::vector<core::byte>& data, int method, size_t win) {
    size_t comp_win = win;
    if (data.size() < win) {
        size_t p = 0x20000;
        while (p < data.size() && p < win) p <<= 1;
        comp_win = std::min(win, p);
    }
    Compressor50 packer;
    std::vector<core::byte> packed;
    packer.begin_archive(nullptr, method, comp_win);
    packer.set_external_buffer(data.data(), data.size());
    packer.set_memory_dest(&packed);
    packer.arb_probe_enable(true);
    packer.arb_probe_reset();
    if (packer.compress() < 0 || packed.empty()) {
        std::printf("%-12s m%d: COMPRESS FAILED\n", name, method);
        return;
    }
    const auto& s = packer.arb_probe_stats();
    if (s.decisions == 0) {
        std::printf("%-12s m%d: no decisions (vacuous)\n", name, method);
        return;
    }
    const double contested = s.contested ? static_cast<double>(s.contested) : 1.0;
    const double dfrac = 100.0 * s.differs / contested;
    // Axis-6 (A-6 per-band slots): what did the SHIPPED finder hide?
    const double lp = s.lit_positions ? static_cast<double>(s.lit_positions) : 1.0;
    const double fixfrac = 100.0 * s.band_fixable / lp;
    const double fp = s.fresh_positions ? static_cast<double>(s.fresh_positions) : 1.0;
    const double swapfrac = 100.0 * s.band_swap / fp;
    const double pf = s.band_swap ? static_cast<double>(s.band_swap) : 1.0;
    std::printf("%-12s m%d | deco=%-9llu contested=%-7llu differs=%-7llu (%5.2f%%) "
                "| arb-probe: lit_fav=%-7llu fresh_fav=%-6llu bitsSaved=%-7llu\n",
                name, method, (unsigned long long)s.decisions, (unsigned long long)s.contested,
                (unsigned long long)s.differs, dfrac, (unsigned long long)s.prefer_rep,
                (unsigned long long)s.prefer_fresh, (unsigned long long)s.bits_saved);
    std::printf(
        "%-12s   | A-6 probe: lit-pos=%-9llu fresh-pos=%-9llu | fixable=%-8llu (%5.2f%% of rec) "
        "fix_bits=%-8llu bands=[0:%-6llu 1:%-6llu 2:%-6llu 3:%-6llu] | swaps=%-7llu (%5.2f%% of "
        "fresh) "
        "swap_bits=%-7llu profitable(>=8b)=%-7llu (%5.1f%% of swaps, %llu bits)\n",
        name, (unsigned long long)s.lit_positions, (unsigned long long)s.fresh_positions,
        (unsigned long long)s.band_fixable, fixfrac, (unsigned long long)s.band_fix_bits,
        (unsigned long long)s.fix_band[0], (unsigned long long)s.fix_band[1],
        (unsigned long long)s.fix_band[2], (unsigned long long)s.fix_band[3],
        (unsigned long long)s.band_swap, swapfrac, (unsigned long long)s.band_swap_bits,
        (unsigned long long)s.swap_profit, 100.0 * s.swap_profit / pf,
        (unsigned long long)s.swap_profit_bits);
}

} // namespace

#include <fstream>

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t M = 1024 * 1024;
    std::printf("=== v1.43 M2 axes 5+6 RAT-A: arbiter pricing + per-band candidate recovery ===\n");
    std::printf("differs = static price disagrees with the shipped rule; A-6 = matches the walk "
                "saw and hid\n\n");
    if (argc > 1) {
        // Real-corpus mode: probe one file; argv[2] = window (C literal, e.g.
        // 0x800000), argv[3] = method (default 3). Splits encoder-honors-win
        // vs CLI-floor when comparing two windows on the same file.
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) {
            std::printf("cannot open %s\n", argv[1]);
            return 1;
        }
        std::vector<core::byte> data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
        size_t win = 0x800000;
        if (argc > 2) win = static_cast<size_t>(std::strtoul(argv[2], nullptr, 0));
        int method = 3;
        if (argc > 3) method = static_cast<int>(std::strtoul(argv[3], nullptr, 0));
        std::printf("real corpus: %s (%zu bytes), win=0x%zx m%d\n", argv[1], data.size(), win,
                    method);
        run(argv[1], data, method, win);
        return 0;
    }
    run("text6m", make_text(6 * M, 0x137), 3, 0x200000);
    run("text6m", make_text(6 * M, 0x137), 5, 0x200000);
    run("farrepeat", make_far_repeat(40), 3, 0x200000);
    run("zeros6m", make_zeros(6 * M), 3, 0x200000);
    run("random6m", make_random(6 * M, 7), 3, 0x200000);
    return 0;
}