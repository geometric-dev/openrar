// v1.37.0 two-phase Gate 0 probe: token census + alpha measurement.
// (Master port: the census walks a whole-member span through decode_span —
// the phase-1 worker path itself — instead of the parked branch's scout.)
//
// v1.43 M1 (RAT-A): the census is now ONE analyzer shared by two streams.
//   * default: compress a synthetic corpus in memory, then analyze the packed
//     member (the "ours" stream).
//   * --ingest <file.rar>: open an EXTERNALLY produced archive with
//     ArchiveReader, read each compressed member's packed stream, and analyze
//     it with the SAME census (the "oracle" stream). This is how a WinRAR
//     archive and one of ours go through identical code.
//
// Per member the analyzer walks the phase-1 OpRecord stream and emits one
// statistic vector: coverage (bytes carried by matches/rep vs literals),
// match-length distribution, distance histogram, rep/257 frequency, and
// filter usage (so a filter-bearing oracle member is reported, never silently
// mixed in). Per-block table entropy is NOT emitted — OpRecord does not carry
// the Huffman tables; that axis needs a deeper decode hook (recorded as owed).
#include "../src/archive/archive_entry.hpp"
#include "../src/archive/archive_reader.hpp"
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

// ---------------------------------------------------------------------------
// The shared statistic vector (v1.43 M1).
// ---------------------------------------------------------------------------
struct StatVector {
    size_t members = 0;
    uint64_t lit_runs = 0, matches = 0, reps = 0, r257 = 0, filters = 0;
    uint64_t lit_bytes = 0, match_bytes = 0, rep_bytes = 0;
    uint64_t total_bytes = 0;     // uncompressed bytes covered by all members
    uint64_t len_hist[17] = {0};  // log2-bucketed match/rep length
    uint64_t dist_hist[17] = {0}; // log2-bucketed match distance (a+1)
    bool filter_used = false;

    void merge(const StatVector& o) {
        members += o.members;
        lit_runs += o.lit_runs;
        matches += o.matches;
        reps += o.reps;
        r257 += o.r257;
        filters += o.filters;
        lit_bytes += o.lit_bytes;
        match_bytes += o.match_bytes;
        rep_bytes += o.rep_bytes;
        total_bytes += o.total_bytes;
        for (int i = 0; i < 17; ++i) {
            len_hist[i] += o.len_hist[i];
            dist_hist[i] += o.dist_hist[i];
        }
        filter_used = filter_used || o.filter_used;
    }
};

int log2_bucket(uint64_t v) {
    int b = 0;
    while (v > 1) {
        v >>= 1;
        ++b;
    }
    return b > 16 ? 16 : b;
}

// THE analyzer. Runs a packed member stream through the real phase-1 decode
// path and fills `sv`. Used identically for our encoder's output and for an
// external archive's members, so the two are directly comparable.
bool census_member(const std::vector<core::byte>& packed, size_t out_size, size_t win,
                   StatVector& sv, bool verbose) {
    Decompressor50 ps(win);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(packed.data(), packed.size(), tl)) {
        if (verbose) std::printf("    prescan FAILED (packed=%zu win=%zu)\n", packed.size(), win);
        return false;
    }
    Decompressor50 w(win);
    Decompressor50::SpanRecords sr;
    if (!w.decode_span(tl, packed.data(), packed.size(), out_size, 0,
                       static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31, sr)) {
        if (verbose)
            std::printf("    decode_span FAILED (blocks=%zu out=%zu)\n", tl.blocks.size(),
                        out_size);
        return false;
    }
    // Walk the OpRecord stream once. R257 re-emits (old_dist[0], last_length),
    // so track last_length to attribute its bytes correctly.
    uint64_t last_len = 0;
    for (const auto& r : sr.recs) {
        switch (static_cast<Decompressor50::OpRecord::Tag>(r.tag)) {
        case Decompressor50::OpRecord::Tag::Lit:
            ++sv.lit_runs;
            sv.lit_bytes += r.aux; // aux = run length (1..255)
            break;
        case Decompressor50::OpRecord::Tag::Match:
            ++sv.matches;
            sv.match_bytes += r.b;
            sv.len_hist[log2_bucket(r.b)]++;
            sv.dist_hist[log2_bucket(static_cast<uint64_t>(r.a) + 1)]++;
            last_len = r.b;
            break;
        case Decompressor50::OpRecord::Tag::Rep:
            ++sv.reps;
            sv.rep_bytes += r.b;
            sv.len_hist[log2_bucket(r.b)]++;
            last_len = r.b;
            break;
        case Decompressor50::OpRecord::Tag::R257:
            ++sv.r257;
            sv.match_bytes += last_len; // re-covers last_length bytes
            sv.len_hist[log2_bucket(last_len)]++;
            break;
        case Decompressor50::OpRecord::Tag::Filter:
            ++sv.filters;
            sv.filter_used = true;
            break;
        }
    }
    sv.total_bytes += out_size;
    ++sv.members;
    return true;
}

void print_stats(const char* label, const StatVector& sv, int method) {
    const double tot = sv.total_bytes ? static_cast<double>(sv.total_bytes) : 1.0;
    const double cov = (static_cast<double>(sv.match_bytes) + sv.rep_bytes) / tot;
    const uint64_t toks = sv.matches + sv.reps + sv.r257;
    std::printf("%s m%d | members=%zu tokens=%llu | COVERAGE=%.4f (match=%.4f rep=%.4f) | "
                "rep%%=%.3f 257%%=%.3f | filter=%s(%llu) | lits=%llu\n",
                label, method, sv.members, static_cast<unsigned long long>(toks), cov,
                static_cast<double>(sv.match_bytes) / tot, static_cast<double>(sv.rep_bytes) / tot,
                toks ? 100.0 * sv.reps / toks : 0.0, toks ? 100.0 * sv.r257 / toks : 0.0,
                sv.filter_used ? "YES" : "no", static_cast<unsigned long long>(sv.filters),
                static_cast<unsigned long long>(sv.lit_bytes));
    std::printf("   len_hist(2^k bytes):");
    for (int i = 0; i <= 16; ++i)
        if (sv.len_hist[i])
            std::printf(" %d:%llu", i, static_cast<unsigned long long>(sv.len_hist[i]));
    std::printf("\n   dist_hist(2^k):");
    for (int i = 0; i <= 16; ++i)
        if (sv.dist_hist[i])
            std::printf(" %d:%llu", i, static_cast<unsigned long long>(sv.dist_hist[i]));
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// v1.43 M1: ingest an EXTERNALLY produced archive (the oracle stream).
// ---------------------------------------------------------------------------
void ingest(const char* path) {
    archive::ArchiveReader r;
    if (!r.open(path)) {
        std::printf("ingest: cannot open '%s'\n", path);
        return;
    }
    StatVector all;
    size_t skipped_store = 0, skipped_service = 0, skipped_dir = 0, failed = 0;
    for (const auto& e : r.entries()) {
        if (e.header.is_service) {
            ++skipped_service;
            continue;
        }
        if (e.header.file_flags & format::FHFL_DIRECTORY) {
            ++skipped_dir;
            continue;
        }
        if (e.header.method == 0) {
            ++skipped_store;
            continue;
        } // stored: no token stream
        size_t win = static_cast<size_t>(e.header.win_size);
        if (win == 0) win = 32u * 1024 * 1024;
        std::vector<core::byte> packed;
        if (!r.read_packed_data(e, packed)) {
            ++failed;
            continue;
        }
        StatVector sv;
        size_t out_size = static_cast<size_t>(e.header.unp_size);
        if (!census_member(packed, out_size, win, sv, /*verbose=*/false)) {
            ++failed;
            std::printf("    member method=%d win=%zu packed=%zu out=%zu -> CENSUS FAILED\n",
                        e.header.method, win, packed.size(), out_size);
            continue;
        }
        std::printf("  member: method=%d win=%zu packed=%zu out=%zu blocks-ok\n", e.header.method,
                    win, packed.size(), out_size);
        all.merge(sv);
    }
    std::printf("\n");
    print_stats("INGESTED", all, 0);
    std::printf("   skipped: store=%zu service=%zu dir=%zu   failed-members=%zu\n", skipped_store,
                skipped_service, skipped_dir, failed);
    if (all.filter_used)
        std::printf("   NOTE: filter-bearing member(s) present — oracle uses features our "
                    "token-level compare must account for (declared restriction).\n");
}

// ---------------------------------------------------------------------------
// Existing v1.37 alpha+census path ("ours" stream), now routed through the
// shared analyzer so both streams use identical census code.
// ---------------------------------------------------------------------------
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
    // Census via the SHARED analyzer (same code the ingest stream uses).
    StatVector sv;
    if (!census_member(packed, data.size(), win, sv, /*verbose=*/true)) {
        std::printf("%-14s m%d: CENSUS FAILED\n", name, method);
        return;
    }
    print_stats(name, sv, method);

    // alpha (symbol-stage vs sequential) through the real phase-1 path.
    Decompressor50 ps(win);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(packed.data(), packed.size(), tl)) return;
    double symbol_ms = min_time_ms(
        [&] {
            Decompressor50 w(win);
            Decompressor50::SpanRecords sr;
            w.decode_span(tl, packed.data(), packed.size(), data.size(), 0,
                          static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31, sr);
        },
        5);
    double seq_ms = min_time_ms(
        [&] {
            Decompressor50 d(win);
            size_t written = 0;
            d.decompress(packed.data(), packed.size(), data.size(), false, nullptr, &written);
        },
        5);
    const double alpha = symbol_ms / seq_ms;
    std::printf("%-14s m%d packed=%9zu out=%9zu | sym=%7.1fms seq=%7.1fms alpha=%.3f | "
                "pipelined-x4=%.2fx unpipelined-x4=%.2fx\n",
                name, method, packed.size(), data.size(), symbol_ms, seq_ms, alpha,
                1.0 / (1.0 - alpha), 4.0 / (1.0 + 3.0 * alpha));
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && std::string(argv[1]) == "--ingest") {
        std::printf("=== v1.43 M1: oracle-side census (ingesting %s) ===\n", argv[2]);
        ingest(argv[2]);
        return 0;
    }
    const size_t M = 1024 * 1024;
    std::printf("=== v1.37.0 two-phase probe: alpha + shared token census (master path) ===\n");
    run("text64m", make_text(64 * M, 0x137), 3, 0x200000);
    run("text64m", make_text(64 * M, 0x137), 5, 0x200000);
    run("zeros64m", make_zeros(64 * M), 3, 0x200000);
    run("random4m", make_random(4 * M, 7), 3, 0x200000);
    run("exelike16m", make_exelike(16 * M, 99), 3, 0x200000);
    run("exelike16m", make_exelike(16 * M, 99), 5, 0x200000);
    return 0;
}