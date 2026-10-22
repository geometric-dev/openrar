// v1.43 M3 — RAT-F: H1 chain-budget reachability probe (log-only).
//
// The question H1 rests on: a candidate cache would let the walk at token
// start `q` skip past the budget-limited nodes. That only helps where the walk
// terminated by running out of CHAIN BUDGET with a candidate still waiting —
// not where it found a nice_len match, not where the candidate left the
// window, and not where the chain simply ran out of nodes.
//
// So the observable is the walk's TERMINATION REASON, split into:
//   (a) budget exhausted, candidate remained   <- the only cacheable population
//   (d) supply exhausted (ran out of nodes first)
//   (b) nice_len/limit break
//   (c) candidate left the max-distance window
//
// For each (a) site the probe also runs a counterfactual UNBUDGETED re-walk of
// the same chain and records how many extra matched bytes truncation hid. The
// kill test uses the generous bound (recoverable_sites * max_delta / input),
// because if even that cannot reach 0.1% of the archive, H1 is definitively
// dead. A cache hit-rate is NOT this measurement.
//
// Log-only: enabling the probe cannot change an emitted byte. The counters are
// written beside the walk, never read by the emission path.

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
                                                  "decode", "parallel", "range"})
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
        } else {
            line += ' ';
        }
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

std::vector<core::byte> make_exelike(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<core::byte> out;
    out.reserve(n);
    while (out.size() < n) {
        for (size_t i = 0; i < 4096 && out.size() < n; ++i) {
            out.push_back(static_cast<core::byte>(rng() % 16));
            if (rng() % 24 == 0 && out.size() + 5 <= n) {
                out.push_back(0xE8);
                core::uint32 rel = rng() % 0x100000;
                for (int k = 0; k < 4; ++k) out.push_back(static_cast<core::byte>(rel >> (k * 8)));
            }
        }
        const char* s = "kernel32.dll\\GetProcA_address\x00table entry value ";
        size_t sl = 45;
        for (size_t i = 0; i < 8192 && out.size() < n; ++i)
            out.push_back(static_cast<core::byte>(s[rng() % sl]));
    }
    out.resize(n);
    return out;
}

void run(const char* name, const std::vector<core::byte>& data, int method, size_t win) {
    // Clamp the window exactly as Compressor50::compress_buffer does for the
    // single-buffer path, then drive the SAME instance path it uses
    // (begin_archive + set_external_buffer + compress), so this measures the
    // shipping finder. compress_buffer is static and reaches private
    // mem_src_* for large inputs, which an external tool cannot drive; the
    // instance path is the real single-buffer path and is byte-identical for
    // corpora at or below window + 4 MiB (which all of these are).
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
    // Enable AFTER setup (latch the instance state the walk will use).
    packer.h1_probe_enable(true);
    packer.h1_probe_set_extra_chain(2048);
    packer.h1_probe_reset();

    if (packer.compress() < 0 || packed.empty()) {
        std::printf("%-12s m%d: COMPRESS FAILED\n", name, method);
        return;
    }

    const auto& s = packer.h1_probe_stats();
    if (s.calls == 0) {
        std::printf("%-12s m%d: no instrumented walks (calls==0) — VACUOUS, not a result\n", name,
                    method);
        return;
    }

    const double tot = static_cast<double>(s.calls);
    const double f_a = static_cast<double>(s.budget_exhausted) / tot;
    const double f_w = s.budget_exhausted ? static_cast<double>(s.recoverable) /
                                                static_cast<double>(s.budget_exhausted)
                                          : 0.0;
    // Generous upper bound: EVERY recoverable site gains the LARGEST single gain
    // seen anywhere. If this cannot clear 0.1%, no cache can pay for itself.
    const double upper =
        static_cast<double>(s.recoverable * s.max_delta) / static_cast<double>(data.size());
    // Tighter, actual: sum the real per-site deltas.
    const double tight =
        static_cast<double>(s.recoverable_bytes) / static_cast<double>(data.size());

    std::printf("%-12s m%d | calls=%-9llu a=%5.1f%% d=%5.1f%% b=%5.1f%% c=%5.1f%% | "
                "recov(a)=%-8llu maxd=%-4llu | f_a=%.4f f_w=%.4f | upper=%.4f%% tight=%.4f%% %s\n",
                name, method, static_cast<unsigned long long>(s.calls), f_a * 100.0,
                100.0 * static_cast<double>(s.supply_exhausted) / tot,
                100.0 * static_cast<double>(s.nice_len_break) / tot,
                100.0 * static_cast<double>(s.max_dist_break) / tot,
                static_cast<unsigned long long>(s.recoverable),
                static_cast<unsigned long long>(s.max_delta), f_a, f_w, upper * 100.0,
                tight * 100.0,
                (upper * 100.0 < 0.1) ? "=> H1 KILLED (upper < 0.1%)" : "=> survives bar");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t M = 1024 * 1024;
    std::printf("=== v1.43 M3 RAT-F: H1 chain-budget reachability (termination-reason) ===\n");
    std::printf("(a)=budget-exhausted  (d)=supply-exhausted  (b)=nice_len  (c)=max_dist\n");
    std::printf("Kill bar: generous upper bound on archive-size gain < 0.1%%\n");
    std::printf("(raw single-buffer path, no filter; window+4MiB keeps every corpus on the\n");
    std::printf(" shipping instance path. Fractions over token starts.)\n\n");
    run("text6m", make_text(6 * M, 0x137), 1, 0x200000);
    run("text6m", make_text(6 * M, 0x137), 3, 0x200000);
    run("text6m", make_text(6 * M, 0x137), 5, 0x200000);
    run("zeros6m", make_zeros(6 * M), 1, 0x200000);
    run("zeros6m", make_zeros(6 * M), 3, 0x200000);
    run("exelike6m", make_exelike(6 * M, 99), 3, 0x200000);
    run("random6m", make_random(6 * M, 7), 3, 0x200000);
    return 0;
}