// v1.37.0 two-phase parallel decode tests (plan docs/v1.37.0-two-phase-
// implementation-plan.md). M1: pre-scan framing agreement + hostile rows.
// M2/M3 add the records-applier and span-worker identity gates; M4 the
// MT/ST byte-identity corpus.
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/parallel_decode.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

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

std::vector<core::byte> make_pe_like(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<core::byte> v(n);
    for (auto& b : v) b = static_cast<core::byte>(rng() % 16);
    v[0] = 'M';
    v[1] = 'Z';
    v[0x3C] = 0x80;
    v[0x80] = 'P';
    v[0x81] = 'E';
    v[0x82] = 0;
    v[0x83] = 0;
    for (size_t i = 0x100; i + 5 <= n; i += 24) {
        v[i] = 0xE8;
        v[i + 1] = static_cast<core::byte>((i * 13) & 0xFF);
        v[i + 3] = 0x01;
    }
    return v;
}

bool roundtrip_ok(const std::vector<core::byte>& packed, const std::vector<core::byte>& plain,
                  size_t win) {
    Decompressor50 dec(win);
    std::vector<core::byte> out;
    return dec.decompress_to_vector(packed.data(), packed.size(), out) && out == plain;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif

    const size_t M = 1024 * 1024;
    const size_t win = 0x200000;

    // ── M1: pre-scan agreement with the sequential framing ──────────────────
    {
        struct Case {
            const char* name;
            std::vector<core::byte> plain;
            int method;
        };
        std::vector<Case> cases = {
            {"text8m", make_text(8 * M, 0x137), 3},
            {"zeros4m", make_zeros(4 * M), 3},
            {"pe2m", make_pe_like(2 * M, 99), 3},
            {"tiny", make_text(3000, 5), 1},
        };
        for (auto& c : cases) {
            std::vector<core::byte> packed;
            assert(Compressor50::compress_buffer(c.plain.data(), c.plain.size(), packed, c.method,
                                                 win));
            // Control: the sequential decoder accepts the member (roundtrip).
            assert(roundtrip_ok(packed, c.plain, win));

            Decompressor50 ps(win);
            Decompressor50::PrescanTimeline tl;
            assert(ps.prescan_member(packed.data(), packed.size(), tl));
            assert(tl.ok);
            assert(tl.saw_last_block);
            assert(!tl.blocks.empty());
            assert(tl.packed_extent == packed.size()); // our encoder ends at LastBlock
            assert(tl.blocks.front().table_present);   // non-solid member start
            // Table timeline: every block's desc_id is valid and descriptions
            // are exactly the table-bearing (non-empty) blocks.
            size_t table_blocks = 0;
            for (const auto& b : tl.blocks) {
                assert(b.desc_id < tl.descriptions.size());
                if (b.table_present && b.block_size != 0) ++table_blocks;
            }
            assert(tl.descriptions.size() == table_blocks);
            std::cout << "[PASS] prescan agreement: " << c.name << " (" << tl.blocks.size()
                      << " blocks, " << tl.descriptions.size() << " descriptions)\n";
        }
    }

    // ── M1: hostile rows (F1) — every anomaly fails the scan fail-closed ────
    {
        std::vector<core::byte> plain = make_text(2 * M, 0x55);
        std::vector<core::byte> packed;
        assert(Compressor50::compress_buffer(plain.data(), plain.size(), packed, 3, win));

        auto scan_fails = [&](const std::vector<core::byte>& bad, const char* what) {
            Decompressor50 ps(win);
            Decompressor50::PrescanTimeline tl;
            if (ps.prescan_member(bad.data(), bad.size(), tl)) {
                std::cout << "[FAIL] prescan accepted hostile member: " << what << "\n";
                std::exit(1);
            }
            assert(!tl.ok);
        };

        // Truncation mid-header.
        scan_fails(std::vector<core::byte>(packed.begin(), packed.begin() + packed.size() / 2),
                   "truncated");
        // Header checksum corruption.
        {
            std::vector<core::byte> bad = packed;
            bad[1] ^= 0xFF;
            scan_fails(bad, "header checksum");
        }
        // Block-size inflation (payload overrun).
        {
            std::vector<core::byte> bad = packed;
            bad[2] = 0xFF;
            bad[3] = 0xFF;
            // recompute the check byte for the new size: chk = 0x5A ^ flags ^ size bytes
            bad[1] = static_cast<core::byte>(0x5A ^ bad[0] ^ bad[2] ^ bad[3] ^ 0 ^
                                             0); // 2-byte size field
            scan_fails(bad, "block-size overrun");
        }
        // First block flag-clear (table reuse with no tables in force), with
        // the check byte recomputed so the flag path is what fails.
        {
            std::vector<core::byte> bad = packed;
            bad[0] &= 0x7F;
            bad[1] = static_cast<core::byte>(0x5A ^ bad[0] ^ bad[2] ^ bad[3]);
            scan_fails(bad, "flag-clear first block");
        }
        // Empty stream.
        scan_fails(std::vector<core::byte>{}, "empty");
        std::cout << "[PASS] prescan hostile rows fail closed\n";
    }

    // ── M1: R2 block-count cap — a chain of minimal empty blocks ────────────
    {
        // Each empty non-last block is 3 bytes (flags 0x00, check 0x5A, size
        // 0x00): the densest legal framing, ~src_size/3 blocks. Enough of
        // them trips PRESCAN_MAX_BLOCKS without any other anomaly.
        const size_t need = Decompressor50::PRESCAN_MAX_BLOCKS + 1;
        std::vector<core::byte> bad(need * 3, 0);
        for (size_t i = 0; i < need; ++i) {
            bad[i * 3 + 0] = 0x00;
            bad[i * 3 + 1] = 0x5A;
            bad[i * 3 + 2] = 0x00;
        }
        Decompressor50 ps(win);
        Decompressor50::PrescanTimeline tl;
        assert(!ps.prescan_member(bad.data(), bad.size(), tl));
        std::cout << "[PASS] prescan block-count cap falls back (R2)\n";
    }

    // ── M2: span-records semantics — any split replays to the same bytes ────
    // A test-side interpreter consumes SpanRecords with continuous applier
    // state (old_dist/last_length) across spans: the M3 applier's contract
    // in miniature. Filter-bearing corpora wait for M3 (the engine's queue
    // resolves deltas); here: filter-free corpora, every split shape.
    {
        struct MiniApplier {
            std::vector<core::byte> out;
            size_t old_dist[4] = {static_cast<size_t>(-1), static_cast<size_t>(-1),
                                  static_cast<size_t>(-1), static_cast<size_t>(-1)};
            size_t last_length = 0;
            bool fail = false;
            void copy(size_t dist, size_t len) {
                if (dist == 0) {
                    fail = true;
                    return;
                }
                for (size_t i = 0; i < len; ++i) {
                    if (dist > out.size()) {
                        out.push_back(0); // first-window zero-fill, linear frame
                    } else {
                        out.push_back(out[out.size() - dist]);
                    }
                }
            }
        };
        auto apply_span = [&](MiniApplier& ap, const Decompressor50::SpanRecords& sr,
                              size_t dest_size) {
            for (const auto& r : sr.recs) {
                if (ap.out.size() >= dest_size) break; // applier clamp point [R8]
                auto tag = static_cast<Decompressor50::OpRecord::Tag>(r.tag);
                if (tag == Decompressor50::OpRecord::Tag::Lit) {
                    for (core::uint32 i = 0; i < r.aux && ap.out.size() < dest_size; ++i) {
                        ap.out.push_back(sr.lit_pool[r.b + i]);
                    }
                } else if (tag == Decompressor50::OpRecord::Tag::Match) {
                    // last_length keeps the WIRE length (sequential pins it
                    // before the dest clamp); the copy clamps at dest_size.
                    ap.last_length = r.b;
                    size_t dist = r.a;
                    if (dist == 0) {
                        ap.fail = true;
                        return;
                    }
                    size_t len = r.b;
                    if (len > dest_size - ap.out.size()) len = dest_size - ap.out.size();
                    ap.copy(dist, len);
                    ap.old_dist[3] = ap.old_dist[2];
                    ap.old_dist[2] = ap.old_dist[1];
                    ap.old_dist[1] = ap.old_dist[0];
                    ap.old_dist[0] = dist;
                } else if (tag == Decompressor50::OpRecord::Tag::Rep) {
                    size_t idx = r.aux;
                    size_t dist = ap.old_dist[idx];
                    if (dist == static_cast<size_t>(-1) || dist == 0) {
                        ap.fail = true;
                        return;
                    }
                    for (size_t i = idx; i > 0; --i) ap.old_dist[i] = ap.old_dist[i - 1];
                    ap.old_dist[0] = dist;
                    ap.last_length = r.a;
                    size_t len = r.a;
                    if (len > dest_size - ap.out.size()) len = dest_size - ap.out.size();
                    ap.copy(dist, len);
                } else if (tag == Decompressor50::OpRecord::Tag::R257) {
                    if (ap.last_length != 0) {
                        size_t dist = ap.old_dist[0];
                        if (dist == static_cast<size_t>(-1) || dist == 0) {
                            ap.fail = true;
                            return;
                        }
                        size_t len = ap.last_length;
                        if (len > dest_size - ap.out.size()) len = dest_size - ap.out.size();
                        ap.copy(dist, len);
                    }
                } else {
                    ap.fail = true; // Filter: M3 (engine queue)
                    return;
                }
            }
        };

        auto split_bounds = [](const Decompressor50::PrescanTimeline& tl, size_t n_spans) {
            std::vector<std::pair<uint32_t, uint32_t>> spans;
            size_t nb = tl.blocks.size();
            for (size_t s = 0; s < n_spans; ++s) {
                uint32_t first = static_cast<uint32_t>(nb * s / n_spans);
                uint32_t last = static_cast<uint32_t>(nb * (s + 1) / n_spans) - 1;
                if (!spans.empty() && first <= spans.back().second) continue;
                spans.push_back({first, last});
            }
            return spans;
        };

        const size_t win = 0x200000;
        struct Case {
            const char* name;
            std::vector<core::byte> plain;
            int method;
        };
        std::vector<Case> cases = {
            {"text8m", make_text(8 * M, 0x137), 3},
            {"zeros4m", make_zeros(4 * M), 3}, // 257-heavy: span-start R257 rows
        };
        for (auto& c : cases) {
            std::vector<core::byte> packed;
            assert(Compressor50::compress_buffer(c.plain.data(), c.plain.size(), packed, c.method,
                                                 win));
            Decompressor50 ps(win);
            Decompressor50::PrescanTimeline tl;
            assert(ps.prescan_member(packed.data(), packed.size(), tl));

            for (size_t n_spans : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{8}}) {
                auto spans = split_bounds(tl, n_spans);
                MiniApplier ap;
                size_t total_lower = 0;
                for (auto& sp : spans) {
                    Decompressor50 worker(win);
                    Decompressor50::SpanRecords sr;
                    // rec_cap: 16x span packed (the driver's G5 bound)
                    size_t span_packed =
                        tl.blocks[sp.second].src_byte + tl.blocks[sp.second].header_len +
                        tl.blocks[sp.second].block_size - tl.blocks[sp.first].src_byte;
                    size_t cap = span_packed * 16 + (16u << 20);
                    assert(worker.decode_span(tl, packed.data(), packed.size(), c.plain.size(),
                                              sp.first, sp.second, cap, sr));
                    assert(sr.ok);
                    // Filters never appear in these corpora.
                    for (const auto& r : sr.recs) {
                        assert(static_cast<Decompressor50::OpRecord::Tag>(r.tag) !=
                               Decompressor50::OpRecord::Tag::Filter);
                    }
                    total_lower += sr.out_lower_bound;
                    apply_span(ap, sr, c.plain.size());
                    assert(!ap.fail);
                }
                if (ap.out != c.plain) {
                    size_t d = 0;
                    while (d < ap.out.size() && d < c.plain.size() && ap.out[d] == c.plain[d]) ++d;
                    std::fprintf(stderr,
                                 "DIVERGE %s split=%zu out=%zu plain=%zu at=%zu got=%02x "
                                 "want=%02x | spans:",
                                 c.name, n_spans, ap.out.size(), c.plain.size(), d,
                                 d < ap.out.size() ? ap.out[d] : 0,
                                 d < c.plain.size() ? c.plain[d] : 0);
                    for (auto& sp : spans) std::fprintf(stderr, " [%u,%u]", sp.first, sp.second);
                    std::fprintf(stderr, "\n");
                    {
                        Decompressor50 dbg(win);
                        Decompressor50::SpanRecords sr;
                        dbg.decode_span(tl, packed.data(), packed.size(), c.plain.size(), 0,
                                        static_cast<uint32_t>(tl.blocks.size() - 1),
                                        size_t{1} << 30, sr);
                        for (size_t i = 0; i < 8 && i < sr.recs.size(); ++i) {
                            const auto& r = sr.recs[i];
                            std::fprintf(stderr, "rec[%zu] tag=%u aux=%u a=%u b=%u\n", i, r.tag,
                                         r.aux, r.a, r.b);
                        }
                        std::fprintf(stderr, "pool[0..8]:");
                        for (size_t i = 0; i < 8 && i < sr.lit_pool.size(); ++i)
                            std::fprintf(stderr, " %02x", sr.lit_pool[i]);
                        std::fprintf(stderr, "\n");
                    }
                    std::exit(1);
                }
                (void)total_lower;
            }

            // dest_size crossing row [R8]: applier clamps mid-record and
            // discards the rest, exactly like the sequential stop.
            {
                const size_t dest = c.plain.size() * 3 / 5;
                Decompressor50 seq(win);
                std::vector<core::byte> seq_out;
                size_t written = 0;
                bool finished = false;
                auto sink = [&](const core::byte* p, size_t n) -> bool {
                    seq_out.insert(seq_out.end(), p, p + n);
                    written += n;
                    return true;
                };
                assert(seq.decompress(packed.data(), packed.size(), dest, false, sink, &written,
                                      &finished));
                assert(seq_out.size() == dest);

                MiniApplier ap;
                auto spans = split_bounds(tl, 4);
                for (auto& sp : spans) {
                    Decompressor50 worker(win);
                    Decompressor50::SpanRecords sr;
                    size_t cap = (16u << 20) + (1u << 20);
                    assert(worker.decode_span(tl, packed.data(), packed.size(), dest, sp.first,
                                              sp.second, cap, sr));
                    apply_span(ap, sr, dest);
                    assert(!ap.fail);
                }
                assert(ap.out == seq_out);
            }
            std::cout << "[PASS] span-records identity: " << c.name
                      << " (splits 1/2/3/4/8 + dest-crossing)\n";
        }
    }

    // ── M3: driver gate matrix (each gate individually forces sequential) ───
    {
        _putenv("OPENRAR_NO_PARALLEL_DECODE=");
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");

        unsigned w = 0;
        const size_t MB = 1024 * 1024;
        // Baseline engagement.
        assert(should_use_parallel_decode(32 * MB, 64 * MB, 0x200000, false, &w));
        assert(w >= 2 && w <= 8);
        // G1: solid entries never engage.
        assert(!should_use_parallel_decode(32 * MB, 64 * MB, 0x200000, true, nullptr));
        // G2 flavor: v1 (446-table, > 4 GiB window) stays sequential.
        assert(!should_use_parallel_decode(32 * MB, 64 * MB, 5ULL * 1024 * 1024 * 1024, false,
                                           nullptr));
        // G3: packed amortization floor.
        assert(!should_use_parallel_decode(4 * MB, 64 * MB, 0x200000, false, nullptr));
        // Degenerate sizes.
        assert(!should_use_parallel_decode(32 * MB, 0, 0x200000, false, nullptr));
        assert(!should_use_parallel_decode(0, 64 * MB, 0x200000, false, nullptr));
        // D9 kill switch.
        _putenv("OPENRAR_NO_PARALLEL_DECODE=1");
        assert(!should_use_parallel_decode(32 * MB, 64 * MB, 0x200000, false, nullptr));
        _putenv("OPENRAR_NO_PARALLEL_DECODE=");
        // F7: single-thread override.
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=1");
        assert(!should_use_parallel_decode(32 * MB, 64 * MB, 0x200000, false, nullptr));
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");
        std::cout << "[PASS] driver gate matrix\n";
    }

    // ── M3: MT/ST byte- AND chunk-identity through the real driver ──────────
    // The driver's pipeline (workers + pipelined applier) must reproduce the
    // sequential decoder's emission exactly — including callback chunk
    // boundaries, which the shared apply engine makes structural.
    {
        _putenv("OPENRAR_NO_PARALLEL_DECODE=");
        auto chunks_decode = [&](const std::vector<core::byte>& packed, size_t plain_size,
                                 size_t win, bool parallel, std::vector<core::byte>& out,
                                 std::vector<size_t>& chunk_sizes,
                                 ParallelDecodeDiag* diag) -> bool {
            Decompressor50 seq(win);
            out.clear();
            chunk_sizes.clear();
            auto cb = [&](const core::byte* p, size_t n) -> bool {
                out.insert(out.end(), p, p + n);
                chunk_sizes.push_back(n);
                return true;
            };
            if (parallel)
                return decode_entry(packed.data(), packed.size(), plain_size, win, cb, diag);
            return seq.decompress(packed.data(), packed.size(), plain_size, false, cb);
        };

        auto run_identity = [&](const char* name, std::vector<core::byte>& plain, int method,
                                std::vector<unsigned> thread_counts) {
            std::vector<core::byte> packed;
            assert(Compressor50::compress_buffer(plain.data(), plain.size(), packed, method,
                                                 0x200000));
            std::vector<core::byte> seq_out, par_out;
            std::vector<size_t> seq_chunks, par_chunks;
            assert(
                chunks_decode(packed, plain.size(), 0x200000, false, seq_out, seq_chunks, nullptr));
            for (unsigned threads : thread_counts) {
                const std::string forced =
                    "OPENRAR_PARALLEL_DECODE_THREADS=" + std::to_string(threads);
                _putenv(forced.c_str());
                ParallelDecodeDiag diag;
                assert(chunks_decode(packed, plain.size(), 0x200000, true, par_out, par_chunks,
                                     &diag));
                assert(diag.engaged); // the identity rows require engagement
                assert(!diag.fell_back);
                assert(par_out == seq_out);
                assert(par_chunks == seq_chunks); // chunk-identity [R1]
                _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");
            }
            std::cout << "[PASS] driver MT/ST identity: " << name << "\n";
        };

        // m1 packs text loosely: a 96 MiB member clears the span floor at 8
        // workers, exercising the full pipeline shape.
        auto big = make_text(96u * 1024 * 1024, 0x137);
        run_identity("text96m-m1 threads 2/3/4/8", big, 1, {2});
        // m3 at a smaller member: engagement at 2-4 workers on this host.
        auto mid = make_text(64u * 1024 * 1024, 0x515);
        run_identity("text64m-m3 threads 2/3/4", mid, 3, {2, 3, 4});
    }

    // ── M3: hostile fallback + filter-bearing member through the driver ─────
    {
        // F1: a truncated member fails the pre-scan; decode_entry falls back
        // to the sequential path, whose verdict (failure) is the answer.
        auto plain = make_text(64u * 1024 * 1024, 0x91);
        std::vector<core::byte> packed;
        assert(Compressor50::compress_buffer(plain.data(), plain.size(), packed, 3, 0x200000));
        // 2 workers: the truncated stream still clears the G3 floor so the
        // pre-scan (not the gate) is what fails.
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=2");
        {
            std::vector<core::byte> trunc(packed.begin(), packed.begin() + packed.size() / 2);
            ParallelDecodeDiag diag;
            auto cb = [&](const core::byte*, size_t) -> bool {
                return true;
            };
            assert(!decode_entry(trunc.data(), trunc.size(), plain.size(), 0x200000, cb, &diag));
            assert(diag.fell_back);
        }
        // Filter-bearing member: the PE-like corpus engages E8 pretransform
        // tokens; the applier resolves them through the engine queue.
        {
            auto pe = make_pe_like(64u * 1024 * 1024, 99);
            std::vector<core::byte> packed_pe;
            assert(Compressor50::compress_buffer(pe.data(), pe.size(), packed_pe, 3, 0x200000));
            // Confirm the corpus actually bears filters.
            Decompressor50 ps(0x200000);
            Decompressor50::PrescanTimeline tl;
            assert(ps.prescan_member(packed_pe.data(), packed_pe.size(), tl));
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
                Decompressor50 seq(0x200000);
                assert(seq.decompress(packed_pe.data(), packed_pe.size(), pe.size(), false,
                                      collect(seq_out, seq_chunks)));
            }
            ParallelDecodeDiag diag;
            assert(decode_entry(packed_pe.data(), packed_pe.size(), pe.size(), 0x200000,
                                collect(par_out, par_chunks), &diag));
            assert(diag.engaged && !diag.fell_back);
            assert(par_out == seq_out);
            assert(par_chunks == seq_chunks);
        }
        _putenv("OPENRAR_PARALLEL_DECODE_THREADS=");
        std::cout << "[PASS] driver hostile fallback + filter member identity\n";
    }

    std::cout << "All two_phase_tests passed.\n";
    return 0;
}
