// v1.38.0 Gate 0 debug: full token-by-token diff of the first mismatching
// block between the replica loop and decode_span.
// Diagnostic build: expose the kernel's private decoders for the verbatim
// instrumented clone. STL headers are pre-included so the keyword macroize
// guard (xkeycheck.h) is not re-entered; the define is undone immediately.
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#define private public
#include "../src/compress/compressor50.hpp"
#include "../src/compress/decompressor50.hpp"
#undef private

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

#include "decode_kernel_probe.inc"

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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t WIN = 2 * 1024 * 1024;
    auto data = make_text(2 * 1024 * 1024, 0x137);
    std::vector<core::byte> packed;
    if (!Compressor50::compress_buffer(data.data(), data.size(), packed, 3, WIN)) return 1;
    Decompressor50 ps(WIN);
    Decompressor50::PrescanTimeline tl;
    if (!ps.prescan_member(packed.data(), packed.size(), tl)) return 1;

    for (uint32_t bi = 0; bi < tl.blocks.size(); ++bi) {
        Decompressor50 d(WIN);
        Decompressor50::SpanRecords sr;
        if (!d.decode_span(tl, packed.data(), packed.size(), data.size(), bi, bi, size_t{1} << 30,
                           sr)) {
            std::printf("block %u: decode_span FAILED\n", bi);
            continue;
        }
        std::vector<std::pair<core::uint32, size_t>> trace_bra, trace_vrb;
        std::vector<std::string> real_toks;
        for (const auto& r : sr.recs) {
            auto tag = static_cast<Decompressor50::OpRecord::Tag>(r.tag);
            char buf[64];
            switch (tag) {
            case Decompressor50::OpRecord::Tag::Lit:
                for (core::uint32 i = 0; i < r.aux; ++i) {
                    std::snprintf(buf, sizeof(buf), "L%u", (unsigned)sr.lit_pool[r.b + i]);
                    real_toks.push_back(buf);
                }
                break;
            case Decompressor50::OpRecord::Tag::Match:
                std::snprintf(buf, sizeof(buf), "M d=%u l=%u", r.a, r.b);
                real_toks.push_back(buf);
                break;
            case Decompressor50::OpRecord::Tag::Rep:
                std::snprintf(buf, sizeof(buf), "R%u l=%u", r.aux, r.a);
                real_toks.push_back(buf);
                break;
            case Decompressor50::OpRecord::Tag::R257:
                real_toks.push_back("S257");
                break;
            case Decompressor50::OpRecord::Tag::Filter:
                std::snprintf(buf, sizeof(buf), "F a=%u b=%u aux=%u", r.a, r.b, r.aux);
                real_toks.push_back(buf);
                break;
            }
        }

        std::vector<std::string> rep_toks;
        {
            const auto& expect = tl.blocks[bi];
            PR r;
            r.init(packed.data() + expect.src_byte, packed.size() - expect.src_byte);
            {
                size_t tb = expect.token_bits;
                while (r.consumed_ < tb) {
                    size_t rem = tb - r.consumed_;
                    r.consume(static_cast<unsigned>(rem > 57 ? 57 : rem));
                }
            }
            Canon cl, cd, cl16, cr;
            VQuick ld, dd, ldd, rd;
            if (expect.table_present && expect.block_size != 0) {
                const auto& d = tl.descriptions[expect.desc_id];
                const size_t cur_dc = d.extra_dist ? 80 : 64;
                cl = canon_build(d.lengths.data(), 306);
                cd = canon_build(d.lengths.data() + 306, cur_dc);
                cl16 = canon_build(d.lengths.data() + 306 + cur_dc, 16);
                cr = canon_build(d.lengths.data() + 306 + cur_dc + 16, 44);
                ld.build(cl);
                dd.build(cd);
                ldd.build(cl16);
                rd.build(cr);
            }
            char buf[64];
            size_t end_bit;
            if (expect.block_size == 0) {
                end_bit = 0; // block-reader-relative
            } else {
                end_bit = static_cast<size_t>(expect.header_len + expect.block_size - 1) * 8 +
                          expect.block_bit_size; // relative
            }
            while (r.consumed_ < end_bit && r.bits_remaining() >= 1) {
                core::uint32 slot = ld.decode(r);
                if (slot < 256) {
                    std::snprintf(buf, sizeof(buf), "L%u", slot);
                    rep_toks.push_back(buf);
                } else if (slot == 256) {
                    std::snprintf(buf, sizeof(buf), "F?@%zu", r.consumed_);
                    rep_toks.push_back(buf);
                    break;
                } else if (slot == 257) {
                    rep_toks.push_back("S257");
                } else if (slot >= 262) {
                    core::uint32 len = slot_to_length(r, slot - 262);
                    if (r.bits_remaining() < 1) {
                        rep_toks.push_back("EOF");
                        break;
                    }
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
                                if (r.bits_remaining() < d_bits - 4) {
                                    rep_toks.push_back("EOF");
                                    break;
                                }
                                core::uint64 extra =
                                    (d_bits > 36) ? r.get64(d_bits - 4) : r.get(d_bits - 4);
                                distance += static_cast<size_t>(extra) << 4;
                            }
                            if (r.bits_remaining() < 1) {
                                rep_toks.push_back("EOF");
                                break;
                            }
                            core::uint32 low = ldd.decode(r);
                            distance += low;
                        } else {
                            if (r.bits_remaining() < d_bits) {
                                rep_toks.push_back("EOF");
                                break;
                            }
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
                    std::snprintf(buf, sizeof(buf), "M d=%u l=%u", (unsigned)distance, len);
                    rep_toks.push_back(buf);
                } else {
                    if (r.bits_remaining() < 1) {
                        rep_toks.push_back("EOF");
                        break;
                    }
                    core::uint32 len_slot = rd.decode(r);
                    core::uint32 len = slot_to_length(r, len_slot);
                    std::snprintf(buf, sizeof(buf), "R%u l=%u", slot - 258, len);
                    rep_toks.push_back(buf);
                }
            }
        }

        // Three-way: decode_span count vs PR-clone count vs real-BitReader
        // clone count for the same block.
        {
            // clone with the REAL BitReader
            const auto& expect = tl.blocks[bi];
            BitReader br(packed.data() + expect.src_byte, packed.size() - expect.src_byte);
            {
                size_t tb = expect.token_bits;
                while (br.bit_pos() < tb) {
                    size_t rem = tb - br.bit_pos();
                    br.consume_bits(static_cast<unsigned>(rem > 57 ? 57 : rem));
                }
            }
            Canon cl, cd, cl16, cr;
            VQuick ld, dd, ldd, rd;
            if (expect.table_present && expect.block_size != 0) {
                const auto& d = tl.descriptions[expect.desc_id];
                const size_t cur_dc = d.extra_dist ? 80 : 64;
                cl = canon_build(d.lengths.data(), 306);
                cd = canon_build(d.lengths.data() + 306, cur_dc);
                cl16 = canon_build(d.lengths.data() + 306 + cur_dc, 16);
                cr = canon_build(d.lengths.data() + 306 + cur_dc + 16, 44);
                ld.build(cl);
                dd.build(cd);
                ldd.build(cl16);
                rd.build(cr);
            }
            size_t end_bit = static_cast<size_t>(expect.header_len + expect.block_size - 1) * 8 +
                             expect.block_bit_size; // relative
            struct BrA {
                BitReader& b;
                core::uint32 peek(unsigned c) { return b.peek_bits(c); }
                void consume(unsigned c) { b.consume_bits(c); }
                core::uint32 get(unsigned c) {
                    auto v = b.peek_bits(c);
                    b.consume_bits(c);
                    return v;
                }
                core::uint64 get64(unsigned c) { return b.get_bits64(c); }
            } ba{br};
            size_t tok = 0;
            while (br.bit_pos() < end_bit && br.bits_remaining() >= 1) {
                core::uint32 slot = ld.decode(ba);
                trace_bra.push_back({slot, br.bit_pos()});
                if (slot < 256) {
                    ++tok;
                    continue;
                }
                if (slot == 257) {
                    ++tok;
                    continue;
                }
                if (slot == 256) break;
                if (slot >= 262) {
                    core::uint32 len = slot_to_length(ba, slot - 262);
                    if (br.bits_remaining() < 1) break;
                    core::uint32 ds = dd.decode(ba);
                    size_t dist = 1;
                    core::uint32 db = 0;
                    if (ds < 4)
                        dist += ds;
                    else {
                        db = ds / 2 - 1;
                        dist += static_cast<size_t>(2 | (ds & 1)) << db;
                    }
                    if (db > 0) {
                        if (db >= 4) {
                            if (db > 4) {
                                if (br.bits_remaining() < db - 4) break;
                                dist += static_cast<size_t>(db > 36 ? br.get_bits64(db - 4)
                                                                    : br.get_bits(db - 4))
                                        << 4;
                            }
                            if (br.bits_remaining() < 1) break;
                            dist += ldd.decode(ba);
                        } else {
                            if (br.bits_remaining() < db) break;
                            dist += br.get_bits(db);
                        }
                    }
                    if (dist > 0x100) {
                        ++len;
                        if (dist > 0x2000) {
                            ++len;
                            if (dist > 0x40000) ++len;
                        }
                    }
                    ++tok;
                } else {
                    if (br.bits_remaining() < 1) break;
                    core::uint32 ls = rd.decode(ba);
                    slot_to_length(ba, ls);
                    ++tok;
                }
            }
            std::printf("  clone-real-bitreader tokens=%zu final_bitpos=%zu end_bit=%zu\n", tok,
                        br.bit_pos(), end_bit);
        }

        // Verbatim instrumented copy of decode_span's loop (private->public
        // hack) to find where the real kernel stops.
        {
            Decompressor50 dd2(WIN);
            const auto& expect = tl.blocks[bi];
            dd2.tables_ready_ = false;
            BitReader reader(packed.data() + expect.src_byte, packed.size() - expect.src_byte);
            struct BrB {
                BitReader& b;
                core::uint32 get(unsigned c) {
                    auto v = b.peek_bits(c);
                    b.consume_bits(c);
                    return v;
                }
            } bb{reader};
            Decompressor50::BlockHeader header;
            {
                // read_block_header replicated (private method; access is
                // mangled into MSVC symbol names so the hacked TU cannot
                // call it directly).
                header.header_size = 0;
                bool hok = reader.bits_remaining() >= 16;
                if (hok) {
                    reader.align_byte();
                    core::uint32 flags = reader.get_bits(8);
                    core::uint32 byte_cnt = ((flags >> 3) & 3) + 1;
                    if (byte_cnt == 4) hok = false;
                    if (hok) {
                        header.header_size = static_cast<int>(2 + byte_cnt);
                        header.block_bit_size = static_cast<int>((flags & 7) + 1);
                        if (reader.bits_remaining() < 8)
                            hok = false;
                        else {
                            core::uint32 saved = reader.get_bits(8);
                            core::uint32 block_size = 0;
                            for (core::uint32 i = 0; i < byte_cnt && hok; ++i) {
                                if (reader.bits_remaining() < 8)
                                    hok = false;
                                else
                                    block_size |= reader.get_bits(8) << (i * 8);
                            }
                            core::uint32 chk = (0x5A ^ flags ^ block_size ^ (block_size >> 8) ^
                                                (block_size >> 16)) &
                                               0xFF;
                            if (chk != saved) hok = false;
                            header.block_size = static_cast<int>(block_size);
                            header.block_start = static_cast<int>(reader.byte_pos());
                            header.last_block_in_file = (flags & 0x40) != 0;
                            header.table_present = (flags & 0x80) != 0;
                        }
                    }
                }
                if (!hok) std::printf("  hdr FAIL\n");
            }
            if (expect.table_present && expect.block_size != 0) {
                if (!dd2.initialize_from_description(tl.descriptions[expect.desc_id]))
                    std::printf("  desc FAIL\n");
            }
            while (reader.bit_pos() < expect.token_bits) {
                size_t remain = expect.token_bits - reader.bit_pos();
                reader.consume_bits(static_cast<unsigned>(remain > 57 ? 57 : remain));
            }
            auto block_end_bit = [&](const Decompressor50::BlockHeader& h) -> size_t {
                if (h.block_size <= 0) return static_cast<size_t>(h.block_start) * 8;
                return static_cast<size_t>(h.block_start + h.block_size - 1) * 8 +
                       static_cast<size_t>(h.block_bit_size);
            };
            const size_t end_bit2 = block_end_bit(header);
            size_t tok = 0;
            while (reader.bit_pos() < end_bit2 && reader.bits_remaining() >= 1) {
                core::uint32 slot = dd2.ld_decoder_.decode(reader);
                trace_vrb.push_back({slot, reader.bit_pos()});
                if (slot < 256) {
                    ++tok;
                    continue;
                }
                if (slot == 257) {
                    ++tok;
                    continue;
                }
                if (slot == 256) break;
                if (slot >= 262) {
                    core::uint32 len = slot_to_length(bb, slot - 262);
                    if (reader.bits_remaining() < 1) break;
                    core::uint32 ds = dd2.dd_decoder_.decode(reader);
                    size_t dist = 1;
                    core::uint32 db = 0;
                    if (ds < 4)
                        dist += ds;
                    else {
                        db = ds / 2 - 1;
                        dist += static_cast<size_t>(2 | (ds & 1)) << db;
                    }
                    if (db > 0) {
                        if (db >= 4) {
                            if (db > 4) {
                                if (reader.bits_remaining() < db - 4) break;
                                dist += static_cast<size_t>(dd2.ldd_decoder_.decode(reader)) << 0;
                                dist += static_cast<size_t>(reader.get_bits64(db - 4)) << 4;
                            } else {
                                if (reader.bits_remaining() < 1) break;
                                dist += dd2.ldd_decoder_.decode(reader);
                            }
                        } else {
                            if (reader.bits_remaining() < db) break;
                            dist += reader.get_bits(db);
                        }
                    }
                    if (dist > 0x100) {
                        ++len;
                        if (dist > 0x2000) {
                            ++len;
                            if (dist > 0x40000) ++len;
                        }
                    }
                    ++tok;
                } else {
                    if (reader.bits_remaining() < 1) break;
                    core::uint32 ls = dd2.rd_decoder_.decode(reader);
                    slot_to_length(bb, ls);
                    ++tok;
                }
            }
            std::printf(
                "  verbatim-clone tokens=%zu bitpos=%zu end_bit=%zu hdr(bs=%d hlen=%d bbs=%d)\n",
                tok, reader.bit_pos(), end_bit2, header.block_size, header.header_size,
                header.block_bit_size);
            {
                size_t nn = std::min(trace_bra.size(), trace_vrb.size());
                for (size_t i = 0; i <= nn; ++i) {
                    if (i == nn || trace_bra[i].first != trace_vrb[i].first ||
                        trace_bra[i].second != trace_vrb[i].second) {
                        std::printf("  TRACE-DIVERGE at tok %zu (bra %zu vs vrb %zu)\n", i,
                                    trace_bra.size(), trace_vrb.size());
                        for (size_t j = (i > 3 ? i - 3 : 0); j < std::min(nn, i + 3); ++j)
                            std::printf("    %s bra slot=%u bitpos=%zu | vrb slot=%u bitpos=%zu\n",
                                        j == i ? "X" : " ", trace_bra[j].first, trace_bra[j].second,
                                        trace_vrb[j].first, trace_vrb[j].second);
                        break;
                    }
                }
            }
        }

        size_t n = std::max(real_toks.size(), rep_toks.size());
        size_t first_diff = n;
        for (size_t i = 0; i < n; ++i) {
            if (i >= real_toks.size() || i >= rep_toks.size() || real_toks[i] != rep_toks[i]) {
                first_diff = i;
                break;
            }
        }
        const auto& eb = tl.blocks[bi];
        std::printf("block %u: real=%zu rep=%zu first_diff=%zu | src_byte=%zu header_len=%u "
                    "block_size=%u bit_size=%d token_bits=%u recs=%zu",
                    bi, real_toks.size(), rep_toks.size(), first_diff, eb.src_byte, eb.header_len,
                    eb.block_size, eb.block_bit_size, eb.token_bits, sr.recs.size());
        if (first_diff == n) {
            std::printf("  IDENTICAL\n");
        } else {
            std::printf("\n");
            for (size_t i = (first_diff > 4 ? first_diff - 4 : 0); i < std::min(n, first_diff + 6);
                 ++i) {
                std::printf("  %s real=%-20s rep=%s\n", i == first_diff ? "X" : " ",
                            i < real_toks.size() ? real_toks[i].c_str() : "-",
                            i < rep_toks.size() ? rep_toks[i].c_str() : "-");
            }
            break; // first diverging block only
        }
    }
    return 0;
}
