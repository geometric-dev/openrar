// Shared v1.38.0 Gate 0 probe replica machinery (canonical tables, PR bit
// reader, decode variants). Included by decode_kernel_probe.cpp and
// decode_kernel_debug.cpp — keep the definitions identical across users.
#pragma once
// ── Canonical-table replica (EXACT HuffmanDecoder::build arrays) ────────────
struct Canon {
    core::uint32 decode_len[16]{};
    core::uint32 decode_pos[16]{};
    core::uint16 decode_num[306]{};
    size_t max_num{0};
    size_t nsym{0};
};

Canon canon_build(const core::byte* bit_lengths, size_t count) {
    Canon c;
    c.max_num = count;
    c.nsym = count;
    core::uint32 len_cnt[16] = {0};
    for (size_t i = 0; i < count; ++i) len_cnt[bit_lengths[i] & 0xF]++;
    len_cnt[0] = 0;
    c.decode_pos[0] = 0;
    c.decode_len[0] = 0;
    core::uint32 upper = 0;
    for (unsigned int i = 1; i < 16; ++i) {
        upper += len_cnt[i];
        c.decode_len[i] = upper << (16 - i);
        upper <<= 1;
        c.decode_pos[i] = c.decode_pos[i - 1] + len_cnt[i - 1];
    }
    core::uint32 copy_pos[16];
    std::memcpy(copy_pos, c.decode_pos, sizeof(copy_pos));
    for (size_t i = 0; i < count; ++i) {
        core::uint8 l = bit_lengths[i] & 0xF;
        if (l != 0) c.decode_num[copy_pos[l]++] = static_cast<core::uint16>(i);
    }
    return c;
}

// Canonical first-code-per-length + per-symbol code values (left-aligned in
// 16 bits). Over-subscribed symbols (first + index >= 2^len) get code == 0
// with valid == false: no slot is ever allocated for them, matching the
// shipped decoder where such codes are unreachable (their bit ranges are
// already consumed by earlier codes of the same length).
struct CodeAssign {
    core::uint32 code[306]; // left-aligned 16-bit; valid only when valid[sym]
    bool valid[306];
    core::uint32 first[16];
    core::uint32 len_cnt[16];
};

CodeAssign code_assign(const core::byte* lens, size_t nsym) {
    CodeAssign a{};
    for (size_t i = 0; i < nsym; ++i) a.len_cnt[lens[i] & 0xF]++;
    a.len_cnt[0] = 0;
    core::uint32 c = 0;
    for (unsigned int l = 1; l < 16; ++l) {
        a.first[l] = c;
        c = (c + a.len_cnt[l]) << 1;
    }
    core::uint32 idx[16] = {0};
    for (size_t s = 0; s < nsym; ++s) {
        core::uint32 l = lens[s] & 0xF;
        if (l == 0) {
            a.valid[s] = false;
            continue;
        }
        core::uint32 k = a.first[l] + idx[l]++;
        if (k >= (1u << l)) { // over-subscribed length class
            a.valid[s] = false;
            continue;
        }
        a.code[s] = k << (16 - l);
        a.valid[s] = true;
    }
    return a;
}

// ── BitReader replica (contiguous ctor only; exposed state) ─────────────────
struct PR {
    const core::byte* p_{nullptr};
    const core::byte* end_{nullptr};
    core::uint64 acc_{0};
    unsigned int n_{0};
    size_t consumed_{0};

    void init(const core::byte* data, size_t size) {
        p_ = data;
        end_ = data + size;
        acc_ = 0;
        n_ = 0;
        consumed_ = 0;
    }
    size_t bits_remaining() const { return static_cast<size_t>(end_ - p_) * 8 + n_; }
    void refill() {
        if (n_ > 56) return;
        for (;;) {
            if (p_ >= end_) return; // contiguous reader: true end of stream
            unsigned int max_bytes = (64 - n_) / 8;
            unsigned int k =
                std::min<unsigned int>(max_bytes, static_cast<unsigned int>(end_ - p_));
            if (k == 0) return;
            core::uint64 v = 0;
            std::memcpy(&v, p_, k);
#if defined(_MSC_VER)
            v = _byteswap_uint64(v);
#else
            v = __builtin_bswap64(v);
#endif
            v >>= (64 - 8 * k);
            if (k == 8) {
                acc_ = v;
            } else {
                acc_ = (acc_ << (8 * k)) | v;
            }
            n_ += 8 * k;
            p_ += k;
            if (n_ >= 57) return;
        }
    }
    core::uint32 peek(unsigned int count) {
        if (count == 0) return 0;
        if (n_ < count) refill();
        unsigned int take = std::min(count, n_);
        core::uint32 val = static_cast<core::uint32>(take ? acc_ >> (n_ - take) : 0);
        return val << (count - take);
    }
    core::uint64 peek64(unsigned int count) {
        if (count == 0) return 0;
        if (n_ < count) refill();
        unsigned int take = std::min(count, n_);
        core::uint64 val = (take ? acc_ >> (n_ - take) : 0);
        return val << (count - take);
    }
    void consume(unsigned int count) {
        if (count == 0) return;
        if (n_ < count) refill();
        unsigned int take = std::min(count, n_);
        consumed_ += take;
        n_ -= take;
        acc_ &= (n_ ? (core::uint64(1) << n_) - 1 : 0);
    }
    core::uint32 get(unsigned int count) {
        core::uint32 v = peek(count);
        consume(count);
        return v;
    }
    core::uint64 get64(unsigned int count) {
        if (count == 0) return 0;
        if (count > 64) count = 64;
        if (n_ < count) refill();
        if (n_ >= count) {
            core::uint64 val =
                (count == 64) ? acc_ : ((core::uint64(1) << count) - 1) & (acc_ >> (n_ - count));
            consume(count);
            return val;
        }
        if (count > 32) {
            unsigned int hc = count - 32;
            core::uint64 hi = get64(hc);
            core::uint64 lo = get64(32);
            return (hi << 32) | lo;
        }
        core::uint64 v = peek64(count);
        consume(count);
        return v;
    }
};

// Exact slot_to_length replica.
template <class R> core::uint32 slot_to_length(R& r, core::uint32 slot) {
    core::uint32 length = 2;
    core::uint32 l_bits = 0;
    if (slot < 8) {
        length += slot;
    } else {
        l_bits = slot / 4 - 1;
        length += (4 | (slot & 3)) << l_bits;
    }
    if (l_bits > 0) length += r.get(l_bits);
    return length;
}

// Shipped slow-path semantics over a Canon (exact; scan from i=1 — the
// shipped scan starts at QUICK_BITS+1 but the first i with
// bitfield < decode_len[i] is the code length either way, and uncovered
// values fail every comparison and fall to bits=15).
template <class R> core::uint32 canon_slow(R& r, core::uint32 v, const Canon& c) {
    core::uint32 bitfield = (v << 1) & 0xFFFE;
    unsigned int bits = 15;
    for (unsigned int i = 1; i < 15; ++i) {
        if (bitfield < c.decode_len[i]) {
            bits = i;
            break;
        }
    }
    r.consume(bits);
    core::uint32 dist = bitfield - c.decode_len[bits - 1];
    dist >>= (16 - bits);
    core::uint32 pos = c.decode_pos[bits] + dist;
    if (pos >= c.max_num) pos = 0;
    return c.decode_num[pos];
}

// ── Decode variants ──────────────────────────────────────────────────────────
// V1: the shipped decoder's structure, replica (10-bit quick + canonical slow).
struct VQuick {
    Canon c_;
    core::uint32 quick[1 << 10]{};
    size_t pages_materialized_{0};
    size_t page_writes_{0};

    void build(const Canon& c) {
        c_ = c;
        unsigned int cur_len = 1;
        for (size_t code = 0; code < (1u << 10); ++code) {
            core::uint32 bitfield = static_cast<core::uint32>(code) << 6; // (16-10)
            while (cur_len < 16 && bitfield >= c_.decode_len[cur_len]) ++cur_len;
            if (cur_len > 10) {
                quick[code] = 0;
            } else {
                unsigned int len_cap = cur_len >= 16 ? 15 : cur_len;
                core::uint32 symbol = 0;
                core::uint32 dist = bitfield - c_.decode_len[len_cap - 1];
                dist >>= (16 - len_cap);
                core::uint32 pos = c_.decode_pos[len_cap] + dist;
                if (pos < c_.max_num) symbol = c_.decode_num[pos];
                quick[code] = (static_cast<core::uint32>(len_cap) << 16) | symbol;
            }
        }
    }
    void build(const Canon& c, const core::byte*, size_t) { build(c); }
    template <class R> core::uint32 decode(R& r) const {
        core::uint32 peek = r.peek(10);
        core::uint32 e = quick[peek];
        core::uint32 len = e >> 16;
        if (len != 0) {
            r.consume(len);
            return e & 0xFFFF;
        }
        core::uint32 bitfield = r.peek(15) << 1;
        bitfield &= 0xFFFE;
        unsigned int bits = 15;
        for (unsigned int i = 11; i < 15; ++i) {
            if (bitfield < c_.decode_len[i]) {
                bits = i;
                break;
            }
        }
        r.consume(bits);
        core::uint32 dist = bitfield - c_.decode_len[bits - 1];
        dist >>= (16 - bits);
        core::uint32 pos = c_.decode_pos[bits] + dist;
        if (pos >= c_.max_num) pos = 0;
        return c_.decode_num[pos];
    }
};

// V2: fused single peek(15); top 10 bits index the quick table; slow path on
// the same 16-bit bitfield without a second peek.
struct VFused {
    Canon c_;
    core::uint32 quick[1 << 10]{};
    size_t pages_materialized_{0};
    size_t page_writes_{0};

    void build(const Canon& c) {
        c_ = c;
        VQuick vq;
        vq.build(c);
        std::memcpy(quick, vq.quick, sizeof(quick));
    }
    void build(const Canon& c, const core::byte*, size_t) { build(c); }
    core::uint32 decode(PR& r) const {
        core::uint32 v = r.peek(15);
        core::uint32 e = quick[v >> 5]; // top 10 of 15
        core::uint32 len = e >> 16;
        if (len != 0) {
            r.consume(len);
            return e & 0xFFFF;
        }
        core::uint32 bitfield = (v << 1) & 0xFFFE;
        unsigned int bits = 15;
        for (unsigned int i = 11; i < 15; ++i) {
            if (bitfield < c_.decode_len[i]) {
                bits = i;
                break;
            }
        }
        r.consume(bits);
        core::uint32 dist = bitfield - c_.decode_len[bits - 1];
        dist >>= (16 - bits);
        core::uint32 pos = c_.decode_pos[bits] + dist;
        if (pos >= c_.max_num) pos = 0;
        return c_.decode_num[pos];
    }
};

// V3: lazy-page flat table. Primary PB bits; a slot either resolves directly
// (code len <= PB) or names a second-level page materialized on first use.
// Uncovered slots (incomplete/hostile codes) fall back to the canonical walk
// — exactly the shipped slow-path semantics; complete tables never hit it.
struct VFlatLazy {
    unsigned PB_{10};
    unsigned REST_{5};
    core::uint32 page_size_{32};
    Canon c_;
    // prim entry: (len<<16)|sym for len<=PB codes; 0xFFFFFFFF -> page for
    // this prefix; 0 -> uncovered prefix (slow path per peek value).
    std::vector<core::uint32> prim;
    std::vector<core::uint32> page_id;  // per prim slot: page id or NONE
    std::vector<core::uint32> pages;    // materialized pages, page_size_ each
    std::vector<core::uint32> page_len; // per page slot: 0 -> uncovered (slow)
    std::vector<std::vector<core::uint32>> bucket_code_, bucket_len_, bucket_sym_;
    size_t pages_materialized_{0};
    size_t page_writes_{0};

    void configure(unsigned pb) {
        PB_ = pb;
        REST_ = 15 - pb;
        page_size_ = 1u << REST_;
    }

    void build_lens(const Canon& c, const core::byte* lens, size_t nsym) {
        c_ = c;
        prim.assign(size_t{1} << PB_, 0);
        page_id.assign(size_t{1} << PB_, 0xFFFFFFFFu);
        pages.clear();
        page_len.clear();
        pages_materialized_ = 0;
        page_writes_ = 0;
        CodeAssign a = code_assign(lens, nsym);
        bucket_code_.clear();
        bucket_len_.clear();
        bucket_sym_.clear();
        bucket_code_.resize(size_t{1} << PB_);
        bucket_len_.resize(size_t{1} << PB_);
        bucket_sym_.resize(size_t{1} << PB_);
        bool has_page = false;
        std::vector<char> page_marked(size_t{1} << PB_, 0);
        for (size_t sym = 0; sym < nsym; ++sym) {
            if (!a.valid[sym]) continue;
            core::uint32 l = lens[sym] & 0xF;
            if (l <= PB_) {
                core::uint32 span = 1u << (PB_ - l);
                core::uint32 base = (a.code[sym] >> (16 - l)) << (PB_ - l);
                core::uint32 entry = (l << 16) | static_cast<core::uint32>(sym);
                for (core::uint32 s = 0; s < span; ++s) prim[base + s] = entry;
            } else {
                core::uint32 pfx = (a.code[sym] >> 1) >> REST_; // top PB of the 15-bit index
                bucket_code_[pfx].push_back(a.code[sym]);
                bucket_len_[pfx].push_back(l);
                bucket_sym_[pfx].push_back(static_cast<core::uint32>(sym));
                if (!page_marked[pfx]) {
                    page_marked[pfx] = 1;
                    prim[pfx] = 0xFFFFFFFFu;
                    has_page = true;
                }
            }
        }
        (void)has_page;
    }
    void build(const Canon& c, const core::byte* lens, size_t nsym) { build_lens(c, lens, nsym); }

    void materialize(core::uint32 pfx) {
        core::uint32 id = static_cast<core::uint32>(page_len.size() / page_size_);
        page_len.resize(page_len.size() + page_size_, 0);
        pages.resize(pages.size() + page_size_, 0);
        page_id[pfx] = id;
        ++pages_materialized_;
        const auto& bc = bucket_code_[pfx];
        const auto& bl = bucket_len_[pfx];
        const auto& bs = bucket_sym_[pfx];
        for (size_t i = 0; i < bc.size(); ++i) {
            // Sub-index of the code's first peek inside the page; spans never
            // cross a page for len > PB (top-aligned multiples of 2^(15-len)
            // divide 2^REST evenly and 15-len < REST).
            core::uint32 start = (bc[i] >> 1) & (page_size_ - 1);
            core::uint32 span = 1u << (15 - bl[i]);
            core::uint32 entry = (bl[i] << 16) | bs[i];
            core::uint32 base = id * page_size_ + start;
            for (core::uint32 s = 0; s < span; ++s) {
                pages[base + s] = entry;
                page_len[base + s] = bl[i];
            }
            page_writes_ += span;
        }
    }

    core::uint32 decode(PR& r) {
        core::uint32 v = r.peek(15);
        core::uint32 pfx = v >> REST_;
        core::uint32 e = prim[pfx];
        if (e != 0xFFFFFFFFu) {
            core::uint32 len = e >> 16;
            if (len != 0) {
                r.consume(len);
                return e & 0xFFFF;
            }
            return canon_slow(r, v, c_);
        }
        if (page_id[pfx] == 0xFFFFFFFFu) materialize(pfx);
        core::uint32 slot = page_id[pfx] * page_size_ + (v & (page_size_ - 1));
        if (page_len[slot] != 0) {
            r.consume(pages[slot] >> 16);
            return pages[slot] & 0xFFFF;
        }
        return canon_slow(r, v, c_);
    }
};

// V4: full 15-bit flat table (the measured trap reference).
struct VFullFlat {
    Canon c_;
    core::uint32 t[1 << 15]{};
    size_t pages_materialized_{0};
    size_t page_writes_{0};

    void build_lens(const Canon& c, const core::byte* lens, size_t nsym) {
        c_ = c;
        CodeAssign a = code_assign(lens, nsym);
        for (size_t sym = 0; sym < nsym; ++sym) {
            if (!a.valid[sym]) continue;
            core::uint32 l = lens[sym] & 0xF;
            core::uint32 start = a.code[sym] >> 1; // first 15-bit peek index
            core::uint32 span = 1u << (15 - l);
            core::uint32 entry = (l << 16) | static_cast<core::uint32>(sym);
            for (core::uint32 s = 0; s < span; ++s) t[start + s] = entry;
        }
    }
    void build(const Canon& c, const core::byte* lens, size_t nsym) { build_lens(c, lens, nsym); }
    core::uint32 decode(PR& r) const {
        core::uint32 v = r.peek(15);
        core::uint32 e = t[v];
        if (e != 0) {
            r.consume(e >> 16);
            return e & 0xFFFF;
        }
        return canon_slow(r, v, c_);
    }
};


// Single-block token dump for the divergence finder (debug tool).
static std::vector<std::string> replica_block_tokens(const Decompressor50::PrescanTimeline& tl,
                                                     uint32_t bi, const core::byte* src,
                                                     size_t src_size, size_t win) {
    (void)win;
    std::vector<std::string> out;
    const auto& expect = tl.blocks[bi];
    if (expect.src_byte >= src_size) return out;
    PR r;
    r.init(src + expect.src_byte, src_size - expect.src_byte);
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
            out.push_back(buf);
        } else if (slot == 256) {
            std::snprintf(buf, sizeof(buf), "F@%zu", r.consumed_);
            out.push_back(buf);
            break;
        } else if (slot == 257) {
            out.push_back("S257");
        } else if (slot >= 262) {
            core::uint32 len = slot_to_length(r, slot - 262);
            if (r.bits_remaining() < 1) break;
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
                        if (r.bits_remaining() < d_bits - 4) break;
                        core::uint64 extra =
                            (d_bits > 36) ? r.get64(d_bits - 4) : r.get(d_bits - 4);
                        distance += static_cast<size_t>(extra) << 4;
                    }
                    if (r.bits_remaining() < 1) break;
                    core::uint32 low = ldd.decode(r);
                    distance += low;
                } else {
                    if (r.bits_remaining() < d_bits) break;
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
            out.push_back(buf);
        } else {
            if (r.bits_remaining() < 1) break;
            core::uint32 len_slot = rd.decode(r);
            core::uint32 len = slot_to_length(r, len_slot);
            std::snprintf(buf, sizeof(buf), "R%u l=%u", slot - 258, len);
            out.push_back(buf);
        }
        if (out.size() >= 24) break;
    }
    return out;
}


// Single-block hash variant (debug tool): same body as token_loop for one
// block index.
template <class D>
core::uint64 token_loop_block(const Decompressor50::PrescanTimeline& tl, uint32_t blk,
                              const core::byte* src, size_t src_size, D& ld, D& dd, D& ldd, D& rd,
                              size_t* builds, size_t* pages_mat, size_t* page_writes) {
    core::uint64 h = 1469598103934665603ull;
    auto mix = [&](core::uint64 x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    Canon cl, cd, cl16, cr;
    const auto& expect = tl.blocks[blk];
    if (expect.src_byte >= src_size) return h;
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
            *page_writes += ld.page_writes_ + dd.page_writes_ + ldd.page_writes_ + rd.page_writes_;
    }
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
        if (r.bits_remaining() < 1) return h;
        core::uint32 len_slot = rd.decode(r);
        core::uint32 len = slot_to_length(r, len_slot);
        mix(0x4000000ull | (core::uint64(slot - 258) << 16) | len);
    }
    return h;
}
