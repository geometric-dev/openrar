// v1.37.0 two-phase Gate 0 probe: token census + alpha measurement.
// (Master port: the census walks a whole-member span through decode_span —
// the phase-1 worker path itself — instead of the parked branch's scout.)
//
// For each corpus x method: compress in memory, pre-scan, then
//  (1) decode_span over all blocks (symbol-only pass, zero byte movement)
//      -> token-class census, record-volume estimate;
//  (2) timed min-of-5 of the symbol pass vs the full sequential decode.
// alpha = symbol_time / sequential_time estimates the parallelizable
// symbol-stage fraction; the pipelined two-phase wall model is
// max(alpha/R_ht, 1-alpha) (apply floor), the unpipelined one
// R/(1+(R-1)*alpha).
#include "../src/compress/compressor50.hpp"
#include "../src/compress/decompressor50.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
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

std::vector<core::byte> make_zeros(size_t n) {
    return std::vector<core::byte>(n, 0);
}

std::vector<core::byte> make_random(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<core::byte> out(n);
    for (auto& b : out) b = static_cast<core::byte>(rng());
    return out;
}

// Pseudo-EXE: low-entropy code bytes with E8 call patterns that engage the
// E8 filter path, interleaved with string-table stretches.
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
        for (size_t i = 0; i < 8192 && out.size() < n; ++i) {
            out.push_back(static_cast<core::byte>(s[rng() % sl]));
        }
    }
    out.resize(n);
    return out;
}

template <typename F> double min_time_ms(F&& f, int reps) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    return best;
}

void run(const char* name, const std::vector<core::byte>& data, int method, size_t win) {
    std::vector<core::byte> packed;
    if (!Compressor50::compress_buffer(data.data(), data.size(), packed, method, win)) {
        std::printf("%-14s m%d: COMPRESS FAILED\n", name, method);
        return;
    }
    Decompressor50 ps(win);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(packed.data(), packed.size(), tl)) {
        std::printf("%-14s m%d: PRES CAN FAILED\n", name, method);
        return;
    }

    // Census + symbol-stage timing through the real phase-1 path.
    size_t n_lit_runs = 0, n_match = 0, n_rep = 0, n_257 = 0, n_filter = 0;
    size_t rec_bytes = 0, pool_bytes = 0;
    double symbol_ms = min_time_ms(
        [&] {
            Decompressor50 w(win);
            Decompressor50::SpanRecords sr;
            if (!w.decode_span(tl, packed.data(), packed.size(), data.size(), 0,
                               static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31, sr)) {
                std::printf("  decode_span FAILED\n");
                return;
            }
            n_lit_runs = n_match = n_rep = n_257 = n_filter = 0;
            rec_bytes = sr.recs.size() * sizeof(Decompressor50::OpRecord);
            pool_bytes = sr.lit_pool.size();
            for (const auto& r : sr.recs) {
                switch (static_cast<Decompressor50::OpRecord::Tag>(r.tag)) {
                case Decompressor50::OpRecord::Tag::Lit:
                    ++n_lit_runs;
                    break;
                case Decompressor50::OpRecord::Tag::Match:
                    ++n_match;
                    break;
                case Decompressor50::OpRecord::Tag::Rep:
                    ++n_rep;
                    break;
                case Decompressor50::OpRecord::Tag::R257:
                    ++n_257;
                    break;
                case Decompressor50::OpRecord::Tag::Filter:
                    ++n_filter;
                    break;
                }
            }
        },
        5);

    double seq_ms = min_time_ms(
        [&] {
            Decompressor50 d(win);
            size_t written = 0;
            if (!d.decompress(packed.data(), packed.size(), data.size(), false, nullptr,
                              &written)) {
                std::printf("  seq decode FAILED\n");
            }
        },
        5);

    const double alpha = symbol_ms / seq_ms;
    std::printf("%-14s m%d win=%zuMiB packed=%9zu out=%9zu | litruns=%-7zu match=%-8zu rep=%-7zu "
                "257=%-7zu filt=%-5zu | recvol=%8zu+%zu (%.2fx out) | sym=%7.1fms seq=%7.1fms "
                "alpha=%.3f | pipelined-x4=%.2fx unpipelined-x4=%.2fx\n",
                name, method, win >> 20, packed.size(), data.size(), n_lit_runs, n_match, n_rep,
                n_257, n_filter, rec_bytes, pool_bytes,
                (double)(rec_bytes + pool_bytes) / (double)data.size(), symbol_ms, seq_ms, alpha,
                1.0 / (1.0 - alpha),        // apply floor, symbols fully covered
                4.0 / (1.0 + 3.0 * alpha)); // symbols parallel, apply serial after join
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t M = 1024 * 1024;
    std::printf("=== v1.37.0 two-phase probe: alpha + record census (master path) ===\n");
    run("text64m", make_text(64 * M, 0x137), 3, 0x200000);
    run("text64m", make_text(64 * M, 0x137), 5, 0x200000);
    run("zeros64m", make_zeros(64 * M), 3, 0x200000);
    run("random4m", make_random(4 * M, 7), 3, 0x200000);
    run("exelike16m", make_exelike(16 * M, 99), 3, 0x200000);
    run("exelike16m", make_exelike(16 * M, 99), 5, 0x200000);
    return 0;
}
