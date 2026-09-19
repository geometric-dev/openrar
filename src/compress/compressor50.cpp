#include "compressor50.hpp"
#include "arch/match_simd.hpp"
#include <algorithm>
#include <cstring>
#include <cassert>
#include <cstdio>

namespace openrar::compress {

/////////////////////////////////////////////////////////////////////////////
// HuffmanBuilder
/////////////////////////////////////////////////////////////////////////////


void HuffmanBuilder::build_lengths(const core::uint32* freq, core::uint32 size,
                                   core::uint32 max_bits, core::byte* lengths) {
    if (size == 0) return;
    std::memset(lengths, 0, size);

    core::uint32 used = 0;
    for (core::uint32 i = 0; i < size; i++) {
        if (freq[i] != 0) used++;
    }
    if (used == 0) return;
    if (used == 1) {
        for (core::uint32 i = 0; i < size; i++) {
            if (freq[i] != 0) lengths[i] = 1;
        }
        return;
    }

    // Shrink thread_local vectors when capacity is ≥4× the needed size to
    // avoid indefinite retention of worst-case allocations in long-lived hosts
    // (DLL, server embedding).  The 4× threshold avoids thrashing on
    // alternating sizes while still reclaiming memory after a one-off large call.
    auto maybe_shrink = [](auto& vec, size_t needed) {
        if (vec.capacity() >= needed * 4) {
            vec.shrink_to_fit();
        }
    };

    struct TreeNode {
        core::uint64 weight;
        int child_l, child_r;
        int symbol;
    };
    static thread_local std::vector<TreeNode> tree;
    tree.clear();
    maybe_shrink(tree, size * 2);
    tree.reserve(size * 2);

    static thread_local std::vector<core::uint32> q1;
    q1.clear();
    maybe_shrink(q1, size);
    q1.reserve(size);

    static thread_local std::vector<core::uint32> q2;
    q2.clear();
    maybe_shrink(q2, size);
    q2.reserve(size);

    static thread_local std::vector<core::uint32> depths;
    depths.clear();
    maybe_shrink(depths, size);
    depths.resize(size, 0);

    static thread_local std::vector<std::pair<core::uint32, core::uint32>> stack;
    stack.clear();
    maybe_shrink(stack, 64);
    stack.reserve(64);

    static thread_local std::vector<core::uint32> bl_count;
    bl_count.clear();
    maybe_shrink(bl_count, max_bits + 1);
    bl_count.assign(max_bits + 1, 0);

    static thread_local std::vector<core::uint32> sorted_syms;
    sorted_syms.clear();
    maybe_shrink(sorted_syms, size);
    sorted_syms.reserve(size);


    for (core::uint32 i = 0; i < size; i++) {
        if (freq[i] != 0) {
            tree.push_back({freq[i], -1, -1, static_cast<int>(i)});
            q1.push_back(static_cast<core::uint32>(tree.size() - 1));
        }
    }

    std::sort(q1.begin(), q1.end(), [&](core::uint32 a, core::uint32 b) {
        if (tree[a].weight != tree[b].weight) return tree[a].weight < tree[b].weight;
        return tree[a].symbol < tree[b].symbol;
    });

    core::uint32 i1 = 0, i2 = 0;
    auto get_min = [&]() -> core::uint32 {
        if (i1 < q1.size() && (i2 == q2.size() || tree[q1[i1]].weight <= tree[q2[i2]].weight)) {
            return q1[i1++];
        }
        return q2[i2++];
    };

    while (q1.size() - i1 + q2.size() - i2 > 1) {
        core::uint32 left = get_min();
        core::uint32 right = get_min();
        tree.push_back({tree[left].weight + tree[right].weight, static_cast<int>(left),
                        static_cast<int>(right), -1});
        q2.push_back(static_cast<core::uint32>(tree.size() - 1));
    }

    core::uint32 root = static_cast<core::uint32>(tree.size() - 1);

    // reset depths and stack correctly
    std::fill(depths.begin(), depths.end(), 0);
    stack.clear();
    stack.push_back({root, 0});

    core::uint32 max_depth = 0;
    while (!stack.empty()) {
        auto entry = stack.back();
        stack.pop_back();
        core::uint32 node_idx = entry.first;
        core::uint32 d = entry.second;
        if (tree[node_idx].symbol != -1) {
            depths[tree[node_idx].symbol] = static_cast<core::byte>(d);
            if (d > max_depth) max_depth = d;
        } else {
            stack.push_back({static_cast<core::uint32>(tree[node_idx].child_r), d + 1});
            stack.push_back({static_cast<core::uint32>(tree[node_idx].child_l), d + 1});
        }
    }

    if (max_depth > max_bits) {
        bl_count.assign(max_depth + 1, 0);
        for (core::uint32 i = 0; i < size; ++i) {
            if (depths[i] > 0) bl_count[depths[i]]++;
        }

        // Bit-length limiting via the zlib gen_bitlen approach.
        // 1. Collapse all over-limit leaves to max_bits.
        core::uint32 overflow = 0;
        for (core::uint32 d = max_bits + 1; d <= max_depth; ++d) {
            overflow += bl_count[d];
            bl_count[d] = 0;
        }
        bl_count[max_bits] += overflow;

        // 2. Recompute Kraft sum to determine over-completeness
        core::uint32 kraft_limit = 1u << max_bits;
        core::uint32 kraft_sum = 0;
        for (core::uint32 d = 1; d <= max_bits; ++d) {
            kraft_sum += bl_count[d] << (max_bits - d);
        }

        // 3. While over-complete, push leaves to longer codes
        while (kraft_sum > kraft_limit) {
            core::uint32 d = 1;
            while (d < max_bits && bl_count[d] == 0) d++;
            if (d >= max_bits) break; // all at max_bits, can't push deeper

            bl_count[d]--;
            bl_count[d + 1]++;
            kraft_sum -= (1u << (max_bits - d - 1));
        }

        max_depth = max_bits;

        sorted_syms.clear();
        for (core::uint32 i = 0; i < size; ++i) {
            if (depths[i] > 0) sorted_syms.push_back(i);
        }
        std::sort(sorted_syms.begin(), sorted_syms.end(), [&](core::uint32 a, core::uint32 b) {
            if (freq[a] != freq[b]) return freq[a] > freq[b];
            return a < b;
        });

        core::uint32 sym_idx = 0;
        for (core::uint32 bits = 1; bits <= max_bits; ++bits) {
            for (core::uint32 i = 0; i < bl_count[bits]; ++i) {
                assert(sym_idx < sorted_syms.size() &&
                       "bl_count overruns sorted_syms — Kraft invariant broken");
                depths[sorted_syms[sym_idx++]] = bits;
            }
        }
    }

    for (core::uint32 i = 0; i < size; ++i) {
        lengths[i] = static_cast<core::byte>(depths[i]);
    }
}

void HuffmanBuilder::build_codes(const core::byte* lengths, core::uint32 size,
                                 core::uint32* codes) {
    core::uint32 bl_count[16] = {0};
    core::uint32 next_code[16] = {0};
    for (core::uint32 i = 0; i < size; i++) {
        if ((lengths[i] & 0xf) != 0) bl_count[lengths[i] & 0xf]++;
    }

    core::uint32 code = 0;
    for (core::uint32 bits = 1; bits < 16; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }
    for (core::uint32 i = 0; i < size; i++) {
        core::uint32 len = lengths[i] & 0xf;
        codes[i] = len != 0 ? next_code[len]++ : 0;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Compressor50 Implementation
/////////////////////////////////////////////////////////////////////////////

void Compressor50::reset_state() {
    src_file_ = nullptr;
    dest_file_ = nullptr;
    src_size_ = 0;
    src_loaded_ = 0;
    src_eof_ = false;
    buf_data_ = nullptr;
    external_buf_ = false;
    mem_src_ptr_ = nullptr;
    mem_src_size_ = 0;
    mem_src_pos_ = 0;
    pos_base_ = 0;
    buf_size_ = 0;
    win_size_ = 0;
    max_dist_ = 0;
    method_ = 5;
    packed_total_ = 0;
    total_at_file_start_ = 0;
    input_since_block_ = 0;
    cur_ = 0;
    unhashed_pos_ = 0;
    mem_out_ = nullptr;
    token_seq_.clear();
    lit_bytes_.clear();
    match_tokens_.clear();
    filter_tokens_.clear();
    active_filter_ = FilterType::None;
    filter_channels_ = 1;
    filter_emitted_until_ = 0;
    is_large_window_ = false;
    head_.clear();
    prev_.clear();
    head64_.clear();
    prev64_.clear();
    reset_old_dist();
    hash_crc32_.reset();
    hash_blake2_.reset();
    init_freq();
}

Compressor50::Compressor50() {
    reset_state();
}

void Compressor50::begin_archive(io::FileStream* dest, int method, size_t win_size) {
    // Wipe every scalar first. Without this, a Compressor50 that was
    // previously used with set_external_buffer() or as the memory-input
    // packer inside compress_buffer() would retain external_buf_==true or
    // stale mem_src_* pointers, and load_data() would short-circuit or
    // memcpy from a dangling source on the next session.
    reset_state();
    dest_file_ = dest;
    method_ = method < 1 || method > 5 ? 5 : method;

    const core::uint64 MAX_RETAIN = std::min<core::uint64>(
        0x1000000000ULL, static_cast<core::uint64>(static_cast<size_t>(-1)) - 0x1000000);
    max_dist_ = std::min(static_cast<core::uint64>(win_size), MAX_RETAIN);
    size_t ws = static_cast<size_t>(max_dist_ == 0 ? 0x20000 : max_dist_);
    size_t pow2_ws = 0x10000;
    while (pow2_ws < ws && pow2_ws < static_cast<size_t>(MAX_RETAIN)) {
        pow2_ws <<= 1;
    }
    win_size_ = std::max<size_t>(0x10000, pow2_ws);
    assert((win_size_ & (win_size_ - 1)) == 0);

    buf_size_ = win_size_ + 0x400000 + READ_CHUNK;
    buf_.resize(buf_size_);
    buf_data_ = buf_.data();

    is_large_window_ = (win_size_ > 4ULL * 1024 * 1024 * 1024);
    if (is_large_window_) {
        head_.clear();
        prev_.clear();
        head64_.assign(HASH_SIZE, static_cast<core::uint64>(-1));
        prev64_.assign(win_size_, static_cast<core::uint64>(-1));
    } else {
        head64_.clear();
        prev64_.clear();
        head_.assign(HASH_SIZE, static_cast<core::uint32>(-1));
        prev_.assign(win_size_, static_cast<core::uint32>(-1));
    }

    cur_ = 0;
    pos_base_ = 0;
    src_loaded_ = 0;
    src_eof_ = false;
    reset_old_dist();
    init_freq();
}

// -- Streaming session (Path A) ----------------------------------------------
bool Compressor50::begin_stream(int method, size_t win_size) {
    if (method < 1 || method > 5) return false; // STORE (0) never streams - passthrough
    if (win_size == 0) win_size = 0x200000;
    streaming_ = true;
    stream_finished_ = false;
    begin_archive(nullptr, method, win_size);
    src_size_ = 0; // grows with every feed: every loaded byte is valid input
    init_match_params();
    fail_count_ = 0;
    return true;
}

int Compressor50::feed(const core::byte* data, size_t n) {
    if (!streaming_ || stream_finished_) return -1;
    if (!data && n != 0) return -1;
    const core::byte* p = data;
    size_t left = n;
    while (left > 0) {
        size_t used = static_cast<size_t>(src_loaded_ - pos_base_);
        size_t space = buf_size_ > used ? buf_size_ - used : 0;
        if (space == 0) {
            // Cannot normally happen: process_available keeps the unprocessed
            // gap at <= 4 bytes, so used <= win_size_ + 4 < buf_size_ after
            // every pass. Slide defensively rather than overflow.
            slide_window();
            continue;
        }
        size_t chunk = std::min(space, left);
        std::memcpy(&buf_[used], p, chunk);
        src_loaded_ += chunk;
        src_size_ = src_loaded_;
        p += chunk;
        left -= chunk;
        if (process_available(false) < 0) return -1;
    }
    return 0;
}

int Compressor50::finish_stream() {
    if (!streaming_ || stream_finished_) return -1;
    src_eof_ = true;
    src_size_ = src_loaded_;
    if (process_available(true) < 0) return -1;
    consume_source(unhashed_pos_, cur_);
    unhashed_pos_ = cur_;
    if (!write_block(true)) return -1;
    stream_finished_ = true;
    return 0;
}

void Compressor50::start_file(io::FileStream* src, core::uint64 src_file_size,
                              bool continue_window) {
    src_file_ = src;
    src_size_ = src_file_size;
    src_eof_ = false;
    token_seq_.clear();
    lit_bytes_.clear();
    match_tokens_.clear();
    filter_tokens_.clear();
    filter_emitted_until_ = 0;
    input_since_block_ = 0;
    total_at_file_start_ = packed_total_;
    hash_crc32_.reset();
    hash_blake2_.reset();

    if (!continue_window) {
        cur_ = 0;
        pos_base_ = 0;
        src_loaded_ = 0;
        reset_old_dist();
    } else {
        src_size_ = src_loaded_ + src_file_size;
    }
    unhashed_pos_ = cur_;
}

void Compressor50::init(io::FileStream* src, io::FileStream* dest, int method,
                        core::uint64 src_file_size, size_t win_size) {
    begin_archive(dest, method, win_size);
    src_file_ = src;
    src_size_ = src_file_size;
    packed_total_ = 0;
    total_at_file_start_ = 0;
    mem_out_ = nullptr;
    hash_crc32_.reset();
    hash_blake2_.reset();
    init_freq();
}

void Compressor50::init_freq() {
    std::memset(freq_ld_, 0, sizeof(freq_ld_));
    std::memset(freq_dd_, 0, sizeof(freq_dd_));
    std::memset(freq_ldd_, 0, sizeof(freq_ldd_));
    std::memset(freq_rd_, 0, sizeof(freq_rd_));
    std::memset(freq_bd_, 0, sizeof(freq_bd_));
}

void Compressor50::load_data(core::uint64 until) {
    if (external_buf_) return;
    if (streaming_) return; // feed() copies bytes into buf_ directly
    if (until > src_size_) until = src_size_;
    if (until <= src_loaded_) return;

    while (src_loaded_ < until && !src_eof_) {
        size_t space = buf_size_ - static_cast<size_t>(src_loaded_ - pos_base_);
        if (space == 0) break;
        size_t to_read = std::min(space, static_cast<size_t>(until - src_loaded_));

        size_t read_size = 0;
        if (src_file_) {
            read_size =
                src_file_->read(&buf_[static_cast<size_t>(src_loaded_ - pos_base_)], to_read);
        } else if (mem_src_ptr_) {
            size_t avail = mem_src_size_ - mem_src_pos_;
            read_size = std::min(to_read, avail);
            if (read_size > 0) {
                std::memcpy(&buf_[static_cast<size_t>(src_loaded_ - pos_base_)],
                            mem_src_ptr_ + mem_src_pos_, read_size);
                mem_src_pos_ += read_size;
            }
        }

        if (read_size == 0) {
            src_eof_ = true;
            break;
        }
        src_loaded_ += read_size;
    }
}

core::uint32 Compressor50::calc_hash(core::uint64 pos) {
    const core::byte* d = &buf_data_[static_cast<size_t>(pos - pos_base_)];
    core::uint32 val;
    std::memcpy(&val, d, 4);
    return (val * 0x9e3779b1u) >> (32 - HASH_BITS);
}

void Compressor50::insert_position(core::uint64 pos) {
    if (pos + 4 > src_loaded_ || pos < pos_base_) return;
    core::uint32 h = calc_hash(pos);
    if (is_large_window_) {
        prev64_[static_cast<size_t>(pos & (win_size_ - 1))] = head64_[h];
        head64_[h] = pos;
    } else {
        prev_[static_cast<size_t>(pos & (win_size_ - 1))] = head_[h];
        head_[h] = static_cast<core::uint32>(pos & 0xffffffff);
    }
}

size_t Compressor50::match_length(core::uint64 pos, core::uint64 cand, size_t cap) {
    const core::byte* p = &buf_data_[static_cast<size_t>(pos - pos_base_)];
    const core::byte* q = &buf_data_[static_cast<size_t>(cand - pos_base_)];
    return arch::match_length_simd(p, q, cap);
}

Compressor50::MatchInfo Compressor50::find_match(core::uint64 pos, core::uint32 max_chain,
                                                 core::uint32 nice_len) {
    MatchInfo best = {0, -1};
    if (pos + 4 > src_loaded_ || pos < pos_base_) return best;

    core::uint32 h = calc_hash(pos);
    core::int64 cand = -1;
    if (is_large_window_) {
        core::uint64 hpos = head64_[h];
        if (hpos != static_cast<core::uint64>(-1) && pos >= hpos && (pos - hpos) <= max_dist_) {
            cand = static_cast<core::int64>(hpos);
        }
    } else {
        cand = reconstruct_pos(pos, head_[h]);
    }
    core::uint32 chain = max_chain;
    size_t limit = avail_at(pos);
    if (limit > MAX_LZ_MATCH + 3) limit = MAX_LZ_MATCH + 3;

    size_t win_mask = win_size_ - 1;
    const core::byte* buf_pos = &buf_data_[static_cast<size_t>(pos - pos_base_)];

    core::uint32 p4 = 0;
    if (limit >= 4) {
        std::memcpy(&p4, buf_pos, 4);
    }

    while (cand >= 0 && chain-- > 0) {
        if (static_cast<core::uint64>(cand) < pos_base_ ||
            (pos - static_cast<core::uint64>(cand)) > max_dist_)
            break;
        const core::byte* buf_cand =
            &buf_data_[static_cast<size_t>(static_cast<core::uint64>(cand) - pos_base_)];

        core::int64 next_cand = -1;
        if (is_large_window_) {
            core::uint64 next64 = prev64_[static_cast<size_t>(cand) & win_mask];
            if (next64 != static_cast<core::uint64>(-1)) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(&prev64_[static_cast<size_t>(next64) & win_mask]);
#endif
                if (pos >= next64 && (pos - next64) <= max_dist_) {
                    next_cand = static_cast<core::int64>(next64);
                }
            }
        } else {
            core::uint32 next = prev_[static_cast<size_t>(cand) & win_mask];
            if (next != 0xffffffff) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(&prev_[next & win_mask]);
#endif
            }
            next_cand = reconstruct_pos(pos, next);
        }

        if (best.length > 0) {
            if (best.length < limit && buf_cand[best.length] != buf_pos[best.length]) {
                cand = next_cand;
                continue;
            }
            if (buf_cand[0] != buf_pos[0]) {
                cand = next_cand;
                continue;
            }
        }

        if (limit >= 4) {
            core::uint32 c4;
            std::memcpy(&c4, buf_cand, 4);
            if (c4 != p4) {
                cand = next_cand;
                continue;
            }
        } else if (limit >= 3 && (buf_cand[1] != buf_pos[1] || buf_cand[2] != buf_pos[2])) {
            cand = next_cand;
            continue;
        }

        size_t len = match_length(pos, static_cast<core::uint64>(cand), limit);
        if (len > best.length) {
            best.length = len;
            best.candidate = cand;
            if (len >= nice_len || len >= limit) break;
        }
        cand = next_cand;
    }
    if (best.length < MIN_MATCH) best.length = 0;
    return best;
}

size_t Compressor50::rep_length(core::uint64 pos, size_t dist, size_t cap) {
    if (dist == static_cast<size_t>(-1) || dist == 0 || dist > pos - pos_base_ || dist > max_dist_)
        return 0;
    size_t max_len = avail_at(pos);
    if (cap > max_len) cap = max_len;
    const core::byte* p = &buf_data_[static_cast<size_t>(pos - pos_base_)];
    const core::byte* q = p - dist;
    return arch::match_length_simd(p, q, cap);
}

core::uint32 Compressor50::length_increment(size_t distance) {
    core::uint32 inc = 0;
    if (distance > 0x100) {
        inc++;
        if (distance > 0x2000) {
            inc++;
            if (distance > 0x40000) inc++;
        }
    }
    return inc;
}

struct LengthSlotEntry {
    core::byte slot;
    core::byte bits;
    core::uint16 extra;
};

static const struct LengthSlotTableInit {
    LengthSlotEntry entries[Compressor50::MAX_LZ_MATCH];

    LengthSlotTableInit() : entries{} {
        for (core::uint32 length = 2; length <= Compressor50::MAX_LZ_MATCH; length++) {
            core::uint32 v = length - 2;
            core::uint32 slot = 0, extra = 0, bits = 0;
            if (v < 8) {
                slot = v;
                extra = 0;
                bits = 0;
            } else {
                for (core::uint32 s = 8; s < 44; s++) {
                    core::uint32 lb = s / 4 - 1;
                    core::uint32 base = (4 | (s & 3)) << lb;
                    core::uint32 span = core::uint32(1) << lb;
                    if (v < base + span) {
                        slot = s;
                        extra = v - base;
                        bits = lb;
                        break;
                    }
                }
            }
            entries[v].slot = static_cast<core::byte>(slot);
            entries[v].bits = static_cast<core::byte>(bits);
            entries[v].extra = static_cast<core::uint16>(extra);
        }
    }
} g_length_slot_table;

void Compressor50::length_to_slot(core::uint32 length, core::uint32& slot, core::uint32& extra,
                                  core::uint32& bits) {
    core::uint32 v = length >= 2 ? length - 2 : 0;
    assert(v < MAX_LZ_MATCH);
    const LengthSlotEntry& e = g_length_slot_table.entries[v];
    slot = e.slot;
    bits = e.bits;
    extra = e.extra;
}

void Compressor50::distance_to_slot(size_t distance, core::uint32& slot, core::uint64& extra,
                                    core::uint32& raw_bits) {
    assert(distance >= 1);
    if (distance == 0) distance = 1;
    if (distance <= 4) {
        slot = static_cast<core::uint32>(distance > 0 ? distance - 1 : 0);
        extra = 0;
        raw_bits = 0;
        return;
    }

#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
    unsigned long msb;
    _BitScanReverse64(&msb, distance - 1);
    core::uint32 dbits = static_cast<core::uint32>(msb - 1);
#elif defined(_MSC_VER)
    // L14: 32-bit x86 MSVC has no _BitScanReverse64. distance <= max_dist_ =
    // min(win_size, SIZE_MAX - 0x1000000) < 2^32 on 32-bit targets, so
    // (distance - 1) always fits the 32-bit scan exactly.
    unsigned long msb;
    _BitScanReverse(&msb, static_cast<core::uint32>(distance - 1));
    core::uint32 dbits = static_cast<core::uint32>(msb - 1);
#else
    core::uint32 dbits = 63 - __builtin_clzll(distance - 1) - 1;
#endif

    core::uint32 bit = static_cast<core::uint32>((distance - 1) >> dbits) & 1;
    slot = (dbits + 1) * 2 + bit;
    assert(slot < DCX);
    size_t base = static_cast<size_t>(2 | bit) << dbits;
    // uint64: distances above 16 GiB need more than 32 extra bits; the old
    // u32 narrowing silently dropped the top bits and emitted a wrong
    // distance (sweep finding H1).
    extra = static_cast<core::uint64>(distance - (base + 1));
    raw_bits = dbits;
}

void Compressor50::add_literal(core::byte b) {
    token_seq_.push_back(0);
    lit_bytes_.push_back(b);
    if (freq_ld_[b] < 0xfffe) freq_ld_[b]++;
}

void Compressor50::add_match(size_t length, size_t distance) {
    core::uint32 inc = length_increment(distance);
    core::uint32 base;
    if (length <= static_cast<size_t>(inc)) {
        base = 2;
    } else {
        base = static_cast<core::uint32>(length - inc);
        if (base < 2) base = 2;
        if (base > MAX_LZ_MATCH) base = MAX_LZ_MATCH;
    }

    core::uint32 len_slot, len_extra, len_bits;
    length_to_slot(base, len_slot, len_extra, len_bits);

    core::uint32 dist_slot, raw_bits;
    core::uint64 dist_extra;
    distance_to_slot(distance, dist_slot, dist_extra, raw_bits);

    if (freq_ld_[262 + len_slot] < 0xfffe) freq_ld_[262 + len_slot]++;
    if (freq_dd_[dist_slot] < 0xfffe) freq_dd_[dist_slot]++;
    if (raw_bits >= 4 && freq_ldd_[dist_extra & 0xf] < 0xfffe) freq_ldd_[dist_extra & 0xf]++;

    Compressor50Token tok{};
    tok.set_type(Compressor50Token::TokenType::Match);
    tok.set_len_slot(static_cast<core::byte>(len_slot));
    tok.len_extra = static_cast<core::uint16>(len_extra);
    tok.set_dist_slot(static_cast<core::byte>(dist_slot));
    tok.dist_extra = dist_extra;
    token_seq_.push_back(1);
    match_tokens_.push_back(tok);
    insert_old_dist(distance);
}

void Compressor50::add_rep(core::uint32 index, size_t length) {
    core::uint32 len_slot, len_extra, len_bits;
    length_to_slot(static_cast<core::uint32>(length), len_slot, len_extra, len_bits);
    if (freq_ld_[258 + index] < 0xfffe) freq_ld_[258 + index]++;
    if (freq_rd_[len_slot] < 0xfffe) freq_rd_[len_slot]++;

    Compressor50Token tok{};
    tok.set_type(static_cast<Compressor50Token::TokenType>(
        static_cast<core::byte>(Compressor50Token::TokenType::Rep0) + index));
    tok.set_len_slot(static_cast<core::byte>(len_slot));
    tok.len_extra = static_cast<core::uint16>(len_extra);
    token_seq_.push_back(1);
    match_tokens_.push_back(tok);

    size_t distance = old_dist_[index];
    for (core::uint32 i = index; i > 0; i--) old_dist_[i] = old_dist_[i - 1];
    old_dist_[0] = distance;
}

bool Compressor50::need_flush() {
    return token_seq_.size() >= 32768 || input_since_block_ >= 0x80000;
}

void Compressor50::make_tables() {
    std::memset(code_rd_, 0, sizeof(code_rd_));
    std::memset(code_bd_, 0, sizeof(code_bd_));

    // Select table size: RAR7 ExtraDist (win > 4 GiB) uses DCX=80 → 446, else 430
    cur_table_size_ = (win_size_ > (4ULL * 1024 * 1024 * 1024)) ? TABLE_SIZEX : TABLE_SIZE;
    core::uint32 cur_dc = (cur_table_size_ == TABLE_SIZEX) ? DCX : DCB;

    HuffmanBuilder::build_lengths(freq_ld_, NC, 15, len_ld_);
    HuffmanBuilder::build_lengths(freq_dd_, cur_dc, 15, len_dd_);
    HuffmanBuilder::build_lengths(freq_ldd_, LDC, 15, len_ldd_);
    HuffmanBuilder::build_lengths(freq_rd_, RC, 15, len_rd_);

    HuffmanBuilder::build_codes(len_ld_, NC, code_ld_);
    HuffmanBuilder::build_codes(len_dd_, cur_dc, code_dd_);
    HuffmanBuilder::build_codes(len_ldd_, LDC, code_ldd_);
    HuffmanBuilder::build_codes(len_rd_, RC, code_rd_);

    std::memcpy(table_bits_, len_ld_, NC);
    std::memcpy(table_bits_ + NC, len_dd_, cur_dc);
    std::memcpy(table_bits_ + NC + cur_dc, len_ldd_, LDC);
    std::memcpy(table_bits_ + NC + cur_dc + LDC, len_rd_, RC);
}

struct TableItem {
    core::byte sym;
    core::byte extra;
    core::byte bits;
};

static void make_table_items(const core::byte* table_bits, core::uint32 table_size,
                             std::vector<TableItem>& items) {
    items.clear();
    core::uint32 i = 0;
    while (i < table_size) {
        core::byte v = table_bits[i];

        core::uint32 r = 0;
        while (i + r < table_size && table_bits[i + r] == v) r++;

        if (v == 0) {
            if (r >= 3) {
                core::uint32 chunk = r >= 11 ? std::min(r, 138u) : r;
                if (chunk >= 11) {
                    assert(chunk - 11 <= 127 && "RLE code 19 extra field is 7 bits");
                    items.push_back({19, static_cast<core::byte>(chunk - 11), 7});
                } else {
                    assert(chunk - 3 <= 7 && "RLE code 18 extra field is 3 bits");
                    items.push_back({18, static_cast<core::byte>(chunk - 3), 3});
                }
                i += chunk;
            } else {
                items.push_back({v, 0, 0});
                i++;
            }
        } else {
            bool can_repeat = (i > 0 && table_bits[i - 1] == v);
            if (can_repeat && r >= 3) {
                core::uint32 chunk = r >= 11 ? std::min(r, 138u) : r;
                if (chunk >= 11) {
                    assert(chunk - 11 <= 127 && "RLE code 17 extra field is 7 bits");
                    items.push_back({17, static_cast<core::byte>(chunk - 11), 7});
                } else {
                    assert(chunk - 3 <= 7 && "RLE code 16 extra field is 3 bits");
                    items.push_back({16, static_cast<core::byte>(chunk - 3), 3});
                }
                i += chunk;
            } else {
                items.push_back({v, 0, 0});
                i++;
            }
        }
    }
}

void Compressor50::emit_table(BitOutput& local_out) {
    static thread_local std::vector<TableItem> items;
    items.clear();
    // Shrink if capacity is ≥4× the table size (same heuristic as build_lengths)
    if (items.capacity() >= static_cast<size_t>(cur_table_size_) * 4) {
        items.shrink_to_fit();
    }
    make_table_items(table_bits_, cur_table_size_, items);

    std::memset(freq_bd_, 0, sizeof(freq_bd_));
    for (const TableItem& item : items) freq_bd_[item.sym]++;

    HuffmanBuilder::build_lengths(freq_bd_, BC, 15, len_bd_);
    HuffmanBuilder::build_codes(len_bd_, BC, code_bd_);

    // Encode BC bitlengths with RAR5 escape: 15 -> 15+0 literal, 15+ (zc+2) zeros
    for (core::uint32 i = 0; i < BC;) {
        if (len_bd_[i] == 15) {
            local_out.put_bits(15, 4);
            local_out.put_bits(0, 4); // zc==0 -> literal 15
            i++;
        } else if (len_bd_[i] == 0) {
            // Count zero run
            core::uint32 run = 0;
            while (i + run < BC && len_bd_[i + run] == 0 && run < 17) run++;
            if (run >= 3) {
                // Use escape: 15 + (run-2) zeros (need to split if >17)
                core::uint32 chunk = run;
                if (chunk > 17) chunk = 17;
                if (chunk >= 3) {
                    local_out.put_bits(15, 4);
                    assert(chunk - 2 <= 15 && "RLE extra field is 4 bits -> max run 17");
                    local_out.put_bits(chunk - 2, 4);
                    i += chunk;
                } else {
                    local_out.put_bits(0, 4);
                    i++;
                }
            } else {
                local_out.put_bits(0, 4);
                i++;
            }
        } else {
            local_out.put_bits(len_bd_[i], 4);
            i++;
        }
    }

    for (const TableItem& item : items) {
        local_out.put_bits(code_bd_[item.sym], len_bd_[item.sym]);
        local_out.put_bits(item.extra, item.bits);
    }
}

void Compressor50::emit_tokens(BitOutput& local_out) {
    size_t li = 0, mi = 0, fi = 0;
    for (core::byte seq : token_seq_) {
        if (seq == 0) {
            // Literal
            core::byte b = lit_bytes_[li++];
            assert(len_ld_[b] > 0 && "Encoder emitted literal symbol with 0-length code");
            local_out.put_bits(code_ld_[b], len_ld_[b]);
            continue;
        }
        if (seq == 2) {
            // Filter descriptor (slot 256)
            const FilterToken& ft = filter_tokens_[fi++];
            assert(len_ld_[256] > 0 && "Encoder emitted filter symbol with 0-length code");
            local_out.put_bits(code_ld_[256], len_ld_[256]);
            emit_filter_data(local_out, ft.block_start);
            emit_filter_data(local_out, ft.block_length);
            local_out.put_bits(static_cast<core::uint32>(ft.type), 3);
            if (ft.type == 0) {
                local_out.put_bits(ft.channels - 1, 5);
            }
            continue;
        }
        const Compressor50Token& tok = match_tokens_[mi++];
        switch (tok.get_type()) {
        case Compressor50Token::TokenType::Match: {
            core::uint32 len_slot = tok.get_len_slot();
            core::uint32 dist_slot = tok.get_dist_slot();
            core::uint32 sym = 262 + len_slot;

            core::uint32 len_code = code_ld_[sym];
            core::uint32 len_len = len_ld_[sym];
            core::uint32 l_extra = tok.len_extra;
            core::uint32 l_bits = tok.get_len_bits();
            if (len_len + l_bits <= 32) {
                local_out.put_bits((len_code << l_bits) | l_extra, len_len + l_bits);
            } else {
                local_out.put_bits(len_code, len_len);
                local_out.put_bits(l_extra, l_bits);
            }

            core::uint32 dist_code = code_dd_[dist_slot];
            core::uint32 dist_len = len_dd_[dist_slot];
            core::uint32 d_bits = tok.get_dist_bits();
            if (d_bits >= 4) {
                if (d_bits > 4) {
                    core::uint64 extra_high = tok.dist_extra >> 4;
                    core::uint32 extra_high_len = d_bits - 4;
                    if (dist_len + extra_high_len <= 32) {
                        local_out.put_bits(
                            static_cast<core::uint32>((dist_code << extra_high_len) | extra_high),
                            dist_len + extra_high_len);
                    } else {
                        local_out.put_bits(dist_code, dist_len);
                        // extra_high_len can exceed 32 bits for windows above
                        // 16 GiB (d_bits up to 38); put_bits64 keeps the top
                        // bits that the old u32 path silently dropped (H1).
                        local_out.put_bits64(extra_high, extra_high_len);
                    }
                } else {
                    local_out.put_bits(dist_code, dist_len);
                }
                core::uint32 extra_low = static_cast<core::uint32>(tok.dist_extra & 0xf);
                local_out.put_bits(code_ldd_[extra_low], len_ldd_[extra_low]);
            } else {
                if (dist_len + d_bits <= 32) {
                    local_out.put_bits(
                        static_cast<core::uint32>((dist_code << d_bits) | tok.dist_extra),
                        dist_len + d_bits);
                } else {
                    local_out.put_bits(dist_code, dist_len);
                    local_out.put_bits(static_cast<core::uint32>(tok.dist_extra), d_bits);
                }
            }
            break;
        }
        default: { // Rep0..Rep3
            core::uint32 index = static_cast<core::uint32>(tok.get_type()) -
                                 static_cast<core::uint32>(Compressor50Token::TokenType::Rep0);
            core::uint32 sym = 258 + index;
            local_out.put_bits(code_ld_[sym], len_ld_[sym]);

            core::uint32 len_slot = tok.get_len_slot();
            core::uint32 rep_code = code_rd_[len_slot];
            core::uint32 rep_len = len_rd_[len_slot];
            core::uint32 l_extra = tok.len_extra;
            core::uint32 l_bits = tok.get_len_bits();
            if (rep_len + l_bits <= 32) {
                local_out.put_bits((rep_code << l_bits) | l_extra, rep_len + l_bits);
            } else {
                local_out.put_bits(rep_code, rep_len);
                local_out.put_bits(l_extra, l_bits);
            }
            break;
        }
        }
    }
}

void Compressor50::add_filter(const FilterToken& ft) {
    token_seq_.push_back(2);
    filter_tokens_.push_back(ft);
    freq_ld_[256]++;
}

void Compressor50::emit_filter_data(BitOutput& out, core::uint32 val) {
    core::uint32 byte_cnt = 1;
    if (val > 0xFFFFFF) byte_cnt = 4;
    else if (val > 0xFFFF) byte_cnt = 3;
    else if (val > 0xFF) byte_cnt = 2;

    out.put_bits(byte_cnt - 1, 2);
    for (core::uint32 i = 0; i < byte_cnt; ++i) {
        out.put_bits((val >> (i * 8)) & 0xFF, 8);
    }
}

void Compressor50::apply_pending_filter(core::uint64 up_to) {
    (void)up_to;
}

bool Compressor50::write_block(bool last_block) {
    block_mem_.clear();

    BitOutput local_out;
    local_out.set_memory(&block_mem_);

    make_tables();
    emit_table(local_out);
    emit_tokens(local_out);

    local_out.finish_block_data();

    core::uint64 data_size = block_mem_.size();
    if (data_size == 0) {
        // No tokens to emit. Two ways to reach here:
        //   1. write_block(false) called when the buffer was already flushed —
        //      simply drop through and reset state; there is nothing to write
        //      and the previous block already delimits the stream.
        //   2. write_block(true) called on empty input, or immediately after a
        //      non-final flush at EOF — we still need a valid RAR5 block header
        //      so the decoder sees the LastBlock flag and terminates cleanly.
        //      Emit a minimal 3-byte header:
        //        flags     = 0x80 (skip-if-unknown) | 0x40 (LastBlock)
        //                    with byte_count=1 (bits 3..4 = 0) and
        //                    block_bit_size=1 (bits 0..2 = 0)
        //        check_sum = 0x5A XOR flags XOR 0 (block_size low byte)
        //        block_size low byte = 0
        //
        // The previous implementation wrote a single 0x00 byte here, which is
        // not a valid RAR5 block header (block_bit_size decoded as 1 but
        // checksum byte 0x5A ≠ 0x00). Strict decoders flag such archives as corrupt.
        if (!last_block) {
            token_seq_.clear();
            lit_bytes_.clear();
            match_tokens_.clear();
            filter_tokens_.clear();
            input_since_block_ = 0;
            init_freq();
            return true;
        }
        core::byte flags = static_cast<core::byte>(0x80 | 0x40);
        core::byte check_sum = static_cast<core::byte>(0x5A ^ flags);
        core::byte head[3] = {flags, check_sum, 0x00};
        bool wrote_ok = true;
        if (mem_out_ != nullptr) {
            mem_out_->insert(mem_out_->end(), head, head + 3);
        } else if (dest_file_ != nullptr) {
            // Short-write check: a disk-full must fail the stream, not emit a
            // truncated archive with a success return (sweep finding M5).
            wrote_ok = dest_file_->write(head, 3) == 3;
        }
        packed_total_ += 3;
        token_seq_.clear();
        lit_bytes_.clear();
        match_tokens_.clear();
        filter_tokens_.clear();
        input_since_block_ = 0;
        init_freq();
        return wrote_ok;
    }
    // RAR5 block-size field is 24 bits max. A block encoded larger than 16
    // MiB would need its size field truncated to 0xFFFFFF and produce a
    // header whose declared size no longer matches the payload — silent
    // corruption. In normal operation need_flush() fires at ~500 KiB of
    // input and ~32K tokens so this branch is unreachable, but if some
    // future path violates the invariant we refuse to write rather than
    // emit a malformed archive.
    if (data_size >= 0x1000000) {
        assert(false && "Compressor50 block exceeds 16 MiB size ceiling");
        token_seq_.clear();
        lit_bytes_.clear();
        match_tokens_.clear();
        filter_tokens_.clear();
        input_since_block_ = 0;
        init_freq();
        return false;
    }
    core::uint64 total_bits = local_out.get_block_bits();

    assert(total_bits >= (data_size - 1) * 8 && "Block bit size underflow");
    core::uint32 block_bit_size = static_cast<core::uint32>(total_bits - (data_size - 1) * 8);
    if (block_bit_size < 1) block_bit_size = 1;
    if (block_bit_size > 8) block_bit_size = 8;

    core::uint32 byte_count = data_size < 0x100 ? 1 : (data_size < 0x10000 ? 2 : 3);
    core::byte flags = static_cast<core::byte>(0x80 | (last_block ? 0x40 : 0) |
                                               ((byte_count - 1) << 3) | (block_bit_size - 1));
    core::byte check_sum =
        static_cast<core::byte>(0x5a ^ flags ^ data_size ^ (data_size >> 8) ^ (data_size >> 16));

    core::byte head[5];
    head[0] = flags;
    head[1] = check_sum;
    head[2] = static_cast<core::byte>(data_size);
    head[3] = static_cast<core::byte>(data_size >> 8);
    head[4] = static_cast<core::byte>(data_size >> 16);

    bool wrote_ok = true;
    if (mem_out_ != nullptr) {
        mem_out_->insert(mem_out_->end(), head, head + (2 + byte_count));
        mem_out_->insert(mem_out_->end(), block_mem_.begin(), block_mem_.begin() + data_size);
    } else if (dest_file_ != nullptr) {
        wrote_ok = dest_file_->write(head, 2 + byte_count) == 2 + byte_count &&
                   dest_file_->write(block_mem_.data(), data_size) == data_size;
    }

    packed_total_ += 2 + byte_count + data_size;

    token_seq_.clear();
    lit_bytes_.clear();
    match_tokens_.clear();
    filter_tokens_.clear();
    input_since_block_ = 0;
    init_freq();
    return wrote_ok;
}

void Compressor50::init_match_params() {
    static const core::uint32 chains[6] = {0, 4, 8, 32, 128, 512};
    static const core::uint32 nices[6] = {0, 256, 512, 1024, 2048, 4096};
    static const core::uint32 lazies[6] = {0, 0, 0, 1, 1, 1};
    max_chain_ = chains[method_];
    nice_len_ = nices[method_];
    lazy_tests_ = lazies[method_];
}

void Compressor50::slide_window() {
    // Slide the input window once loaded bytes drift past it. External
    // (fully-loaded-memory) sources never slide: buf_data_ points at the
    // caller's buffer rather than buf_, so a pos_base_ shift alone would
    // offset every subsequent byte_at/insert_position/read by that amount
    // (and the memmove below would write move_count bytes into buf_'s
    // empty vector - a heap OOB write). Widening runs past win_size_
    // (e.g. 2 MiB 'A's) hit exactly this path and silently corrupted
    // every subsequent literal.
    core::uint64 retain = std::min(cur_, static_cast<core::uint64>(win_size_));
    core::uint64 new_base = cur_ - retain;
    size_t move_count = static_cast<size_t>(src_loaded_ - new_base);
    std::memmove(&buf_[0], &buf_[static_cast<size_t>(new_base - pos_base_)], move_count);
    pos_base_ = new_base;
}

// One pass of the match/emit loop over everything loaded so far. Shared
// verbatim by the one-shot compress() and streaming feed()/finish_stream();
// the only input-dependent gates read src_loaded_/src_size_/src_eof_, all of
// which streaming maintains with the same invariants (src_size_ ==
// src_loaded_ from the first feed; final=true only after src_eof_).
int Compressor50::process_available(bool final) {
    while (true) {
        load_data(cur_ + 0x80000);

        // L13: the source ended before the declared size (file shrank after
        // sizing, or a read error). The bytes in [src_loaded_, src_size_) were
        // never read - buf_ holds allocated-but-uninitialized memory there,
        // and the literal branch below would pack those bytes into the block
        // while the file header still describes the declared size and CRC.
        // Fail the compression instead. Cannot fire for
        // set_external_buffer()/compress_buffer() sources or for streaming
        // feeds, where src_size_ is always exactly what was loaded.
        if (src_eof_ && src_loaded_ < src_size_) return -1;

        if (cur_ >= src_loaded_) {
            // Everything loaded is consumed. Streaming defers to the next
            // feed() unless final (nothing will ever arrive). One-shot exits:
            // the caller's loop ends when cur_ reaches src_size_, and any
            // further load_data round grows src_loaded_ or sets src_eof_
            // (which L13 or the exit above handles).
            if (streaming_ && !final) return 0;
            break;
        }

        if (!external_buf_ && cur_ >= pos_base_ + buf_size_ - READ_CHUNK) {
            slide_window();
        }

        if (active_filter_ != FilterType::None && filter_emitted_until_ <= cur_ &&
            filter_emitted_until_ < src_size_) {
            size_t max_chunk = 0x100000; // 1 MiB clamp
            if (win_size_ > 0x20000 && win_size_ / 2 < max_chunk) {
                max_chunk = win_size_ / 2;
            } else if (win_size_ <= 0x20000) {
                max_chunk = win_size_ > 4 ? win_size_ / 2 : 1;
            }
            core::uint32 chunk_len = static_cast<core::uint32>(
                std::min<core::uint64>(src_size_ - filter_emitted_until_, max_chunk));
            if (chunk_len > 0) {
                FilterToken ft;
                ft.block_start = 0;
                ft.block_length = chunk_len;
                ft.type = static_cast<core::uint8>(active_filter_);
                ft.channels = filter_channels_;
                add_filter(ft);
                filter_emitted_until_ += chunk_len;
            }
        }

        if (need_flush()) {
            consume_source(unhashed_pos_, cur_);
            unhashed_pos_ = cur_;
            if (!write_block(false)) return -1;
        }

        if (streaming_ && !final && cur_ + STREAM_LOOKAHEAD > src_loaded_) {
            // Byte-identity gate: one-shot processing always sees
            // load_data(cur_ + 0x80000) look-ahead, so matches at cur_ may
            // extend up to 512 KiB into "future" input. Streaming must defer
            // decisions at cur_ until the same look-ahead exists, otherwise
            // emitted matches would be shorter than the one-shot encoder's.
            // (The load quantum 0x80000 must stay in lockstep with the
            // load_data() call above.)
            return 0;
        }
        if (cur_ + 4 > src_loaded_) {
            // One-shot drains inline (the original behavior) - a stall there
            // is always the declared-size tail; short sources fail via L13.
            add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
            cur_++;
            input_since_block_++;
            // Sync unhashed: remaining positions near EOF cannot be hashed (need +4 bytes)
            unhashed_pos_ = cur_;
            continue;
        }

        while (unhashed_pos_ + 7 <= cur_ && unhashed_pos_ + 7 <= src_loaded_) {
            core::uint64 q;
            std::memcpy(&q, &buf_data_[static_cast<size_t>(unhashed_pos_ - pos_base_)], 8);
            core::uint32 h0 = ((static_cast<core::uint32>(q)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h1 =
                ((static_cast<core::uint32>(q >> 8)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h2 =
                ((static_cast<core::uint32>(q >> 16)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h3 =
                ((static_cast<core::uint32>(q >> 24)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            prev_[static_cast<size_t>(unhashed_pos_ & (win_size_ - 1))] = head_[h0];
            head_[h0] = static_cast<core::uint32>(unhashed_pos_ & 0xffffffff);
            prev_[static_cast<size_t>((unhashed_pos_ + 1) & (win_size_ - 1))] = head_[h1];
            head_[h1] = static_cast<core::uint32>((unhashed_pos_ + 1) & 0xffffffff);
            prev_[static_cast<size_t>((unhashed_pos_ + 2) & (win_size_ - 1))] = head_[h2];
            head_[h2] = static_cast<core::uint32>((unhashed_pos_ + 2) & 0xffffffff);
            prev_[static_cast<size_t>((unhashed_pos_ + 3) & (win_size_ - 1))] = head_[h3];
            head_[h3] = static_cast<core::uint32>((unhashed_pos_ + 3) & 0xffffffff);
            unhashed_pos_ += 4;
        }
        // Fill every remaining p < cur_ that can be hashed (eliminates the previous 0-3 byte lag)
        while (unhashed_pos_ < cur_ && unhashed_pos_ + 4 <= src_loaded_) {
            insert_position(unhashed_pos_);
            unhashed_pos_++;
        }

        core::uint32 cur_nice_len = nice_len_;
        size_t cur_max_lz = MAX_LZ_MATCH;
        if (active_filter_ != FilterType::None && filter_emitted_until_ > cur_) {
            size_t rem = static_cast<size_t>(filter_emitted_until_ - cur_);
            if (rem < 2) {
                fail_count_++;
                add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
                insert_position(cur_);
                cur_++;
                input_since_block_++;
                unhashed_pos_ = cur_;
                continue;
            }
            if (rem < cur_nice_len) cur_nice_len = static_cast<core::uint32>(rem);
            if (rem < cur_max_lz) cur_max_lz = rem;
        }

        // FailCount heuristic: skip chain search on long incompressible runs
        core::uint32 search_chain = max_chain_;
        if (fail_count_ > 0x100) {
            core::uint32 skip_mask = 3;
            if (fail_count_ > 0x400) skip_mask = 7;
            if (fail_count_ > 0x800) skip_mask = 15;
            if (fail_count_ > 0x1000) skip_mask = 31;
            if ((fail_count_ & skip_mask) != 0) search_chain = 0;
        }
        MatchInfo best = {0, -1};
        if (search_chain > 0) best = find_match(cur_, search_chain, cur_nice_len);
        if (best.length > cur_max_lz) best.length = cur_max_lz;

        size_t rep_best_len = 0;
        int rep_idx = -1;
        const core::byte* buf_cur = &buf_data_[static_cast<size_t>(cur_ - pos_base_)];
        for (core::uint32 i = 0; i < 4; i++) {
            size_t d = old_dist_[i];
            if (d == static_cast<size_t>(-1) || d == 0 || d > cur_ - pos_base_ || d > max_dist_)
                continue;
            if (buf_cur[0] != buf_cur[-static_cast<core::int64>(d)]) continue;
            size_t len = rep_length(cur_, d, cur_max_lz);
            if (len > rep_best_len) {
                rep_best_len = len;
                rep_idx = static_cast<int>(i);
            }
        }

        bool have_match = false;
        bool is_rep = false;
        size_t len = 0;
        size_t dist = 0;

        if (rep_best_len >= MIN_MATCH && rep_best_len + 1 > best.length) {
            have_match = true;
            is_rep = true;
            len = rep_best_len;
        } else if (best.length >= MIN_MATCH) {
            have_match = true;
            len = best.length;
            dist = static_cast<size_t>(
                best.candidate >= 0 ? cur_ - static_cast<core::uint64>(best.candidate) : 0);
            core::uint32 inc = length_increment(dist);
            if (len < inc + 2) {
                have_match = false;
            } else if (static_cast<core::uint32>(len - inc) > MAX_LZ_MATCH) {
                len = MAX_LZ_MATCH + inc;
            }
        }

        if (!have_match) {
            fail_count_++;
            add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
            insert_position(cur_);
            cur_++;
            input_since_block_++;
            // Keep invariant: every hashable p < cur_ has been inserted exactly once
            unhashed_pos_ = cur_;
        } else {
            if (len >= 8) fail_count_ = 0;
            if (!is_rep && lazy_tests_ > 0 && len < cur_nice_len && avail_at(cur_) > len + 1 &&
                cur_ + 1 < src_size_ &&
                (active_filter_ == FilterType::None || cur_ + 1 < filter_emitted_until_)) {
                MatchInfo next = {0, -1};
                if (search_chain > 0) next = find_match(cur_ + 1, search_chain, cur_nice_len);
                if (next.length > cur_max_lz) next.length = cur_max_lz;

                size_t next_dist = static_cast<size_t>(
                    next.candidate >= 0 ? cur_ + 1 - static_cast<core::uint64>(next.candidate) : 0);
                core::uint32 next_inc = length_increment(next_dist);
                core::uint32 curr_inc = length_increment(dist);

                if ((next.length >= next_inc ? next.length - next_inc : 0) >
                    (len >= curr_inc ? len - curr_inc : 0) + 1) {
                    add_literal(byte_at(cur_));
                    insert_position(cur_);
                    cur_++;
                    input_since_block_++;
                    unhashed_pos_ = cur_;
                    continue;
                }
            }

            if (is_rep) {
                add_rep(static_cast<core::uint32>(rep_idx), len);
            } else {
                add_match(len, dist);
            }

            // Catch-up already inserted every hashable p < old cur_.
            // Insert the match body [old_cur, end) exactly once, then sync unhashed_pos_
            // so the next catch-up never re-inserts the span (prevents self-loops).
            core::uint64 end = cur_ + len;
            if (end > src_loaded_) end = src_loaded_;
            while (cur_ < end) {
                insert_position(cur_);
                cur_++;
            }
            input_since_block_ += len;
            unhashed_pos_ = cur_;
        }
    }
    return 0;
}

core::int64 Compressor50::compress() {
    init_match_params();
    fail_count_ = 0;

    // process_available loads and consumes until cur_ == src_size_ (or fails
    // via L13). The streaming_ flag keeps its stall-point deferring off.
    if (process_available(false) < 0) return -1;

    consume_source(unhashed_pos_, cur_);
    unhashed_pos_ = cur_;
    if (!write_block(true)) return -1;

    return static_cast<core::int64>(packed_total_ - total_at_file_start_);
}

bool Compressor50::compress_buffer(const core::byte* src, size_t src_size,
                                   std::vector<core::byte>& dest, int method, size_t win_size,
                                   const FilterConfig& filter_cfg) {
    if (src_size == 0) {
        dest.clear();
        return true;
    }
    dest.clear();

    // For single-buffer compression, clamp internal compressor window to input size
    // (with 128 KiB floor) to avoid huge memory allocations when win_size is e.g. 4 GiB..64 GiB.
    size_t comp_win_size = win_size;
    if (src_size > 0 && src_size < win_size) {
        size_t pow2_sz = 0x20000;
        while (pow2_sz < src_size && pow2_sz < win_size) {
            pow2_sz <<= 1;
        }
        comp_win_size = std::min(win_size, pow2_sz);
    }

    core::uint8 detected_channels = 1;
    FilterType detected_filter =
        Filters50::detect_filter(src, src_size, detected_channels, filter_cfg);

    if (detected_filter == FilterType::None) {
        if (src_size <= comp_win_size + 0x400000) {
            Compressor50 packer;
            packer.begin_archive(nullptr, method, comp_win_size);
            packer.set_external_buffer(src, src_size);
            packer.set_memory_dest(&dest);
            if (packer.compress() < 0) {
                dest.clear();
                return false;
            }
            return !dest.empty();
        }

        Compressor50 packer;
        packer.init(nullptr, nullptr, method, src_size, comp_win_size);
        packer.mem_src_ptr_ = src;
        packer.mem_src_size_ = src_size;
        packer.mem_src_pos_ = 0;
        packer.set_memory_dest(&dest);
        if (packer.compress() < 0) {
            dest.clear();
            return false;
        }
        return !dest.empty();
    }

    // Filter-assisted compression
    size_t max_chunk = 0x100000; // 1 MiB clamp
    if (win_size > 0x20000 && win_size / 2 < max_chunk) {
        max_chunk = win_size / 2;
    } else if (win_size <= 0x20000) {
        max_chunk = win_size > 4 ? win_size / 2 : 1;
    }

    std::vector<core::byte> filtered(src, src + src_size);
    size_t chunk_start = 0;
    while (chunk_start < src_size) {
        size_t chunk_len = std::min<size_t>(src_size - chunk_start, max_chunk);
        if (detected_filter == FilterType::E8) {
            Filters50::encode_e8(filtered.data() + chunk_start, chunk_len, chunk_start, false);
        } else if (detected_filter == FilterType::E8E9) {
            Filters50::encode_e8(filtered.data() + chunk_start, chunk_len, chunk_start, true);
        } else if (detected_filter == FilterType::Arm) {
            Filters50::encode_arm(filtered.data() + chunk_start, chunk_len, chunk_start);
        } else if (detected_filter == FilterType::Delta) {
            std::vector<core::byte> tmp(chunk_len);
            Filters50::encode_delta(filtered.data() + chunk_start, tmp.data(), chunk_len,
                                   detected_channels);
            std::memcpy(filtered.data() + chunk_start, tmp.data(), chunk_len);
        }
        chunk_start += chunk_len;
    }

    if (src_size <= comp_win_size + 0x400000) {
        Compressor50 packer;
        packer.begin_archive(nullptr, method, comp_win_size);
        packer.set_filter_config(filter_cfg);
        packer.set_active_filter(detected_filter, detected_channels);
        packer.set_external_buffer(filtered.data(), src_size);
        packer.set_memory_dest(&dest);
        if (packer.compress() < 0) {
            dest.clear();
            return false;
        }
        return !dest.empty();
    }

    Compressor50 packer;
    packer.init(nullptr, nullptr, method, src_size, comp_win_size);
    packer.set_filter_config(filter_cfg);
    packer.set_active_filter(detected_filter, detected_channels);
    packer.mem_src_ptr_ = filtered.data();
    packer.mem_src_size_ = src_size;
    packer.mem_src_pos_ = 0;
    packer.set_memory_dest(&dest);
    if (packer.compress() < 0) {
        dest.clear();
        return false;
    }
    return !dest.empty();
}

} // namespace openrar::compress
