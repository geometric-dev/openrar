// v1.38.0 Gate 0 probe: decode-kernel variant A/B + rebuild-trap reproduction
// (docs/v1.38.0-pre-analysis.md; measurement protocol v1.34 §3).
//
// Measures, on real packed streams produced by the in-repo encoder:
//   1. Kernel composition: real symbol-stage time (decode_span), replica
//      token-loop time (no records), full sequential decode time.
//   2. Rebuild trap: per-table-set build cost of the shipped HuffmanDecoder
//      (initialize_from_description) vs a naive full 15-bit flat table
//      (4 x 32768 entries) vs lazy-page flat tables (PB in 10..13).
//   3. Decode A/B: token loops identical to the symbol stage except the
//      Huffman lookup — current quick/canonical vs fused peek15 vs
//      lazy-page flat (PB sweep) vs full flat. Every variant must reproduce
//      the REAL kernel's token-stream hash (decode_span records) exactly,
//      including on hostile sparse tables — the stage-1 byte-identity
//      argument, verified before any implementation.
//
// Semantics contract mirrored by every variant: a peek value covered by a
// code resolves to that code's (len, symbol); an uncovered peek value runs
// the shipped canonical walk (bits=15 fallback, pos>=max_num => decode_num[0]).
// Over-subscribed codes (first-code + index overflowing the length class)
// are treated as never-allocated: their slots stay uncovered, exactly what
// the shipped canonical walk resolves them to. No variant writes a slot
// outside its table for any input lengths.
//
// Probe exes print through unbuffered stdout (environment quirk rule).
#include "../src/compress/compressor50.hpp"
#include "../src/compress/decompressor50.hpp"

#include <algorithm>
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

std::vector<core::byte> make_zeros(size_t n, uint32_t) {
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

#include "decode_kernel_probe.inc"

// ── Token loop (mirrors decode_span's symbol pass, no records) ──────────────
// Variant interface: build(const Canon&, const core::byte* lens, size_t nsym)
// (VQuick/VFused ignore lens), decode(PR&), optional pages_materialized_ /
// page_writes_ counters.
template <class D>
core::uint64 token_loop(const core::byte* src, size_t src_size,
                        const Decompressor50::PrescanTimeline& tl, D& ld, D& dd, D& ldd, D& rd,
                        size_t* builds, size_t* pages_mat, size_t* page_writes) {
    core::uint64 h = 1469598103934665603ull;
    auto mix = [&](core::uint64 x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    Canon cl, cd, cl16, cr;
    for (uint32_t bi = 0; bi < tl.blocks.size(); ++bi) {
        const auto& expect = tl.blocks[bi];
        if (expect.src_byte >= src_size) break;
        PR r;
        r.init(src + expect.src_byte, src_size - expect.src_byte);
        {
            size_t tb = expect.token_bits;
            while (r.consumed_ < tb) {
                size_t rem = tb - r.consumed_;
                r.consume(static_cast<unsigned>(rem > 57 ? 57 : rem));
            }
        }
        if (expect.table_present && expect.block_size != 0) {
            const auto& d = tl.descriptions[expect.desc_id];
            const size_t cur_dc = d.extra_dist ? 80 : 64;
            cl = canon_build(d.lengths.data(), 306);
            cd = canon_build(d.lengths.data() + 306, cur_dc);
            cl16 = canon_build(d.lengths.data() + 306 + cur_dc, 16);
            cr = canon_build(d.lengths.data() + 306 + cur_dc + 16, 44);
            ld.build(cl, d.lengths.data(), 306);
            dd.build(cd, d.lengths.data() + 306, cur_dc);
            ldd.build(cl16, d.lengths.data() + 306 + cur_dc, 16);
            rd.build(cr, d.lengths.data() + 306 + cur_dc + 16, 44);
            ++*builds;
            if (pages_mat)
                *pages_mat += ld.pages_materialized_ + dd.pages_materialized_ +
                              ldd.pages_materialized_ + rd.pages_materialized_;
            if (page_writes)
                *page_writes +=
                    ld.page_writes_ + dd.page_writes_ + ldd.page_writes_ + rd.page_writes_;
        } else if (bi == 0) {
            return 0; // no tables in force; skip member
        }
        size_t end_bit; // block-reader-relative (consumed_ is relative)
        if (expect.block_size == 0) {
            end_bit = 0;
        } else {
            end_bit = static_cast<size_t>(expect.header_len + expect.block_size - 1) * 8 +
                      expect.block_bit_size;
        }
        while (r.consumed_ < end_bit && r.bits_remaining() >= 1) {
            core::uint32 slot = ld.decode(r);
            if (slot < 256) {
                mix(slot);
                continue;
            }
            if (slot == 256) {
                bool ok = true;
                auto rd_field = [&](core::uint32& out) {
                    if (!ok) return;
                    if (r.bits_remaining() < 2) {
                        ok = false;
                        return;
                    }
                    core::uint32 bc = r.get(2) + 1;
                    core::uint32 val = 0;
                    for (core::uint32 i = 0; i < bc; ++i) {
                        if (r.bits_remaining() < 8) {
                            ok = false;
                            return;
                        }
                        val |= (r.get(8) << (i * 8));
                    }
                    out = val;
                };
                core::uint32 fs = 0, fl = 0;
                rd_field(fs);
                rd_field(fl);
                if (!ok) return h;
                if (fl > 0x400000) fl = 0;
                if (r.bits_remaining() < 3) return h;
                core::uint32 ft = r.get(3);
                core::uint32 fc = 1;
                if (ft == 0) {
                    if (r.bits_remaining() < 5) return h;
                    fc = r.get(5) + 1;
                }
                mix(0x1000000ull | (core::uint64(fs) << 32) | (core::uint64(fl) << 8) |
                    (core::uint64(ft) << 3) | (fc - 1));
                continue;
            }
            if (slot == 257) {
                mix(0x2000000ull);
                continue;
            }
            if (slot >= 262) {
                core::uint32 len = slot_to_length(r, slot - 262);
                if (r.bits_remaining() < 1) return h;
                core::uint32 dist_slot = dd.decode(r);
                size_t distance = 1;
                core::uint32 d_bits = 0;
                if (dist_slot < 4) {
                    distance += dist_slot;
                } else {
                    d_bits = dist_slot / 2 - 1;
                    distance += static_cast<size_t>(2 | (dist_slot & 1)) << d_bits;
                }
                if (d_bits > 0) {
                    if (d_bits >= 4) {
                        if (d_bits > 4) {
                            if (r.bits_remaining() < d_bits - 4) return h;
                            core::uint64 extra =
                                (d_bits > 36) ? r.get64(d_bits - 4) : r.get(d_bits - 4);
                            distance += static_cast<size_t>(extra) << 4;
                        }
                        if (r.bits_remaining() < 1) return h;
                        core::uint32 low = ldd.decode(r);
                        distance += low;
                    } else {
                        if (r.bits_remaining() < d_bits) return h;
                        distance += r.get(d_bits);
                    }
                }
                if (distance > 0x100) {
                    ++len;
                    if (distance > 0x2000) {
                        ++len;
                        if (distance > 0x40000) ++len;
                    }
                }
                mix(0x3000000ull | (core::uint64(distance) << 16) | len);
                continue;
            }
            // slots 258..261
            if (r.bits_remaining() < 1) return h;
            core::uint32 len_slot = rd.decode(r);
            core::uint32 len = slot_to_length(r, len_slot);
            mix(0x4000000ull | (core::uint64(slot - 258) << 16) | len);
        }
    }
    return h;
}

// Real-kernel anchor: hash of the decode_span records for the whole member.
core::uint64 real_anchor(size_t win, const core::byte* src, size_t src_size,
                         const Decompressor50::PrescanTimeline& tl, size_t dest) {
    Decompressor50 d(win);
    Decompressor50::SpanRecords sr;
    if (!d.decode_span(tl, src, src_size, dest, 0, static_cast<uint32_t>(tl.blocks.size() - 1),
                       size_t{1} << 31, sr)) {
        return 0;
    }
    core::uint64 h = 1469598103934665603ull;
    auto mix = [&](core::uint64 x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    for (const auto& r : sr.recs) {
        auto tag = static_cast<Decompressor50::OpRecord::Tag>(r.tag);
        switch (tag) {
        case Decompressor50::OpRecord::Tag::Lit:
            for (core::uint32 i = 0; i < r.aux; ++i) mix(sr.lit_pool[r.b + i]);
            break;
        case Decompressor50::OpRecord::Tag::Match:
            mix(0x3000000ull | (core::uint64(r.a) << 16) | r.b);
            break;
        case Decompressor50::OpRecord::Tag::Rep:
            mix(0x4000000ull | (core::uint64(r.aux) << 16) | r.a);
            break;
        case Decompressor50::OpRecord::Tag::R257:
            mix(0x2000000ull);
            break;
        case Decompressor50::OpRecord::Tag::Filter:
            mix(0x1000000ull | (core::uint64(r.a) << 32) | (core::uint64(r.b) << 8) |
                (core::uint64((r.aux & 7)) << 3) | ((r.aux >> 3) & 31));
            break;
        }
    }
    return h;
}

struct Row {
    const char* name;
    double ms{0};
    core::uint64 hash{0};
    size_t builds{0};
    size_t pages_mat{0};
    size_t page_writes{0};
};

struct CorpusResult {
    const char* name;
    int method;
    size_t plain_size{0};
    size_t packed_size{0};
    size_t blocks{0};
    size_t table_blocks{0};
    size_t desc_count{0};
    core::uint64 anchor{0};
    double real_symbol_ms{0}; // decode_span whole member (the shipped kernel)
    double seq_ms{0};         // full sequential decompress
    std::vector<Row> rows;
};

CorpusResult run_corpus(const char* name, int method, const std::vector<core::byte>& data,
                        size_t win) {
    CorpusResult R;
    R.name = name;
    R.method = method;
    R.plain_size = data.size();
    std::vector<core::byte> packed;
    if (!Compressor50::compress_buffer(data.data(), data.size(), packed, method, win)) {
        std::printf("%s m%d: COMPRESS FAILED\n", name, method);
        return R;
    }
    R.packed_size = packed.size();
    Decompressor50 ps(win);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(packed.data(), packed.size(), tl)) {
        std::printf("%s m%d: PRESCAN FAILED\n", name, method);
        return R;
    }
    R.blocks = tl.blocks.size();
    for (const auto& b : tl.blocks)
        if (b.table_present && b.block_size != 0) ++R.table_blocks;
    R.desc_count = tl.descriptions.size();

    R.anchor = real_anchor(win, packed.data(), packed.size(), tl, data.size());
    R.real_symbol_ms = min_time_ms(
        [&] {
            Decompressor50 w(win);
            Decompressor50::SpanRecords sr;
            if (!w.decode_span(tl, packed.data(), packed.size(), data.size(), 0,
                               static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31, sr)) {
                std::printf("  decode_span FAILED\n");
            }
        },
        7);
    R.seq_ms = min_time_ms(
        [&] {
            Decompressor50 w(win);
            std::vector<core::byte> out;
            out.reserve(data.size());
            if (!w.decompress(packed.data(), packed.size(), data.size(), false,
                              [&](const core::byte* p, size_t n) {
                                  out.insert(out.end(), p, p + n);
                                  return true;
                              })) {
                std::printf("  decompress FAILED\n");
            }
        },
        5);

    auto run_row = [&](const char* nm, auto make_variants) {
        Row row;
        row.name = nm;
        auto [l, d2, l16, r2] = make_variants();
        size_t builds = 0, pm = 0, pw = 0;
        row.ms = min_time_ms(
            [&] {
                builds = pm = pw = 0;
                row.hash = token_loop(packed.data(), packed.size(), tl, *l, *d2, *l16, *r2, &builds,
                                      &pm, &pw);
            },
            7);
        row.builds = builds;
        row.pages_mat = pm;
        row.page_writes = pw;
        R.rows.push_back(row);
    };

    run_row("VQuick(replica)",
            [] { return std::make_tuple(new VQuick, new VQuick, new VQuick, new VQuick); });
    run_row("VFused(peek15)",
            [] { return std::make_tuple(new VFused, new VFused, new VFused, new VFused); });
    for (unsigned pb : {10u, 11u, 12u, 13u}) {
        char* nm = new char[32];
        std::snprintf(nm, 32, "VFlatLazy(PB=%u)", pb);
        run_row(nm, [pb] {
            auto mk = [pb] {
                auto p = new VFlatLazy;
                p->configure(pb);
                return p;
            };
            return std::make_tuple(mk(), mk(), mk(), mk());
        });
    }
    run_row("VFullFlat(15-bit)", [] {
        return std::make_tuple(new VFullFlat, new VFullFlat, new VFullFlat, new VFullFlat);
    });

    // build() adapters for VQuick/VFused (lengths unused — tables come from
    // the Canon arrays alone).
    return R;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== v1.38.0 Gate 0 decode-kernel probe ===\n");
    const size_t WIN = 2 * 1024 * 1024;
    const size_t N = 16 * 1024 * 1024;

    struct Spec {
        const char* name;
        int method;
        std::vector<core::byte> (*gen)(size_t, uint32_t);
        uint32_t seed;
    };
    const Spec specs[] = {
        {"text", 3, make_text, 0x137},      {"text", 1, make_text, 0x137},
        {"exelike", 3, make_exelike, 0x55}, {"zeros", 3, make_zeros, 0},
        {"random", 3, make_random, 7},
    };

    for (const auto& sp : specs) {
        auto data = sp.gen(N, sp.seed);
        auto R = run_corpus(sp.name, sp.method, data, WIN);
        std::printf("\n--- %s m%d: plain %.1f MB packed %.1f MB | blocks %zu (table %zu, desc %zu)"
                    "\n",
                    R.name, R.method, R.plain_size / 1048576.0, R.packed_size / 1048576.0, R.blocks,
                    R.table_blocks, R.desc_count);
        std::printf("    anchor=%016llx real_symbol=%.2fms seq=%.2fms\n",
                    (unsigned long long)R.anchor, R.real_symbol_ms, R.seq_ms);
        for (const auto& row : R.rows) {
            std::printf("    %-18s %8.2fms hash=%016llx %s builds=%zu pages=%zu pw=%zu\n", row.name,
                        row.ms, (unsigned long long)row.hash,
                        row.hash == R.anchor ? "OK" : "HASH-MISMATCH", row.builds, row.pages_mat,
                        row.page_writes);
        }
    }

    // ── Part 1b: VFlatLazy self-check — resolve all 32768 peek values
    // through the prim/page structure (no reader) and against the real
    // kernel's semantics (VQuick quick table + the shipped slow path).
    std::printf("\n=== VFlatLazy self-check (table resolution vs real semantics) ===\n");
    {
        auto data = make_text(N, 0x137);
        std::vector<core::byte> packed;
        if (Compressor50::compress_buffer(data.data(), data.size(), packed, 3, WIN)) {
            Decompressor50 ps(WIN);
            Decompressor50::PrescanTimeline tl;
            if (ps.prescan_member(packed.data(), packed.size(), tl)) {
                for (int di = 0; di < (int)tl.descriptions.size() && di < 3; ++di) {
                    const auto& d = tl.descriptions[di];
                    const size_t cur_dc = d.extra_dist ? 80 : 64;
                    struct Sub {
                        const char* name;
                        size_t off, cnt;
                    };
                    const Sub subs[] = {{"ld", 0, 306},
                                        {"dd", 306, cur_dc},
                                        {"ldd", 306 + cur_dc, 16},
                                        {"rd", 306 + cur_dc + 16, 44}};
                    for (const auto& sub : subs) {
                        Canon c = canon_build(d.lengths.data() + sub.off, sub.cnt);
                        VQuick ref;
                        ref.build(c);
                        for (unsigned pb : {10u, 11u, 12u, 13u}) {
                            VFlatLazy f;
                            f.configure(pb);
                            f.build_lens(c, d.lengths.data() + sub.off, sub.cnt);
                            size_t bad = 0;
                            core::uint32 first_bad_v = 0;
                            for (core::uint32 v = 0; v < 32768; ++v) {
                                // reference resolution (real kernel semantics)
                                core::uint32 e = ref.quick[v >> 5];
                                core::uint32 rlen, rsym;
                                if ((e >> 16) != 0) {
                                    rlen = e >> 16;
                                    rsym = e & 0xFFFF;
                                } else {
                                    core::uint32 bitfield = (v << 1) & 0xFFFE;
                                    unsigned int bits = 15;
                                    for (unsigned int i = 11; i < 15; ++i) {
                                        if (bitfield < c.decode_len[i]) {
                                            bits = i;
                                            break;
                                        }
                                    }
                                    rlen = bits;
                                    core::uint32 dist = bitfield - c.decode_len[bits - 1];
                                    dist >>= (16 - bits);
                                    core::uint32 pos = c.decode_pos[bits] + dist;
                                    if (pos >= c.max_num) pos = 0;
                                    rsym = c.decode_num[pos];
                                }
                                // flat resolution without a reader: emulate
                                core::uint32 flen, fsym;
                                core::uint32 pfx = v >> f.REST_;
                                core::uint32 fe = f.prim[pfx];
                                if (fe != 0xFFFFFFFFu) {
                                    flen = fe >> 16;
                                    fsym = fe & 0xFFFF;
                                    if (flen == 0) { // uncovered -> slow
                                        flen = rlen;
                                        fsym = rsym;
                                    }
                                } else {
                                    // page slot (materialize without counters)
                                    if (f.page_id[pfx] == 0xFFFFFFFFu) f.materialize(pfx);
                                    core::uint32 slot =
                                        f.page_id[pfx] * f.page_size_ + (v & (f.page_size_ - 1));
                                    if (f.page_len[slot] != 0) {
                                        flen = f.page_len[slot];
                                        fsym = f.pages[slot] & 0xFFFF;
                                    } else {
                                        flen = rlen;
                                        fsym = rsym;
                                    }
                                }
                                if (flen != rlen || fsym != rsym) {
                                    if (bad == 0) {
                                        first_bad_v = v;
                                        std::printf(
                                            "    first bad v=%u: ref len=%u sym=%u | flat len=%u "
                                            "sym=%u | quick0=%08x\n",
                                            v, rlen, rsym, flen, fsym, ref.quick[0]);
                                    }
                                    ++bad;
                                }
                            }
                            if (bad)
                                std::printf("  desc%d %s PB=%u: %zu MISMATCHES (first v=%u "
                                            "prim=%08x)\n",
                                            di, sub.name, pb, bad, first_bad_v,
                                            f.prim[first_bad_v >> f.REST_]);
                        }
                    }
                }
                std::printf("  self-check done\n");
            }
        }
    }

    // ── Part 2: build-cost bench (the rebuild trap) ────────────────────────
    std::printf("\n=== build cost per table-set rebuild (real descriptions, text m3) ===\n");
    {
        auto data = make_text(N, 0x137);
        std::vector<core::byte> packed;
        if (Compressor50::compress_buffer(data.data(), data.size(), packed, 3, WIN)) {
            Decompressor50 ps(WIN);
            Decompressor50::PrescanTimeline tl;
            if (ps.prescan_member(packed.data(), packed.size(), tl) && !tl.descriptions.empty()) {
                const auto& desc = tl.descriptions[0];
                const size_t cur_dc = desc.extra_dist ? 80 : 64;
                double t_real = min_time_ms(
                    [&] {
                        Decompressor50 w(WIN);
                        w.initialize_from_description(desc);
                    },
                    2001);
                std::printf("  shipped initialize_from_description : %8.2f us/rebuild\n",
                            t_real * 1000.0);

                Canon cl = canon_build(desc.lengths.data(), 306);
                Canon cd = canon_build(desc.lengths.data() + 306, cur_dc);
                Canon cl16 = canon_build(desc.lengths.data() + 306 + cur_dc, 16);
                Canon cr = canon_build(desc.lengths.data() + 306 + cur_dc + 16, 44);
                double t_canon = min_time_ms(
                    [&] {
                        Canon a = canon_build(desc.lengths.data(), 306);
                        Canon b = canon_build(desc.lengths.data() + 306, cur_dc);
                        Canon c = canon_build(desc.lengths.data() + 306 + cur_dc, 16);
                        Canon d = canon_build(desc.lengths.data() + 306 + cur_dc + 16, 44);
                        (void)a;
                        (void)b;
                        (void)c;
                        (void)d;
                    },
                    2001);
                std::printf("  canonical arrays only (O(symbols))   : %8.2f us/rebuild\n",
                            t_canon * 1000.0);

                double t_flat = min_time_ms(
                    [&] {
                        VFullFlat a, b, c, d;
                        a.build_lens(cl, desc.lengths.data(), 306);
                        b.build_lens(cd, desc.lengths.data() + 306, cur_dc);
                        c.build_lens(cl16, desc.lengths.data() + 306 + cur_dc, 16);
                        d.build_lens(cr, desc.lengths.data() + 306 + cur_dc + 16, 44);
                    },
                    501);
                std::printf("  full-flat 4x(32768 span fill)        : %8.2f us/rebuild\n",
                            t_flat * 1000.0);

                for (unsigned pb : {10u, 11u, 12u, 13u}) {
                    double t_lazy = min_time_ms(
                        [&] {
                            VFlatLazy a, b, c, d;
                            a.configure(pb);
                            b.configure(pb);
                            c.configure(pb);
                            d.configure(pb);
                            a.build_lens(cl, desc.lengths.data(), 306);
                            b.build_lens(cd, desc.lengths.data() + 306, cur_dc);
                            c.build_lens(cl16, desc.lengths.data() + 306 + cur_dc, 16);
                            d.build_lens(cr, desc.lengths.data() + 306 + cur_dc + 16, 44);
                        },
                        501);
                    std::printf("  lazy-page PB=%u (primary fill only)  : %8.2f us/rebuild\n", pb,
                                t_lazy * 1000.0);
                }

                // Hostile sparse table: 2 one-bit codes + uncovered remainder.
                core::byte sparse[430];
                std::memset(sparse, 0, sizeof(sparse));
                sparse[0] = 1;
                sparse[1] = 1;
                Canon cs = canon_build(sparse, 306);
                double t_sparse_real = min_time_ms(
                    [&] {
                        Decompressor50 w(WIN);
                        Decompressor50::PrescanDescription d;
                        d.lengths.assign(sparse, sparse + 430);
                        w.initialize_from_description(d);
                    },
                    2001);
                std::printf("  hostile sparse table, shipped path   : %8.2f us/rebuild\n",
                            t_sparse_real * 1000.0);
                for (unsigned pb : {10u, 12u}) {
                    double t_sparse = min_time_ms(
                        [&] {
                            VFlatLazy a;
                            a.configure(pb);
                            a.build_lens(cs, sparse, 306);
                            PR r;
                            core::byte bits[64];
                            std::memset(bits, 0xFF, sizeof(bits));
                            r.init(bits, sizeof(bits));
                            volatile core::uint32 sink = 0;
                            for (int i = 0; i < 64; ++i) sink ^= a.decode(r);
                            (void)sink;
                        },
                        501);
                    std::printf("  hostile sparse, lazy PB=%u +64 decodes: %6.2f us\n", pb,
                                t_sparse * 1000.0);
                }
            }
        }
    }

    // ── Part 3: record-emission attribution + slimming (phase-1 lever) ──────
    std::printf("\n=== record emission (decode_span vs replica vs slim) ===\n");
    {
        auto data = make_text(N, 0x137);
        std::vector<core::byte> packed;
        if (Compressor50::compress_buffer(data.data(), data.size(), packed, 3, WIN)) {
            Decompressor50 ps(WIN);
            Decompressor50::PrescanTimeline tl;
            if (ps.prescan_member(packed.data(), packed.size(), tl)) {
                // (a) decode_span as shipped (records + cap checks)
                double t_span = min_time_ms(
                    [&] {
                        Decompressor50 w(WIN);
                        Decompressor50::SpanRecords sr;
                        w.decode_span(tl, packed.data(), packed.size(), data.size(), 0,
                                      static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31,
                                      sr);
                    },
                    7);
                // (b) decode_span with warm record buffers (the pipeline reuses
                // SpanRecords across spans, so this isolates growth/allocation)
                double t_span_warm = min_time_ms(
                    [&] {
                        Decompressor50 w(WIN);
                        Decompressor50::SpanRecords sr;
                        sr.recs.reserve(size_t{1} << 21);
                        sr.lit_pool.reserve(size_t{1} << 23);
                        w.decode_span(tl, packed.data(), packed.size(), data.size(), 0,
                                      static_cast<uint32_t>(tl.blocks.size() - 1), size_t{1} << 31,
                                      sr);
                    },
                    7);
                std::printf("  decode_span shipped        : %8.2f ms\n", t_span);
                std::printf("  decode_span warm buffers   : %8.2f ms\n", t_span_warm);
                std::printf("  replica (no records)       : %8.2f ms\n", [&] {
                    VQuick l, d2, l16, r2;
                    size_t builds = 0, pm = 0, pw = 0;
                    return min_time_ms(
                        [&] {
                            token_loop(packed.data(), packed.size(), tl, l, d2, l16, r2, &builds,
                                       &pm, &pw);
                        },
                        7);
                }());
                // (c) micro: per-byte push_back vs bulk insert for the lit pool
                const size_t nlit = 2 * 1000 * 1000;
                double t_push = min_time_ms(
                    [&] {
                        std::vector<core::byte> pool;
                        pool.reserve(nlit + 16);
                        for (size_t i = 0; i < nlit; ++i)
                            pool.push_back(static_cast<core::byte>(i));
                    },
                    25);
                double t_bulk = min_time_ms(
                    [&] {
                        std::vector<core::byte> pool;
                        pool.reserve(nlit + 16);
                        core::byte run[255];
                        for (size_t i = 0; i < nlit;) {
                            size_t n = std::min<size_t>(255, nlit - i);
                            for (size_t j = 0; j < n; ++j) run[j] = static_cast<core::byte>(i + j);
                            pool.insert(pool.end(), run, run + n);
                            i += n;
                        }
                    },
                    25);
                std::printf("  lit pool %uB: push_back %8.3f ms | stack-run+insert %8.3f ms\n",
                            (unsigned)nlit, t_push, t_bulk);
            }
        }
    }

    // ── Part 4: filter-region extraction + delta kernel A/B ────────────────
    std::printf("\n=== apply-stage kernels ===\n");
    {
        // (a) per-byte circular region extraction vs two-memcpy (the flush
        // path's filter-region read on a 1 MiB region, 2 MiB window)
        {
            const size_t W = 2 * 1024 * 1024;
            std::vector<core::byte> win(W);
            for (size_t i = 0; i < W; ++i) win[i] = static_cast<core::byte>(i * 7);
            const size_t LEN = 1024 * 1024;
            const size_t circ = 1234567; // region start in the ring
            std::vector<core::byte> out1(LEN), out2(LEN);
            double t_byte = min_time_ms(
                [&] {
                    size_t cur = circ;
                    for (size_t i = 0; i < LEN; ++i) {
                        out1[i] = win[cur];
                        if (++cur == W) cur = 0;
                    }
                },
                25);
            double t_memcpy = min_time_ms(
                [&] {
                    size_t first = std::min(LEN, W - circ);
                    std::memcpy(out2.data(), win.data() + circ, first);
                    std::memcpy(out2.data() + first, win.data(), LEN - first);
                },
                25);
            bool eq = std::memcmp(out1.data(), out2.data(), LEN) == 0;
            std::printf("  region extract 1MiB: per-byte %8.3f ms | two-memcpy %8.3f ms | %s\n",
                        t_byte, t_memcpy, eq ? "OK" : "MISMATCH");
            std::printf("    (the memcpy variant MEASURED SLOWER end-to-end: +16%% exelike\n"
                        "     sequential - the micro-bench verdict reverses in the integrated\n"
                        "     loop; declined, pre-analysis section 6 - do not re-apply)\n");
        }
        // (b) delta filter: shipped scalar vs SSE2/AVX2 row-subtract
        {
            const size_t LEN = 1024 * 1024;
            const core::uint8 ch = 4;
            std::vector<core::byte> src(LEN);
            std::mt19937 rng(9);
            for (auto& b : src) b = static_cast<core::byte>(rng());
            std::vector<core::byte> out_a(LEN), out_b(LEN);
            double t_scalar =
                min_time_ms([&] { Filters50::apply_delta(src.data(), out_a.data(), LEN, ch); }, 25);
            // probe-local vectorized delta: rows of ch bytes; per-row wrap-sub
            auto delta_vec = [&](core::byte* d, const core::byte* sp, size_t n, core::uint8 c) {
                size_t rows = n / c;
                for (size_t r = 0; r < rows; ++r) {
                    const core::byte* in = sp + r * c;
                    core::byte* o = d + r * c;
                    if (r == 0) {
                        for (core::uint8 j = 0; j < c; ++j) o[j] = in[j];
                    } else {
                        const core::byte* prev = d + (r - 1) * c;
                        for (core::uint8 j = 0; j < c; ++j)
                            o[j] = static_cast<core::byte>(in[j] - prev[j]);
                    }
                }
                for (size_t i = rows * c; i < n; ++i) {
                    // tail: treat missing channel partners as 0 (matches scalar?
                    // verified by memcmp below)
                    d[i] = sp[i];
                }
            };
            double t_vec = min_time_ms([&] { delta_vec(out_b.data(), src.data(), LEN, ch); }, 25);
            bool eq = std::memcmp(out_a.data(), out_b.data(), LEN) == 0;
            std::printf("  delta ch=%u 1MiB: scalar %8.3f ms | row-wise %8.3f ms | %s\n",
                        (unsigned)ch, t_scalar, t_vec, eq ? "OK" : "MISMATCH");
        }
    }

    std::printf("\n=== done ===\n");
    return 0;
}
