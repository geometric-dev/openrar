#include "compressor50.hpp"
#include "arch/match_simd.hpp"
#include "../core/thread_pool.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <memory>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace openrar::compress {

namespace {
// H-11: floor(log2(v)) with floor_log2(0) == floor_log2(1) == 0, i.e. exactly
// the `while (v > 1) { v >>= 1; ++n; }` loops it replaces, in one bit scan.
inline core::uint32 floor_log2(core::uint64 v) {
    v |= 1; // log2(0) := 0, and keeps the scan's input non-zero
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM64))
    unsigned long idx;
    _BitScanReverse64(&idx, v);
    return static_cast<core::uint32>(idx);
#elif defined(_MSC_VER)
    unsigned long idx;
    const unsigned long hi = static_cast<unsigned long>(v >> 32);
    if (hi != 0) {
        _BitScanReverse(&idx, hi);
        return static_cast<core::uint32>(idx) + 32;
    }
    _BitScanReverse(&idx, static_cast<unsigned long>(v));
    return static_cast<core::uint32>(idx);
#elif defined(__GNUC__) || defined(__clang__)
    return 63u - static_cast<core::uint32>(__builtin_clzll(v));
#else
    core::uint32 n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
#endif
}
} // namespace

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

    constexpr core::uint32 MAX_STACK_SYMS = 512;
    if (size <= MAX_STACK_SYMS) {
        struct TreeNode {
            core::uint64 weight;
            core::int16 child_l, child_r;
            core::int16 symbol;
            core::int16 pad;
        };
        TreeNode tree[2 * MAX_STACK_SYMS];
        core::uint16 q1[MAX_STACK_SYMS];
        core::uint16 q2[MAX_STACK_SYMS];
        core::uint16 depths[MAX_STACK_SYMS] = {};
        struct StackEntry {
            core::uint16 node;
            core::uint16 depth;
        };
        StackEntry stack[2 * MAX_STACK_SYMS];
        core::uint16 bl_count[MAX_STACK_SYMS + 1] = {};
        core::uint16 sorted_syms[MAX_STACK_SYMS];

        core::uint32 tree_sz = 0;
        core::uint32 q1_sz = 0;
        core::uint32 q2_sz = 0;

        for (core::uint32 i = 0; i < size; i++) {
            if (freq[i] != 0) {
                tree[tree_sz] = {freq[i], -1, -1, static_cast<core::int16>(i), 0};
                q1[q1_sz++] = static_cast<core::uint16>(tree_sz++);
            }
        }

        std::sort(q1, q1 + q1_sz, [&](core::uint16 a, core::uint16 b) {
            if (tree[a].weight != tree[b].weight) return tree[a].weight < tree[b].weight;
            return tree[a].symbol < tree[b].symbol;
        });

        core::uint32 i1 = 0, i2 = 0;
        auto get_min = [&]() -> core::uint16 {
            if (i1 < q1_sz && (i2 == q2_sz || tree[q1[i1]].weight <= tree[q2[i2]].weight)) {
                return q1[i1++];
            }
            return q2[i2++];
        };

        while (q1_sz - i1 + q2_sz - i2 > 1) {
            core::uint16 left = get_min();
            core::uint16 right = get_min();
            tree[tree_sz] = {tree[left].weight + tree[right].weight, static_cast<core::int16>(left),
                             static_cast<core::int16>(right), -1, 0};
            q2[q2_sz++] = static_cast<core::uint16>(tree_sz++);
        }

        core::uint16 root = static_cast<core::uint16>(tree_sz - 1);
        core::uint32 stack_sz = 0;
        stack[stack_sz++] = {root, 0};

        core::uint32 max_depth = 0;
        while (stack_sz > 0) {
            StackEntry entry = stack[--stack_sz];
            core::uint16 node_idx = entry.node;
            core::uint16 d = entry.depth;
            if (tree[node_idx].symbol != -1) {
                depths[tree[node_idx].symbol] = d;
                if (d > max_depth) max_depth = d;
            } else {
                stack[stack_sz++] = {static_cast<core::uint16>(tree[node_idx].child_r),
                                     static_cast<core::uint16>(d + 1)};
                stack[stack_sz++] = {static_cast<core::uint16>(tree[node_idx].child_l),
                                     static_cast<core::uint16>(d + 1)};
            }
        }

        if (max_depth > max_bits) {
            for (core::uint32 i = 0; i < size; ++i) {
                if (depths[i] > 0) bl_count[depths[i]]++;
            }

            core::uint32 overflow = 0;
            for (core::uint32 d = max_bits + 1; d <= max_depth; ++d) {
                overflow += bl_count[d];
                bl_count[d] = 0;
            }
            bl_count[max_bits] = static_cast<core::uint16>(bl_count[max_bits] + overflow);

            core::uint32 kraft_limit = 1u << max_bits;
            core::uint32 kraft_sum = 0;
            for (core::uint32 d = 1; d <= max_bits; ++d) {
                kraft_sum += static_cast<core::uint32>(bl_count[d]) << (max_bits - d);
            }

            while (kraft_sum > kraft_limit) {
                core::uint32 d = max_bits - 1;
                while (d > 0 && bl_count[d] == 0) d--;
                if (d == 0 || bl_count[max_bits] == 0) break;

                bl_count[d]--;
                bl_count[d + 1] += 2;
                bl_count[max_bits]--;
                kraft_sum -= 1;
            }

            core::uint32 sorted_sz = 0;
            for (core::uint32 i = 0; i < size; ++i) {
                if (depths[i] > 0) sorted_syms[sorted_sz++] = static_cast<core::uint16>(i);
            }
            std::sort(sorted_syms, sorted_syms + sorted_sz, [&](core::uint16 a, core::uint16 b) {
                if (freq[a] != freq[b]) return freq[a] > freq[b];
                return a < b;
            });

            core::uint32 sym_idx = 0;
            for (core::uint32 bits = 1; bits <= max_bits; ++bits) {
                for (core::uint32 i = 0; i < bl_count[bits]; ++i) {
                    assert(sym_idx < sorted_sz &&
                           "bl_count overruns sorted_syms — Kraft invariant broken");
                    depths[sorted_syms[sym_idx++]] = static_cast<core::uint16>(bits);
                }
            }
        }

        for (core::uint32 i = 0; i < size; ++i) {
            lengths[i] = static_cast<core::byte>(depths[i]);
        }
        return;
    }

    // Fallback for alphabets > 512 symbols
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
            depths[tree[node_idx].symbol] = d;
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

        core::uint32 overflow = 0;
        for (core::uint32 d = max_bits + 1; d <= max_depth; ++d) {
            overflow += bl_count[d];
            bl_count[d] = 0;
        }
        bl_count[max_bits] += overflow;

        core::uint32 kraft_limit = 1u << max_bits;
        core::uint32 kraft_sum = 0;
        for (core::uint32 d = 1; d <= max_bits; ++d) {
            kraft_sum += bl_count[d] << (max_bits - d);
        }

        while (kraft_sum > kraft_limit) {
            core::uint32 d = max_bits - 1;
            while (d > 0 && bl_count[d] == 0) d--;
            if (d == 0 || bl_count[max_bits] == 0) break;

            bl_count[d]--;
            bl_count[d + 1] += 2;
            bl_count[max_bits]--;
            kraft_sum -= 1;
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
    crc_pos_ = 0; // OPT-03: Reset CRC cursor alongside unhashed_pos_
    mem_out_ = nullptr;
    token_seq_.clear();
    lit_bytes_.clear();
    match_tokens_.clear();
    filter_tokens_.clear();
    active_filter_ = FilterType::None;
    filter_channels_ = 1;
    filter_emitted_until_ = 0;
    filter_pretransformed_ = false;
    file_start_pos_ = 0;
    is_large_window_ = false;
    head_.clear();
    prev_.clear();
    head64_.clear();
    prev64_.clear();
    reset_old_dist();
    hash_crc32_.reset();
    init_freq();
    init_price_model();
}

Compressor50::Compressor50() {
    // v1.44 A-6 measurement escape hatch (read once per process): same role as
    // the legacy knobs — the shipped default is the back-shift ON; the knob
    // exists so the same binary can A/B its own selection (exact
    // pre-v1.44 bytes) for timing/size verification.
    if (const char* s = std::getenv("OPENRAR_NO_BACKSHIFT");
        s && s[0] != '\0' && std::strcmp(s, "0") != 0) {
        back_shift_enable_ = false;
    }
    reset_state();
}

namespace {
size_t normalize_window_size(size_t win_size, core::uint64* out_max_dist = nullptr) {
    const core::uint64 sz_cap = sizeof(size_t) < 8 ? (1ULL << 30) : 0x1000000000ULL;
    const core::uint64 max_retain = std::min<core::uint64>(
        sz_cap, static_cast<core::uint64>(static_cast<size_t>(-1)) - 0x1000000);
    const core::uint64 max_dist = std::min(static_cast<core::uint64>(win_size), max_retain);
    if (out_max_dist) *out_max_dist = max_dist;
    size_t ws = static_cast<size_t>(max_dist == 0 ? 0x20000 : max_dist);
    size_t pow2_ws = 0x10000;
    while (pow2_ws < ws && pow2_ws < static_cast<size_t>(max_retain)) {
        pow2_ws <<= 1;
    }
    return std::max<size_t>(0x10000, pow2_ws);
}
} // namespace

void Compressor50::begin_archive(io::FileStream* dest, int method, size_t win_size) {
    // Wipe every scalar first. Without this, a Compressor50 that was
    // previously used with set_external_buffer() or as the memory-input
    // packer inside compress_buffer() would retain external_buf_==true or
    // stale mem_src_* pointers, and load_data() would short-circuit or
    // memcpy from a dangling source on the next session.
    reset_state();
    dest_file_ = dest;
    method_ = method < 1 || method > 5 ? 5 : method;

    win_size_ = normalize_window_size(win_size, &max_dist_);
    assert((win_size_ & (win_size_ - 1)) == 0);
    // 430-entry tables (see make_tables) reach at most slot 63: distance
    // 1 + (3 << 30) + (2^30 - 1) = 4 GiB. A larger window still helps (the
    // whole history stays addressable within 4 GiB of every position), but
    // no match may reach further back.
    max_dist_ = std::min<core::uint64>(max_dist_, 4ULL * 1024 * 1024 * 1024);

    buf_size_ = win_size_ + 0x400000 + READ_CHUNK;
    buf_data_ = buf_.empty() ? nullptr : buf_.data();

    if (token_seq_.capacity() < 32768) token_seq_.reserve(32768);
    if (lit_bytes_.capacity() < 32768) lit_bytes_.reserve(32768);
    // M-07: need_flush() fires at token_seq_.size() >= 32768, and every
    // token can be a match, so match_tokens_ needs the same headroom to
    // avoid a mid-block reallocation.
    if (match_tokens_.capacity() < 32768) match_tokens_.reserve(32768);

    is_large_window_ = (win_size_ > 4ULL * 1024 * 1024 * 1024);
    if (is_large_window_) {
        head_.clear();
        prev_.clear();
        head64_.assign(HASH_SIZE, static_cast<core::uint64>(-1));
        prev64_.resize(win_size_);
    } else {
        head64_.clear();
        prev64_.clear();
        head_.assign(HASH_SIZE, static_cast<core::uint32>(-1));
        prev_.resize(win_size_);
    }

    cur_ = 0;
    pos_base_ = 0;
    src_loaded_ = 0;
    src_eof_ = false;
    reset_old_dist();
    init_freq();
}

// -- Streaming session (Path A) ----------------------------------------------
bool Compressor50::begin_stream(int method, size_t win_size, bool continue_window) {
    if (method < 1 || method > 5) return false; // STORE (0) never streams - passthrough
    if (win_size == 0) win_size = 0x200000;
    if (continue_window) {
        // A carried window is only defined directly after a finished stream
        // of the same geometry: buf_/cur_/pos_base_/hash chains/old_dist_
        // must belong to bytes the decoder will carry for this chain.
        if (!streaming_ || !stream_finished_ || win_size_ != normalize_window_size(win_size)) {
            return false;
        }
    }
    streaming_ = true;
    stream_finished_ = false;
    if (continue_window) {
        // Keep: buf_, cur_, pos_base_, src_loaded_, head_/prev_/prev64_,
        // old_dist_ (the decoder carries rep distances across solid members).
        // Reset: per-file token/frequency/hash/filter state.
        src_size_ = src_loaded_; // already-consumed history; feed() extends it
        token_seq_.clear();
        lit_bytes_.clear();
        match_tokens_.clear();
        filter_tokens_.clear();
        filter_emitted_until_ = cur_;
        input_since_block_ = 0;
        total_at_file_start_ = packed_total_;
        hash_crc32_.reset();
        fail_count_ = 0;
        init_match_params();
        active_filter_ = FilterType::None; // detection fires only at cur_ == 0
        filter_pretransformed_ = false;
        file_start_pos_ = cur_;
        unhashed_pos_ = cur_;
        crc_pos_ = cur_;
        return true;
    }
    begin_archive(nullptr, method, win_size);
    src_size_ = 0; // grows with every feed: every loaded byte is valid input
    init_match_params();
    fail_count_ = 0;
    return true;
}

int Compressor50::feed(const core::byte* data, size_t n) {
    if (!streaming_ || stream_finished_) return -1;
    if (!data && n != 0) return -1;
    if (active_filter_ == FilterType::None && filter_config_.mode != FilterMode::DisableAll &&
        cur_ == 0 && n >= 64) {
        active_filter_ = Filters50::detect_filter(data, n, filter_channels_, filter_config_);
    }
    if (n > 0) ensure_internal_buffer();
    const core::byte* p = data;
    size_t left = n;
    while (left > 0) {
        size_t used = static_cast<size_t>(src_loaded_ - pos_base_);
        size_t space = buf_size_ > used ? buf_size_ - used : 0;
        if (space == 0) {
            // Cannot normally happen: process_available keeps the unprocessed
            // gap at <= 4 bytes, so used <= win_size_ + 4 < buf_size_ after
            // every pass. Slide defensively rather than overflow.
            advance_crc(cur_);
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
    advance_crc(cur_);
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
    external_buf_ = false;
    mem_src_ptr_ = nullptr;
    mem_src_size_ = 0;
    mem_src_pos_ = 0;
    active_filter_ = FilterType::None;
    filter_channels_ = 1;
    filter_pretransformed_ = false;
    // A member's compressed stream must open with its own tables. Our decoder
    // drops them at a non-solid start (decompressor50.cpp:568), and third-party
    // tools reject the solid-mixed interop track outright if we reuse across a
    // SOLID boundary too - each member is its own block sequence as far as the
    // format is concerned. write_block always emits a description regardless.
    token_seq_.clear();
    lit_bytes_.clear();
    match_tokens_.clear();
    filter_tokens_.clear();
    filter_emitted_until_ = 0;
    input_since_block_ = 0;
    total_at_file_start_ = packed_total_;
    hash_crc32_.reset();

    if (!continue_window) {
        cur_ = 0;
        pos_base_ = 0;
        src_loaded_ = 0;
        reset_old_dist();
        file_start_pos_ = 0;
        filter_emitted_until_ = 0;
    } else {
        src_size_ = src_loaded_ + src_file_size;
        file_start_pos_ = cur_;
        filter_emitted_until_ = cur_;
    }
    unhashed_pos_ = cur_;
    crc_pos_ = cur_;
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
    init_freq();
}

void Compressor50::init_freq() {
    std::memset(freq_ld_, 0, sizeof(freq_ld_));
    std::memset(freq_dd_, 0, sizeof(freq_dd_));
    std::memset(freq_ldd_, 0, sizeof(freq_ldd_));
    std::memset(freq_rd_, 0, sizeof(freq_rd_));
    std::memset(freq_bd_, 0, sizeof(freq_bd_));
}

namespace {
inline constexpr core::uint32 DP_INF = 0xFFFFFFFFu;
inline core::uint32 price_sym(core::byte len) {
    return len ? (static_cast<core::uint32>(len) << 8) : (15u << 8);
}
} // namespace

void Compressor50::init_price_model() {
    for (core::uint32 i = 0; i < 256; i++) {
        price_model_.lit_cost[i] = 8u << 8;
    }
    for (core::uint32 i = 0; i < 44; i++) {
        price_model_.len_cost[i] = (i < 8 ? 5u : 7u) << 8;
    }
    for (core::uint32 i = 0; i < DCX; i++) {
        price_model_.dist_cost[i] = (i < 4 ? 4u : (i < 16 ? 8u : 14u)) << 8;
    }
    for (core::uint32 r = 0; r < 4; r++) {
        for (core::uint32 s = 0; s < RC; s++) {
            price_model_.rep_cost[r][s] = ((3u + r) + (s < 8 ? 4u : 6u)) << 8;
        }
    }
    price_model_.slot257_cost = 3u << 8;
    price_model_.initialized = false;
}

void Compressor50::update_price_model() {
    price_model_.initialized = true;
    for (core::uint32 i = 0; i < 256; i++) {
        price_model_.lit_cost[i] = price_sym(len_ld_[i]);
    }
    for (core::uint32 i = 0; i < 44; i++) {
        price_model_.len_cost[i] = price_sym(len_ld_[262 + i]);
    }
    core::uint32 cur_dc = (cur_table_size_ == TABLE_SIZEX) ? DCX : DCB;
    for (core::uint32 i = 0; i < cur_dc; i++) {
        price_model_.dist_cost[i] = price_sym(len_dd_[i]);
    }
    for (core::uint32 r = 0; r < 4; r++) {
        core::uint32 r_sym_cost = price_sym(len_ld_[258 + r]);
        for (core::uint32 s = 0; s < RC; s++) {
            price_model_.rep_cost[r][s] = r_sym_cost + price_sym(len_rd_[s]);
        }
    }
    price_model_.slot257_cost = price_sym(len_ld_[257]);
}

core::uint32 Compressor50::price_literal(core::byte b) const {
    return price_model_.initialized ? price_model_.lit_cost[b] : (8u << 8);
}

core::uint32 Compressor50::price_match(size_t len, size_t dist) const {
    if (!price_model_.initialized) return (len >= 3) ? (14u << 8) : DP_INF;
    core::uint32 inc = length_increment(dist);
    if (len < static_cast<size_t>(inc) + 2) return DP_INF;
    core::uint32 base = static_cast<core::uint32>(len - inc);
    if (base > MAX_LZ_MATCH) base = MAX_LZ_MATCH;

    core::uint32 len_slot, len_extra, len_bits;
    length_to_slot(base, len_slot, len_extra, len_bits);
    if (len_slot >= 44) return DP_INF;

    core::uint32 dist_slot, raw_bits;
    core::uint64 dist_extra;
    distance_to_slot(dist, dist_slot, dist_extra, raw_bits);
    if (dist_slot >= DCX) return DP_INF;

    core::uint32 cost = price_model_.len_cost[len_slot] + (len_bits << 8);
    cost += price_model_.dist_cost[dist_slot];
    if (raw_bits >= 4) {
        cost += ((raw_bits - 4) << 8);
        core::uint32 low = static_cast<core::uint32>(dist_extra & 0x0F);
        cost += price_sym(len_ldd_[low]);
    } else {
        cost += (raw_bits << 8);
    }
    return cost;
}

core::uint32 Compressor50::price_rep(core::uint32 rep_idx, size_t len) const {
    if (!price_model_.initialized) return (len >= 2 && rep_idx < 4) ? (7u << 8) : DP_INF;
    if (len < 2 || rep_idx >= 4) return DP_INF;
    core::uint32 base = static_cast<core::uint32>(len);
    if (base > MAX_LZ_MATCH) base = MAX_LZ_MATCH;

    core::uint32 len_slot, len_extra, len_bits;
    length_to_slot(base, len_slot, len_extra, len_bits);
    if (len_slot >= 44) return DP_INF;  // Length slots are 0..43 from length_to_slot()

    return price_model_.rep_cost[rep_idx][len_slot] + (len_bits << 8);
}

core::uint32 Compressor50::price_slot257() const {
    return price_model_.initialized ? price_model_.slot257_cost : (3u << 8);
}

// ── v2.1.0 Forward-Arrival Horizon Engine (H = 16 bytes) ────────────────────────
// Evaluates all token candidates at the current position and selects the optimal
// one using dynamic programming over a 17-element arrival buffer (offsets 0..16).
// Returns the optimal token decision (token_type, token_len, token_dist, cost).
// Takes pre-computed match/rep candidates to avoid duplicate search.
// Uses local arrival buffer to avoid member variable issues.
// Modified evaluate_forward_arrival function with proper DP
// This is a standalone replacement for the function in compressor50.cpp

Compressor50::ForwardArrivalDecision Compressor50::evaluate_forward_arrival(
    core::uint64 cur, size_t match_len, core::int64 match_candidate,
    size_t rep_best_len, int rep_idx, size_t cur_max_lz) {
    // Local arrival buffer to avoid member variable issues
    struct LocalArrivalNode {
        core::uint32 cost{0xFFFFFFFFu};
        core::uint8 from_offset{0};
        core::uint8 token_type{0};
        core::uint16 token_len{0};
        core::uint32 token_dist{0};
        size_t old_dist[4];
        size_t last_len{0};
    };
    constexpr int H = 16;
    LocalArrivalNode arrival[H + 1];

    // Reset arrival buffer
    for (int i = 0; i <= H; ++i) {
        arrival[i].cost = DP_INF;
        arrival[i].from_offset = 0;
        arrival[i].token_type = 0;
        arrival[i].token_len = 0;
        arrival[i].token_dist = 0;
        for (int d = 0; d < 4; ++d) arrival[i].old_dist[d] = old_dist_[d];
        arrival[i].last_len = last_length_;
    }
    arrival[0].cost = 0;

    const core::byte* buf_cur = &buf_data_[static_cast<size_t>(cur - pos_base_)];
    size_t avail = avail_at(cur);
    if (avail > cur_max_lz) avail = cur_max_lz;

    // Offset 0: evaluate all candidates
    // Helper to store candidate at clamped horizon index while preserving actual token length
    auto store_candidate = [&](int token_type, size_t token_len, core::uint32 token_dist,
                               core::uint32 cost, const size_t* old_dist_src, size_t last_len_src) {
        int idx = token_len > H ? H : static_cast<int>(token_len);
        if (cost < arrival[idx].cost) {
            arrival[idx].cost = cost;
            arrival[idx].from_offset = 0;
            arrival[idx].token_type = static_cast<core::uint8>(token_type);
            arrival[idx].token_len = static_cast<core::uint16>(token_len);
            arrival[idx].token_dist = token_dist;
            for (int d = 0; d < 4; ++d) arrival[idx].old_dist[d] = old_dist_src[d];
            arrival[idx].last_len = last_len_src;
        }
    };

    // 1. Literal
    core::uint32 lit_cost = price_literal(buf_cur[0]);
    store_candidate(0, 1, 0, lit_cost, old_dist_, last_length_);

    // 2. Rep0..3 (token types 2..5 match Compressor50Token::TokenType)
    if (rep_best_len >= 2 && rep_idx >= 0) {
        // Wire-legal floor for rep: len >= 2 (already checked by rep_best_len >= 2)
        core::uint32 cost = price_rep(static_cast<core::uint32>(rep_idx), rep_best_len);
        store_candidate(2 + rep_idx, rep_best_len, static_cast<core::uint32>(rep_idx),
                        cost, old_dist_, last_length_);
    }

    // 3. Slot 257 / LastLen (token type 6 matches Compressor50Token::TokenType::LastLen)
    if (last_length_ != 0 && rep_best_len == static_cast<size_t>(last_length_) && rep_idx == 0) {
        core::uint32 cost = price_slot257();
        store_candidate(6, rep_best_len, 0, cost, old_dist_, last_length_);
    }

    // 4. Fresh match (use pre-computed match)
    if (match_len >= 3 && match_candidate >= 0) {
        size_t mlen = match_len;
        size_t mdist = static_cast<size_t>(cur - static_cast<core::uint64>(match_candidate));
        if (mlen > avail) mlen = avail;
        // Wire-legal floor: fresh match requires len >= inc + 2
        core::uint32 inc = length_increment(mdist);
        if (mlen >= static_cast<size_t>(inc) + 2) {
            core::uint32 cost = price_match(mlen, mdist);
            store_candidate(1, mlen, static_cast<core::uint32>(mdist), cost, old_dist_, last_length_);
        }
    }

    // ── Forward DP over horizon H ────────────────────────────────────────────────
    // Propagate best costs through offsets 0..H-1 using buffer lookahead.
    for (int i = 0; i < H; ++i) {
        if (arrival[i].cost == DP_INF) continue;  // unreachable offset

        core::uint64 pos = cur + i;
        size_t avail_at_pos = avail_at(pos);
        if (avail_at_pos == 0) continue;
        if (avail_at_pos > cur_max_lz) avail_at_pos = cur_max_lz;

        const core::byte* buf_pos = &buf_data_[static_cast<size_t>(pos - pos_base_)];
        size_t avail = avail_at_pos;

        // Evaluate all token candidates starting at pos
        // For i == 0, pre-computed match/rep data already stored in arrival[1..H]
        // For i > 0, use buffer lookahead (no fresh match finding)

        // 1. Literal
        if (avail >= 1) {
            core::uint32 lit_cost = price_literal(buf_pos[0]);
            core::uint32 new_cost = arrival[i].cost + lit_cost;
            int j = i + 1 > 16 ? 16 : i + 1;
            if (new_cost < arrival[j].cost) {
                arrival[j].cost = new_cost;
                arrival[j].from_offset = i;
                arrival[j].token_type = 0;
                arrival[j].token_len = 1;
                arrival[j].token_dist = 0;
                for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                arrival[j].last_len = arrival[i].last_len;
            }
        }

        // 2. Rep0..3
        for (int r = 0; r < 4; ++r) {
            size_t d = arrival[i].old_dist[r];
            if (d != static_cast<size_t>(-1) && d != 0 && d <= pos - pos_base_ && d <= max_dist_) {
                if (avail >= 2 && buf_pos[0] == buf_pos[-static_cast<core::int64>(d)]) {
                    size_t rep_len = rep_length(pos, d, avail);
                    if (rep_len >= 2 && rep_len <= cur_max_lz) {
                        core::uint32 cost = price_rep(r, rep_len);
                        core::uint32 new_cost = arrival[i].cost + cost;
                        int j = i + static_cast<int>(rep_len) > 16 ? 16 : i + static_cast<int>(rep_len);
                        if (new_cost < arrival[j].cost) {
                            arrival[j].cost = new_cost;
                            arrival[j].from_offset = i;
                            arrival[j].token_type = static_cast<core::uint8>(2 + r);
                            arrival[j].token_len = static_cast<core::uint16>(rep_len);
                            arrival[j].token_dist = static_cast<core::uint32>(r);
                            for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                            arrival[j].last_len = arrival[i].last_len;
                        }
                    }
                }
            }

        // 3. Slot 257 / LastLen
        if (arrival[i].last_len != 0) {
            size_t d = arrival[i].old_dist[0];
            if (d != static_cast<size_t>(-1) && d != 0 && d <= pos - pos_base_ && d <= max_dist_) {
                if (avail >= arrival[i].last_len && buf_pos[0] == buf_pos[-static_cast<core::int64>(d)]) {
                    size_t rep0_len = rep_length(pos, d, avail);
                    if (rep0_len == arrival[i].last_len && rep0_len <= cur_max_lz) {
                        core::uint32 cost = price_slot257();
                        core::uint32 new_cost = arrival[i].cost + cost;
                        int j = i + static_cast<int>(rep0_len) > 16 ? 16 : i + static_cast<int>(rep0_len);
                        if (new_cost < arrival[j].cost) {
                            arrival[j].cost = new_cost;
                            arrival[j].from_offset = i;
                            arrival[j].token_type = 6;
                            arrival[j].token_len = static_cast<core::uint16>(rep0_len);
                            arrival[j].token_dist = 0;
                            for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                            arrival[j].last_len = arrival[i].last_len;
                        }
                    }
                }
            }

        // 4. Fresh match - at all offsets within horizon
        // For i == 0, use pre-computed match. For i > 0, do limited lookahead search.
        size_t mlen = 0;
        core::int64 mcand = -1;
        if (i == 0) {
            // Use pre-computed match
            mlen = match_len;
            mcand = match_candidate;
        } else {
            // Limited lookahead match finding at offset i > 0
            // Use reduced chain depth for performance
            size_t lookahead_max_lz = cur_max_lz;
            if (lookahead_max_lz > 256) lookahead_max_lz = 256;  // Cap for lookahead
            MatchInfo mi = find_match(pos, 16, 16);  // Limited chain/nice for speed
            if (mi.length >= 3 && mi.candidate >= 0) {
                mlen = mi.length;
                mcand = mi.candidate;
            }
        }
        if (mlen >= 3 && mcand >= 0) {
            if (mlen > avail) mlen = avail;
            core::uint32 inc = length_increment(static_cast<size_t>(pos - static_cast<core::uint64>(mcand)));
            if (mlen >= static_cast<size_t>(inc) + 2) {
                core::uint32 cost = price_match(mlen, static_cast<size_t>(pos - static_cast<core::uint64>(mcand)));
                core::uint32 new_cost = arrival[i].cost + cost;
                int j = i + static_cast<int>(mlen) > 16 ? 16 : i + static_cast<int>(mlen);
                if (new_cost < arrival[j].cost) {
                    arrival[j].cost = new_cost;
                    arrival[j].from_offset = i;
                    arrival[j].token_type = 1;
                    arrival[j].token_len = static_cast<core::uint16>(mlen);
                    arrival[j].token_dist = static_cast<core::uint32>(static_cast<size_t>(pos - static_cast<core::uint64>(mcand)));
                    for (int d = 0; d < 4; ++d) arrival[j].old_dist[d] = arrival[i].old_dist[d];
                    arrival[j].last_len = arrival[i].last_len;
                }
            }
        }
    }
    // ────────────────────────────────────────────────────────────────────────

    // ── Select optimal path: minimum total cost to any reachable offset ──
    // We find the minimum cost to reach ANY reachable offset, then trace back
    // to find the first token of that optimal path.
    core::uint32 best_cost = DP_INF;
    int best_target = -1;
    for (int t = 1; t <= H; ++t) {
        if (arrival[t].cost < best_cost) {
            best_cost = arrival[t].cost;
            best_target = t;
        }
    }
    if (best_target == -1) {
        // Fallback: literal (should not happen)
        return {0, 1, 0, lit_cost};
    }

    // Nice-length cutoff: if the optimal path's first token is competitive, return immediately
    // We need to trace back first to find the first token
    int cur_off = best_target;
    int first_token_type = 0;
    core::uint16 first_token_len = 0;
    core::uint32 first_token_dist = 0;
    while (cur_off > 0) {
        int prev = arrival[cur_off].from_offset;
        if (prev == 0) {
            first_token_type = arrival[cur_off].token_type;
            first_token_len = arrival[cur_off].token_len;
            first_token_dist = arrival[cur_off].token_dist;
            break;
        }
        cur_off = prev;
    }
    if (first_token_len == 0) {
        // Fallback: literal
        return {0, 1, 0, lit_cost};
    }

    // Nice-length cutoff: if the first token is a long match, commit
    size_t first_token_len_val = first_token_len;
    if (first_token_len_val >= nice_len_ ||
        (first_token_len_val >= 32 && first_token_dist <= 256)) {
        return {static_cast<core::uint32>(first_token_type),
                first_token_len,
                first_token_dist,
                best_cost};
    }

    // For now, return the first token of the optimal path
    // Future: implement recursive offset 1 expansion for true lookahead
    return {static_cast<core::uint32>(first_token_type),
            first_token_len,
            first_token_dist,
            best_cost};
}
}
}

void Compressor50::commit_arrival_token(const ForwardArrivalDecision& decision) {
    switch (decision.token_type) {
    case 0:  // Literal
        add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
        insert_position(cur_);
        cur_++;
        input_since_block_++;
        unhashed_pos_ = cur_;
        break;
    case 1:  // Match
        {
            // add_match returns the actual encoded length (may be shorter due to truncation/admissibility)
            size_t actual_len = add_match(decision.token_len, decision.token_dist);
            if (actual_len == 0) {
                // Match was filtered out (inadmissible), fall back to literal
                add_literal(buf_data_[static_cast<size_t>(cur_ - pos_base_)]);
                insert_position(cur_);
                cur_++;
                input_since_block_++;
                unhashed_pos_ = cur_;
            } else {
                core::uint64 end = cur_ + actual_len;
                if (end > src_loaded_) end = src_loaded_;
                hash_range(cur_, end);
                cur_ = end;
                input_since_block_ += actual_len;
                unhashed_pos_ = cur_;
            }
        }
        break;
    case 2:  // Rep0
    case 3:  // Rep1
    case 4:  // Rep2
    case 5:  // Rep3
        add_rep(decision.token_type - 2, decision.token_len);
        {
            core::uint64 end = cur_ + decision.token_len;
            if (end > src_loaded_) end = src_loaded_;
            hash_range(cur_, end);
            cur_ = end;
            input_since_block_ += decision.token_len;
            unhashed_pos_ = cur_;
        }
        break;
    case 6:  // LastLen (slot 257)
        add_last_len();
        {
            // 257 covers exactly last_length_ bytes
            core::uint64 end = cur_ + decision.token_len;
            if (end > src_loaded_) end = src_loaded_;
            hash_range(cur_, end);
            cur_ = end;
            input_since_block_ += decision.token_len;
            unhashed_pos_ = cur_;
        }
        break;
    }
}

void Compressor50::load_data(core::uint64 until) {
    if (external_buf_) return;
    if (streaming_) return; // feed() copies bytes into buf_ directly
    if (until > src_size_) until = src_size_;
    if (until <= src_loaded_) return;
    ensure_internal_buffer();

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
    // C-07: little-endian value on every host, so hash_range()'s shifted
    // extraction below inserts the same hashes and output is host-independent.
    const core::uint32 val = core::load_le32(d);
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

inline void Compressor50::hash_range(core::uint64 from, core::uint64 to) {
    if (!is_large_window_) {
        const size_t win_mask = win_size_ - 1;
        while (from + 4 <= to && from + 8 <= src_loaded_) {
            // C-07: q >> 8k must expose the word at offset +k, which only
            // holds for a little-endian value.
            const core::uint64 q =
                core::load_le64(&buf_data_[static_cast<size_t>(from - pos_base_)]);
            core::uint32 h0 = ((static_cast<core::uint32>(q)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h1 =
                ((static_cast<core::uint32>(q >> 8)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h2 =
                ((static_cast<core::uint32>(q >> 16)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            core::uint32 h3 =
                ((static_cast<core::uint32>(q >> 24)) * 0x9e3779b1u) >> (32 - HASH_BITS);
            prev_[static_cast<size_t>(from & win_mask)] = head_[h0];
            head_[h0] = static_cast<core::uint32>(from);
            prev_[static_cast<size_t>((from + 1) & win_mask)] = head_[h1];
            head_[h1] = static_cast<core::uint32>(from + 1);
            prev_[static_cast<size_t>((from + 2) & win_mask)] = head_[h2];
            head_[h2] = static_cast<core::uint32>(from + 2);
            prev_[static_cast<size_t>((from + 3) & win_mask)] = head_[h3];
            head_[h3] = static_cast<core::uint32>(from + 3);
            from += 4;
        }
    }
    while (from < to && from + 4 <= src_loaded_) {
        insert_position(from);
        from++;
    }
}

size_t Compressor50::match_length(core::uint64 pos, core::uint64 cand, size_t cap) {
    const core::byte* p = &buf_data_[static_cast<size_t>(pos - pos_base_)];
    const core::byte* q = &buf_data_[static_cast<size_t>(cand - pos_base_)];
    return arch::match_length_simd(p, q, cap);
}

template <bool LargeWindow>
Compressor50::MatchInfo Compressor50::find_match_impl(core::uint64 pos, core::uint32 max_chain,
                                                      core::uint32 nice_len) {
    MatchInfo best = {0, -1};
    if (pos + 4 > src_loaded_ || pos < pos_base_) return best;

    core::uint32 h = calc_hash(pos);
    core::int64 cand = -1;
    if constexpr (LargeWindow) {
        core::uint64 hpos = head64_[h];
        // C-04: strict '>' — a candidate at pos itself is a distance-0 self-match.
        if (hpos != static_cast<core::uint64>(-1) && pos > hpos && (pos - hpos) <= max_dist_) {
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

    // RAT-F (v1.43 M3): classify WHY the walk below stopped. 0=unset, 1=(a)
    // budget exhausted while a candidate remained, 2=(d) chain ran out of
    // nodes, 3=(b) nice_len/limit break, 4=(c) candidate left the window. Only
    // (a) is the population a pre-`p` candidate cache could help: the walk ran
    // out of BUDGET, not out of candidates. A local int; never read by the
    // emission path, so it cannot alter an emitted byte.
    int h1_reason = 0;

    // Per-band longest-match slots, harvested from the SAME walk the
    // monotone-max finder already performs (v1.44 A-6: SHIPPED; previously
    // instrument-only). Scope: SIMD-measured candidates only — the cheap
    // pre-screen continue below skips without recording (exactly what the
    // probe measured), and nice_len/limit breaks terminate before a record.
    // The harvest is skipped inside the suppressed h1 counterfactual re-walk.
    // These slots feed BOTH the shipped back-shift below and the axis-6
    // probe's residual accounting.
    size_t band_len[4] = {0, 0, 0, 0};
    core::int64 band_cand[4] = {-1, -1, -1, -1};

    while (cand >= 0 && chain-- > 0) {
        if (static_cast<core::uint64>(cand) < pos_base_ ||
            (pos - static_cast<core::uint64>(cand)) > max_dist_) {
            h1_reason = 4;
            break;
        }
        const core::byte* buf_cand =
            &buf_data_[static_cast<size_t>(static_cast<core::uint64>(cand) - pos_base_)];

        core::int64 next_cand = -1;
        if constexpr (LargeWindow) {
            core::uint64 next64 = prev64_[static_cast<size_t>(cand) & win_mask];
            if (next64 != static_cast<core::uint64>(-1)) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(&prev64_[static_cast<size_t>(next64) & win_mask]);
#elif defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
                _mm_prefetch(
                    reinterpret_cast<const char*>(&prev64_[static_cast<size_t>(next64) & win_mask]),
                    _MM_HINT_T0);
#endif
                if (pos > next64 && (pos - next64) <= max_dist_) { // C-04: no self-match
                    next_cand = static_cast<core::int64>(next64);
                }
            }
        } else {
            core::uint32 next = prev_[static_cast<size_t>(cand) & win_mask];
            if (next != 0xffffffff) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(&prev_[next & win_mask]);
#elif defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
                _mm_prefetch(reinterpret_cast<const char*>(&prev_[next & win_mask]), _MM_HINT_T0);
#endif
            }
            next_cand = reconstruct_pos(pos, next);
        }
        if (next_cand >= 0 && static_cast<core::uint64>(next_cand) >= pos_base_) {
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(
                &buf_data_[static_cast<size_t>(static_cast<core::uint64>(next_cand) - pos_base_)]);
#elif defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_X64))
            _mm_prefetch(reinterpret_cast<const char*>(&buf_data_[static_cast<size_t>(
                             static_cast<core::uint64>(next_cand) - pos_base_)]),
                         _MM_HINT_T0);
#endif
        }

        if (best.length > 0 && best.length < limit &&
            buf_cand[best.length] != buf_pos[best.length]) {
            cand = next_cand;
            continue;
        }

        size_t len = 0;
        size_t cand_dist = static_cast<size_t>(pos - static_cast<core::uint64>(cand));
        if (limit >= 4) {
            core::uint32 c4;
            std::memcpy(&c4, buf_cand, 4);
            if (c4 != p4) {
                cand = next_cand;
                continue;
            }
            len = 4 + arch::match_length_simd(buf_pos + 4, buf_cand + 4, limit - 4);
        } else {
            if (buf_cand[0] != buf_pos[0]) {
                cand = next_cand;
                continue;
            }
            if (limit >= 3 && (buf_cand[1] != buf_pos[1] || buf_cand[2] != buf_pos[2])) {
                cand = next_cand;
                continue;
            }
            len = match_length(pos, static_cast<core::uint64>(cand), limit);
        }

        // ── v2.1.0 Phase 2b: Distance-Costed Hash Chain Pruning ───────────────
        // Prune candidates whose distance header cost alone exceeds best cost.
        if (best.length > 0) {
            core::uint32 inc = length_increment(cand_dist);
            // Enforce wire-legal floor: a match must have len >= inc + 2
            if (len < static_cast<size_t>(inc) + 2) {
                cand = next_cand;
                continue;
            }
            core::uint32 dist_base_cost = price_match(inc + 2, cand_dist);  // minimum legal match cost
            if (dist_base_cost < DP_INF) {
                // Estimate best cost density from current best
                size_t best_dist = static_cast<size_t>(pos - static_cast<core::uint64>(best.candidate));
                core::uint32 best_cost = price_match(best.length, best_dist);
                if (best_cost < DP_INF && dist_base_cost > best_cost) {
                    // Distance header already exceeds best — prune this candidate
                    cand = next_cand;
                    continue;
                }
            }
        }
        // ────────────────────────────────────────────────────────────────────────

        if (len > best.length) {
            best.length = len;
            best.candidate = cand;
            if (len >= nice_len || len >= limit) {
                h1_reason = 3;
                break;
            }
        }
        if (!arb_probe_suppress_) {
            const size_t band_d = static_cast<size_t>(static_cast<core::uint64>(pos) -
                                                      static_cast<core::uint64>(cand));
            const core::uint32 band = length_increment(band_d);
            if (len > band_len[band]) {
                band_len[band] = len;
                band_cand[band] = cand;
            }
        }
        cand = next_cand;
    }
    if (best.length < min_match_) best.length = 0;

    // RAT-F accounting. Normal loop exit leaves h1_reason unset: `cand >= 0`
    // means the budget bound while a candidate was still waiting (a), `cand <
    // 0` means the chain ran out of nodes first (d). The (a) case then runs a
    // counterfactual unbudgeted re-walk of the SAME chain to ask the only
    // question that matters for a cache: would truncation have hidden a
    // strictly longer match? That re-walk is a suppressed re-entry (probe off)
    // so it cannot recurse or double-count, and its result is DISCARDED — no
    // emitted byte depends on it.
    // v1.44 ordering note: this block runs BEFORE the back-shift eval below so
    // the counterfactual compares against the PRISTINE monotone-max, not a
    // post-swap candidate.
    if (h1_probe_on_) {
        if (h1_reason == 0) h1_reason = (cand >= 0) ? 1 : 2;
        ++h1_probe_.calls;
        switch (h1_reason) {
        case 1:
            ++h1_probe_.budget_exhausted;
            break;
        case 2:
            ++h1_probe_.supply_exhausted;
            break;
        case 3:
            ++h1_probe_.nice_len_break;
            break;
        default:
            ++h1_probe_.max_dist_break;
            break;
        }
        if (h1_reason == 1) {
            h1_probe_on_ = false;
            // Suppress arbiter/axis-6 accounting in the re-walk too: these
            // positions were already counted once in the outer walk, and the
            // counterfactual's own result is discarded. Scoped guard, not a
            // manual flip pair: a leaked true would silently disable the
            // shipped v1.44 back-shift for the rest of the file.
            struct SuppressArbOnce {
                bool& f_;
                explicit SuppressArbOnce(bool& f) : f_(f) { f_ = true; }
                ~SuppressArbOnce() { f_ = false; }
            };
            SuppressArbOnce arb_guard(arb_probe_suppress_);
            MatchInfo ext = find_match_impl<LargeWindow>(pos, h1_probe_extra_chain_, nice_len);
            h1_probe_on_ = true;
            if (ext.length > best.length) {
                ++h1_probe_.recoverable;
                core::uint64 delta = ext.length - best.length;
                h1_probe_.recoverable_bytes += delta;
                if (delta > h1_probe_.max_delta) h1_probe_.max_delta = delta;
            }
        }
    }

    // v1.44 A-6 back-shift — the per-band slots harvested by the walk above
    // now SUBSIDIZE the selection (previously instrument-only). Design
    // ratified by the design review (2026-10-17) against the axis-6
    // measurements; the anchored contract:
    //   1. ORDER IS LOAD-BEARING: harvest -> min_match_ floor -> h1 accounting
    //      -> THIS EVAL -> axis-6 accounting -> return. The h1 counterfactual
    //      sees the PRISTINE monotone-max; axis-6 reads the POST-SWAP token
    //      (its counts are then the residual beyond shipped behavior: delta
    //      in (0,8) plus discipline-blocked positives).
    //   2. FIELD DISCIPLINE: a swap mutates ONLY (best.length,
    //      best.candidate). Every increment/price is a FUNCTION recomputed
    //      from (pos, candidate) at its own read site — the arbiter
    //      recomputes inc_f/dist_f after this returns — never cached across
    //      the swap (a cached increment plus fresh price could push a rep
    //      through a contest over a cheaper fresh candidate its own band
    //      would never concede).
    //   3. LEGALITY: the slot loop floors each candidate by its OWN band
    //      increment (`band_len[b] < inc_s + 2` skip, plus min_match_); the
    //      coverage bound is a discipline bound, never an admissibility test;
    //      the emitter re-filter and add_match's floor filter stay in place.
    //   4. 257/REP STATE UNTOUCHED: no old_dist_ rotation, no last_length_
    //      write; the arbiter's rep gate re-verifies its own preconditions.
    //   5. THRESHOLD: realized boundary-shift cost measured at ~7 bits/byte,
    //      so admissible swaps with static delta in (0,8) are net-negative by
    //      construction — both branches require delta >= 8 static bits.
    //      The STRICT ladder-parity bound supersedes the probe's
    //      inc_b-only form: `alt_len + inc_d - alt_inc >= best.length`, i.e.
    //      a swap may concede only what the displaced token's own band
    //      increment covers beyond the incoming candidate's. The probe's
    //      110K/145K site counts measured the LOOSER form and stand only as
    //      an upper bound on the eligible population.
    //   6. The literal-fix branch (no fresh candidate survived the admission
    //      floor) is the principled completion of the same rule; its measured
    //      class was empty on every instrumented corpus, so it must remain a
    //      no-op there — the zeros/rand/farrepeat digests are the executable
    //      canaries (REFCAP).
    if (!arb_probe_suppress_) {
        constexpr core::uint32 BACKSHIFT_MIN_BITS = 8;
        // v1.44 review hardening: most branches below only ever prove the
        // floor for candidates WE placed (slot candidates carry their own
        // band floor); a pristine monotone-max can legitimately carry a
        // floor-tightening it kept (len 4 at distance > 0x40000 is legal
        // input to the walk and gets emitted-as-literals downstream), so the
        // full-floor pin is scoped to mutated placements only.
        bool mutated = false;
        // H-11: two hardware bit scans instead of two serial shift loops.
        auto price_fresh = [](size_t len, core::uint32 inc, size_t dist) -> core::uint32 {
            const core::uint32 m = floor_log2(len - inc);
            const core::uint32 d = floor_log2(dist);
            return m + 9 + (d > 3 ? d - 3 : 0);
        };
        const bool slot_valid = best.length > 0 && best.candidate >= 0;
        core::uint32 shipped_price = 0;
        if (slot_valid) {
            const size_t dist_d = static_cast<size_t>(static_cast<core::uint64>(pos) -
                                                      static_cast<core::uint64>(best.candidate));
            const core::uint32 inc_d = length_increment(dist_d);
            shipped_price = (best.length >= inc_d + 2) ? price_fresh(best.length, inc_d, dist_d)
                                                       : 7 * static_cast<core::uint32>(best.length);
        }
        // Cheapest admissible slot across bands (floor = the candidate's own).
        core::uint32 alt_price = 0;
        core::int64 alt_cand = -1;
        size_t alt_len = 0;
        core::uint32 alt_inc = 0;
        for (core::uint32 b = 0; b < 4; b++) {
            if (band_len[b] == 0 || band_cand[b] < 0) continue;
            const size_t dist_s = static_cast<size_t>(static_cast<core::uint64>(pos) -
                                                      static_cast<core::uint64>(band_cand[b]));
            const core::uint32 inc_s = length_increment(dist_s);
            if (band_len[b] < min_match_ || band_len[b] < inc_s + 2) continue;
            const core::uint32 p = price_fresh(band_len[b], inc_s, dist_s);
            if (alt_cand < 0 || p < alt_price) {
                alt_price = p;
                alt_cand = band_cand[b];
                alt_len = band_len[b];
                alt_inc = inc_s;
            }
        }
        if (alt_cand >= 0) {
            if (!slot_valid) {
                // Literal-fix: the span would pay as literals downstream; a
                // slot candidate wins when it clears the same realized-shift
                // margin. Measured class: empty on every corpus with a
                // pinned digest.
                if (back_shift_enable_ &&
                    static_cast<core::int64>(7 * alt_len) - static_cast<core::int64>(alt_price) >=
                        static_cast<core::int64>(BACKSHIFT_MIN_BITS)) {
                    best.length = alt_len;
                    best.candidate = alt_cand;
                    mutated = true;
                }
            } else {
                const size_t dist_d = static_cast<size_t>(
                    static_cast<core::uint64>(pos) - static_cast<core::uint64>(best.candidate));
                const core::uint32 inc_d = length_increment(dist_d);
                const core::int64 delta =
                    static_cast<core::int64>(shipped_price) - static_cast<core::int64>(alt_price);
                if (back_shift_enable_ && delta >= static_cast<core::int64>(BACKSHIFT_MIN_BITS) &&
                    alt_len + inc_d >= best.length + alt_inc) {
                    assert(alt_price < shipped_price);
                    best.length = alt_len;
                    best.candidate = alt_cand;
                    mutated = true;
                }
            }
        }
        // Pin A (always, pure coherence): the resolver invariant contract.
        assert(best.length == 0 || best.candidate >= 0);
        assert(best.length == 0 || best.length >= 4); // 4-byte-keyed chain walk
        // Pin B (full floor, only where the eval provably placed it): an
        // emitted/swapped candidate satisfies its own band floor. add_match's
        // filter remains in place for pristine bests that must downgrade.
        // The read is inside the `if` so the flag stays consumed in release
        // builds too (the assert itself compiles out under NDEBUG, and gcc/
        // clang would otherwise report the flag as set-but-unused).
        if (mutated) {
            assert(best.length >= length_increment(static_cast<size_t>(
                                      static_cast<core::uint64>(pos) -
                                      static_cast<core::uint64>(best.candidate))) +
                                      2);
        }
    }

    // RAT-A axis-6 accounting (log-only): compare the SHIPPED token against
    // the cheapest admissible per-band fresh candidate the walk itself saw.
    // Shipped comparison: fresh (calibrated price, msb proxy) when admissible,
    // else the literal fallback (7 bits/byte over the span the slot covers).
    // Static estimate — the arbiter work's decision-grade caveat applies: this
    // sizes RECOVERY OPPORTUNITY, not promised output bytes. `best.length`
    // below has already been zeroed when < min_match_, so a "shipped literal"
    // position is one whose only fresh option in the finder's view was
    // filtered-class. v1.44: this block runs AFTER the shipped back-shift,
    // so an eligible site self-cancels (delta 0 < threshold) and the counts
    // are the residual the shipped rule leaves on the table.
    if (arb_probe_on_ && !arb_probe_suppress_) {
        // H-11: two hardware bit scans instead of two serial shift loops.
        auto price_fresh = [](size_t len, core::uint32 inc, size_t dist) -> core::uint32 {
            const core::uint32 m = floor_log2(len - inc);
            const core::uint32 d = floor_log2(dist);
            return m + 9 + (d > 3 ? d - 3 : 0);
        };
        const bool best_valid = best.length > 0 && best.candidate >= 0;
        core::uint32 shipped_price = 0;
        if (best_valid) {
            ++arb_probe_.fresh_positions;
            const size_t dist_b = static_cast<size_t>(static_cast<core::uint64>(pos) -
                                                      static_cast<core::uint64>(best.candidate));
            const core::uint32 inc_b = length_increment(dist_b);
            if (best.length >= inc_b + 2) {
                shipped_price = price_fresh(best.length, inc_b, dist_b);
            } else {
                shipped_price = 7 * static_cast<core::uint32>(best.length);
            }
        } else {
            ++arb_probe_.lit_positions;
        }

        // Cheapest admissible band slot (band slots never influenced `best`).
        core::uint32 alt_price = 0;
        int alt_band = -1;
        size_t alt_len = 0;
        for (core::uint32 b = 0; b < 4; b++) {
            if (band_len[b] == 0 || band_cand[b] < 0) continue;
            const size_t dist_s = static_cast<size_t>(static_cast<core::uint64>(pos) -
                                                      static_cast<core::uint64>(band_cand[b]));
            const core::uint32 inc_s = length_increment(dist_s);
            if (band_len[b] < min_match_ || band_len[b] < inc_s + 2) continue;
            const core::uint32 p = price_fresh(band_len[b], inc_s, dist_s);
            if (alt_band < 0 || p < alt_price) {
                alt_price = p;
                alt_band = static_cast<int>(b);
                alt_len = band_len[b];
            }
        }
        if (alt_band >= 0) {
            if (!best_valid) {
                const core::int64 saved =
                    static_cast<core::int64>(7 * alt_len) - static_cast<core::int64>(alt_price);
                if (saved > 0) {
                    ++arb_probe_.band_fixable;
                    arb_probe_.band_fix_bits += static_cast<core::uint64>(saved);
                    ++arb_probe_.fix_band[alt_band];
                }
            } else {
                // Coverage-disciplined swap: an alternative band slot may only
                // substitute when it concedes no more than the shipped
                // candidate's own band increment (the same concession rule
                // the arbiter applies), so the tokens downstream are bounded
                // by <= inc_b extra token starts. Without this bound a
                // shorter-but-nearer match always "looks cheaper per token"
                // while extending the stream — the defect this probe must not
                // itself commit.
                const size_t dist_b = static_cast<size_t>(
                    static_cast<core::uint64>(pos) - static_cast<core::uint64>(best.candidate));
                const core::uint32 inc_b = length_increment(dist_b);
                if (alt_len + inc_b >= best.length && alt_price < shipped_price) {
                    ++arb_probe_.band_swap;
                    const core::uint64 delta = static_cast<core::uint64>(shipped_price) - alt_price;
                    arb_probe_.band_swap_bits += delta;
                    if (delta >= 8) {
                        ++arb_probe_.swap_profit;
                        arb_probe_.swap_profit_bits += delta;
                    }
                    ++arb_probe_.swap_band[alt_band];
                }
            }
        }
    }
    return best;
}

Compressor50::MatchInfo Compressor50::find_match(core::uint64 pos, core::uint32 max_chain,
                                                 core::uint32 nice_len) {
    if (is_large_window_) return find_match_impl<true>(pos, max_chain, nice_len);
    return find_match_impl<false>(pos, max_chain, nice_len);
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
} LENGTH_SLOT_TABLE;

void Compressor50::length_to_slot(core::uint32 length, core::uint32& slot, core::uint32& extra,
                                  core::uint32& bits) {
    core::uint32 v = length >= 2 ? length - 2 : 0;
    assert(v < MAX_LZ_MATCH);
    const LengthSlotEntry& e = LENGTH_SLOT_TABLE.entries[v];
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

#if defined(_MSC_VER) &&                                                                           \
    (defined(_M_AMD64) || defined(_M_X64) || defined(_M_ARM64) || defined(_M_ARM64EC))
    // 64-bit MSVC targets all have the 64-bit scan. The ARM64 macros must be
    // listed explicitly: the previous `#elif defined(_MSC_VER)` fell through to
    // the 32-bit scan on MSVC ARM64, where size_t IS 64-bit and a distance of
    // 4 GiB or more was narrowed to 32 bits — producing a wrong dbits, and so a
    // wrong distance on the wire (v1.41.1).
    unsigned long msb;
    _BitScanReverse64(&msb, distance - 1);
    core::uint32 dbits = static_cast<core::uint32>(msb - 1);
#elif defined(_MSC_VER) && defined(_M_IX86)
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

size_t Compressor50::add_match(size_t length, size_t distance) {
    core::uint32 inc = length_increment(distance);

    // Admissibility: increment(distance) + 2 <= length. Below the floor the pair
    // cannot be encoded and is FILTERED, not clamped - clamping it up would emit
    // a token covering more bytes than the caller verified.
    if (length < static_cast<size_t>(inc) + 2) return 0;

    core::uint32 base = static_cast<core::uint32>(length - inc);
    if (base > MAX_LZ_MATCH) base = MAX_LZ_MATCH; // truncate to the longest legal token

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
    // The decoder stores base + increment as last_length_. Derive the shadow from
    // the ENCODED base rather than from the caller's `length`, so the two cannot
    // diverge when the match was truncated above.
    last_length_ = base + inc;
    return last_length_;
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
    // Decoder mirror: rep matches store the length WITHOUT the distance
    // increment (decompressor50.cpp:903).
    last_length_ = length;
}

void Compressor50::add_last_len() {
    Compressor50Token tok{};
    tok.set_type(Compressor50Token::TokenType::LastLen);
    if (freq_ld_[257] < 0xfffe) freq_ld_[257]++;
    token_seq_.push_back(1);
    match_tokens_.push_back(tok);
    // The decoder's 257 path changes neither last_length_ nor OldDist.
}

bool Compressor50::need_flush() {
    return token_seq_.size() >= 32768 || input_since_block_ >= 0x80000;
}

void Compressor50::make_tables() {
    // The code_rd_/code_bd_ zeroing that used to be here was dead: build_codes
    // writes every element of its output unconditionally, including
    // zero-length symbols (v1.41.1).

    // Table flavor: always the RAR5 algorithm's 430 entries (DCB = 64). The
    // flavor is selected by the header's algorithm field, not the window:
    // our headers mark unp_ver 1 with FCI_RAR5_COMPAT, which standard
    // decoders (and ours) read as 430-entry tables at ANY window size. The
    // 446-entry RAR7 ExtraDist flavor is never emitted; begin_archive()
    // caps max_dist_ at 4 GiB, the largest distance DCB slots can encode.
    // (Choosing 446 for windows > 4 GiB produced archives that fail verification with
    // a checksum error, found by the large-dictionary interop track.)
    cur_table_size_ = TABLE_SIZE;
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
    update_price_model();
}

struct TableItem {
    core::byte sym;
    core::byte extra;
    core::byte bits;
};

static core::uint32 make_table_items(const core::byte* table_bits, core::uint32 table_size,
                                     TableItem* items) {
    core::uint32 count = 0;
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
                    items[count++] = {19, static_cast<core::byte>(chunk - 11), 7};
                } else {
                    assert(chunk - 3 <= 7 && "RLE code 18 extra field is 3 bits");
                    items[count++] = {18, static_cast<core::byte>(chunk - 3), 3};
                }
                i += chunk;
            } else {
                items[count++] = {v, 0, 0};
                i++;
            }
        } else {
            bool can_repeat = (i > 0 && table_bits[i - 1] == v);
            if (can_repeat && r >= 3) {
                core::uint32 chunk = r >= 11 ? std::min(r, 138u) : r;
                if (chunk >= 11) {
                    assert(chunk - 11 <= 127 && "RLE code 17 extra field is 7 bits");
                    items[count++] = {17, static_cast<core::byte>(chunk - 11), 7};
                } else {
                    assert(chunk - 3 <= 7 && "RLE code 16 extra field is 3 bits");
                    items[count++] = {16, static_cast<core::byte>(chunk - 3), 3};
                }
                i += chunk;
            } else {
                items[count++] = {v, 0, 0};
                i++;
            }
        }
    }
    return count;
}

void Compressor50::emit_table(BitOutput& local_out) {
    assert(cur_table_size_ <= TABLE_SIZEX);
    TableItem items[TABLE_SIZEX];
    core::uint32 item_count = make_table_items(table_bits_, cur_table_size_, items);

    std::memset(freq_bd_, 0, sizeof(freq_bd_));
    for (core::uint32 i = 0; i < item_count; ++i) freq_bd_[items[i].sym]++;

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

    for (core::uint32 i = 0; i < item_count; ++i) {
        const TableItem& item = items[i];
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
        case Compressor50Token::TokenType::LastLen: {
            // Slot 257: one LD Huffman symbol, no extra bits. Must not fall
            // into the rep default branch below (its index arithmetic would
            // map type 6 to main slot 262).
            assert(len_ld_[257] > 0 && "Encoder emitted 257 with 0-length code");
            local_out.put_bits(code_ld_[257], len_ld_[257]);
            break;
        }
        case Compressor50Token::TokenType::Match: {
            core::uint32 len_slot = tok.get_len_slot();
            core::uint32 dist_slot = tok.get_dist_slot();
            core::uint32 sym = 262 + len_slot;

            core::uint32 len_code = code_ld_[sym];
            core::uint32 len_len = len_ld_[sym];
            core::uint32 l_extra = tok.len_extra;
            core::uint32 l_bits = tok.get_len_bits();
            assert(len_len + l_bits <= 32);
            local_out.put_bits((len_code << l_bits) | l_extra, len_len + l_bits);

            core::uint32 dist_code = code_dd_[dist_slot];
            core::uint32 dist_len = len_dd_[dist_slot];
            core::uint32 d_bits = tok.get_dist_bits();
            if (d_bits >= 4) {
                if (d_bits > 4) {
                    core::uint64 extra_high = tok.dist_extra >> 4;
                    core::uint32 extra_high_len = d_bits - 4;
                    if (dist_len + extra_high_len <= 32) {
                        local_out.put_bits(
                            static_cast<core::uint32>(
                                (static_cast<core::uint64>(dist_code) << extra_high_len) |
                                extra_high),
                            dist_len + extra_high_len);
                    } else {
                        assert(dist_len + extra_high_len <= 64);
                        local_out.put_bits64(
                            (static_cast<core::uint64>(dist_code) << extra_high_len) | extra_high,
                            dist_len + extra_high_len);
                    }
                } else {
                    local_out.put_bits(dist_code, dist_len);
                }
                core::uint32 extra_low = static_cast<core::uint32>(tok.dist_extra & 0xf);
                local_out.put_bits(code_ldd_[extra_low], len_ldd_[extra_low]);
            } else {
                assert(dist_len + d_bits <= 32);
                local_out.put_bits(
                    static_cast<core::uint32>((dist_code << d_bits) | tok.dist_extra),
                    dist_len + d_bits);
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
            assert(rep_len + l_bits <= 32);
            local_out.put_bits((rep_code << l_bits) | l_extra, rep_len + l_bits);
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
    if (val > 0xFFFFFF)
        byte_cnt = 4;
    else if (val > 0xFFFF)
        byte_cnt = 3;
    else if (val > 0xFF)
        byte_cnt = 2;

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
    local_out_.reset_for_block(&block_mem_);

    make_tables();

    // Always emit the table description; never reuse (bit 7 clear). Reuse is
    // wire-legal and was enabled after a 16-block seed, tuned to our own
    // decoder's slot model, but it is not interoperable: multithreaded
    // decoders can corrupt a solid chain that contains both a
    // bit-7-clear block and a zero-size REDIR (file-copy) member (checksum
    // errors at -mt5..16, absent at -mt1 and when refs are removed). Emitting
    // a description on every block avoids this bug, at a small ratio loss on repetitive input.
    emit_table(local_out_);
    emit_tokens(local_out_);

    // NB: the shadow tables the decoder would need for reuse are no longer
    // recorded — reuse is disabled above, so that bookkeeping was dead.

    local_out_.finish_block_data();

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
    core::uint64 total_bits = local_out_.get_block_bits();

    assert(total_bits >= (data_size - 1) * 8 && "Block bit size underflow");
    core::uint32 block_bit_size = static_cast<core::uint32>(total_bits - (data_size - 1) * 8);
    if (block_bit_size < 1) block_bit_size = 1;
    if (block_bit_size > 8) block_bit_size = 8;

    core::uint32 byte_count = data_size < 0x100 ? 1 : (data_size < 0x10000 ? 2 : 3);
    // Flag bit 7 is "tables present" (spec/03 block header). Always set: the
    // table description is emitted on every block (see the note above).
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
    // m5 depth 128 — the v1.31 M2 decision (pre-analysis R4, exercised):
    // the 512-deep walk WAS the m5 cost (85.2 s canonical); depth 128 runs
    // 3.2x faster at +0.3% archive size and dominated the binary-tree
    // finder measured the same session (77.5 s for 1.8% better ratio), so
    // the tree was descoped per the pre-agreed fallback. M3 sweeps the
    // depth knee (128 vs 256) and the m4/m5 ladder.
    // M3 sweep outcome (canonical corpus 9458aa2a9f47, medians of 3, sizes
    // deterministic): m5 depth knee confirmed at 128 — d192 costs +43% time
    // for -0.18% size, d256 +81% for -0.22%. m4 dropped 128 -> 64: -38%
    // time (26.3 -> 16.2 s) at +0.89% size, restoring the m3 -> m4 -> m5
    // ladder (8.3 s/54.14 MB -> 16.2 s/53.41 MB -> 28.4 s/52.93 MB) that
    // M2's m5 change had collapsed. m1 swept per the plan grid (chains
    // 4/8/16 x nice 256/512 x lazy 0/1): best candidate bought -1.63%
    // size for +25% time; nothing met the <= current runtime constraint,
    // and the residual ~15-17% ratio gap to external m1 is structural (their
    // parse strategy, not chain depth) — recorded for a future arc. m1/m2/
    // m3 parameters are unchanged from v1.30.
    static const core::uint32 chains[6] = {0, 4, 8, 32, 64, 128};
    static const core::uint32 nices[6] = {0, 256, 512, 1024, 2048, 4096};
    static const core::uint32 lazies[6] = {0, 0, 0, 1, 1, 1};
    max_chain_ = chains[method_];
    nice_len_ = nices[method_];
    lazy_tests_ = lazies[method_];

    // Minimum emitted match length. v1.33 Design B admitted 2-byte matches at
    // m1-m3 on a claimed m1 -1.49%; the v1.33.1 re-gate could not reproduce
    // that win on any corpus available (canonical payload, two text
    // constructions, code-like text, random, zeros - all +/-0.02%), and the
    // floor measurably cost the zeros-to-random boundary case, where
    // stale-distance 2-byte reps suppressed table reuse (-8.3 KB and +167
    // reuse blocks recovered by floor 3 on a 32 MiB heterogeneous member).
    // Floor is therefore 3 everywhere again. The per-method table is kept so a
    // future per-method tuning does not need to re-plumb init_match_params().
    static const size_t min_matches[6] = {3, 3, 3, 3, 3, 3};
    min_match_ = (method_ >= 0 && method_ <= 5) ? min_matches[method_] : MIN_MATCH;
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

// Length of one filter transform/token chunk for a session window of
// `win_size`. Single source of truth for BOTH the in-loop token emission in
// process_available() and the compress_buffer() pre-transform: the decoder
// un-transforms exactly one filter-token region per token, and its scan
// skips any CALL whose operand would cross the region end. If the encoder
// transformed with wider chunk boundaries than the tokens declare, a CALL
// near a token boundary is transformed on the encode side but never
// un-transformed on the decode side (fuzz roundtrip_divergence_727: E8 at
// offset 65534, first divergence 65535).
static size_t filter_max_chunk(size_t win_size) {
    size_t max_chunk = 0x100000; // 1 MiB clamp
    if (win_size > 0x20000 && win_size / 2 < max_chunk) {
        max_chunk = win_size / 2;
    } else if (win_size <= 0x20000) {
        max_chunk = win_size > 4 ? win_size / 2 : 1;
    }
    return max_chunk;
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
            advance_crc(cur_);
            slide_window();
        }

        if (active_filter_ != FilterType::None && filter_emitted_until_ <= cur_ &&
            filter_emitted_until_ < src_size_) {
            const size_t max_chunk = filter_max_chunk(win_size_);
            const core::uint64 remaining = src_size_ - filter_emitted_until_;
            const size_t chunk_len =
                (streaming_ && !final)
                    ? max_chunk
                    : static_cast<size_t>(std::min<core::uint64>(remaining, max_chunk));
            if (chunk_len > 0) {
                if (filter_emitted_until_ > static_cast<core::uint64>(SIZE_MAX) - chunk_len) {
                    return -1;
                }
                const bool needs_transform = !external_buf_ && !filter_pretransformed_;
                if (needs_transform) {
                    if (filter_emitted_until_ < pos_base_) return -1;
                    const core::uint64 required = filter_emitted_until_ + chunk_len;
                    load_data(required);
                    if (src_loaded_ < required) {
                        if (src_eof_ || !streaming_ || final) return -1;
                        return 0;
                    }
                    const size_t buf_off = static_cast<size_t>(filter_emitted_until_ - pos_base_);
                    advance_crc(required);
                    const core::uint64 file_rel_offset = filter_emitted_until_ - file_start_pos_;
                    if (active_filter_ == FilterType::E8) {
                        Filters50::encode_e8(buf_.data() + buf_off, chunk_len, file_rel_offset,
                                             false);
                    } else if (active_filter_ == FilterType::E8E9) {
                        Filters50::encode_e8(buf_.data() + buf_off, chunk_len, file_rel_offset,
                                             true);
                    } else if (active_filter_ == FilterType::Arm) {
                        Filters50::encode_arm(buf_.data() + buf_off, chunk_len, file_rel_offset);
                    } else if (active_filter_ == FilterType::Delta) {
                        // H-08: in place; encode_delta reuses a per-thread
                        // scratch instead of a fresh 1 MiB vector per chunk.
                        Filters50::encode_delta(buf_.data() + buf_off, buf_.data() + buf_off,
                                                chunk_len, filter_channels_);
                    }
                }
                FilterToken ft;
                ft.block_start = 0;
                ft.block_length = static_cast<core::uint32>(chunk_len);
                ft.type = static_cast<core::uint8>(active_filter_);
                ft.channels = filter_channels_;
                add_filter(ft);
                filter_emitted_until_ += chunk_len;
            }
        }

        if (need_flush()) {
            advance_crc(cur_);
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

        // Normal catch-up hashing: insert all positions < cur_ into hash table
        hash_range(unhashed_pos_, cur_);
        unhashed_pos_ = cur_;

        // ── v2.1.0 Phase 2: Homogeneous Run / Zero Fast-Path Guard ──────────────
        // Disabled for now to maintain bit-identical output with reference baselines.
        // Threshold set to MAX_LZ_MATCH + 1 so it never triggers.
        // Can be re-enabled after forward-arrival parser (Phase 3) is stable.
        if (false && cur_ > pos_base_ + 3 && old_dist_[0] != 1) {  // only when no rep0 context
            const core::byte* buf_cur = &buf_data_[static_cast<size_t>(cur_ - pos_base_)];
            if (buf_cur[0] == buf_cur[-1] && buf_cur[0] == buf_cur[1] && buf_cur[0] == buf_cur[2] && buf_cur[0] == buf_cur[3]) {
                size_t max_run = std::min(static_cast<size_t>(MAX_LZ_MATCH), static_cast<size_t>(src_loaded_ - cur_));
                size_t run_len = arch::match_length_simd(buf_cur, buf_cur - 1, max_run);
                if (run_len >= static_cast<size_t>(MAX_LZ_MATCH) + 1) {  // never triggers
                    // Emit a single fresh match token (not the entire run)
                    size_t emit_len = std::min(run_len, static_cast<size_t>(MAX_LZ_MATCH));
                    add_match(emit_len, 1);
                    cur_ += emit_len;
                    input_since_block_ += emit_len;
                    // Match body hashing will be done by catch-up at next loop iteration
                    continue;  // skip match finder for this position
                }
            }
        }
        // ────────────────────────────────────────────────────────────────────────

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

        // ── v2.1.0 Phase 3: Forward-Arrival Parser (infrastructure ready, using original arbiter for compatibility) ───
        // The forward-arrival engine (evaluate_forward_arrival) is implemented and tested,
        // but uses a cost-per-byte heuristic that differs from the original greedy arbiter.
        // For bit-identical output with reference baselines, we use the original arbiter.
        // The forward-arrival engine can be enabled via flag for future optimization.
        
        // v1.43 M2+ arbiter (cost-aware concession). The rep
        // thread reuses a stored distance, so it pays no distance code — its
        // residual is one LD rep symbol + one RD slot; a fresh match pays an
        // LD slot + a DD slot + distance extras. A rep that concedes exactly
        // the FRESH candidate's own distance-band increment leaves the two
        // length ladders equal (base_f = L_f - inc_f = L_r) and wins purely
        // on the skipped distance code; conceding one byte more would put the
        // rep's length field strictly below the fresh one's, so the fresh
        // match's extra coverage starts paying for itself. The cap is what
        // keeps the concession from eating coverage:
        //   shipped rule:  rep_best_len + inc_f >= best.length  (ties -> rep)
        //   legacy rule:   rep_best_len + 1 > best.length       (flat 1-byte
        //                  concession regardless of the fresh match's band)
        // Contracts held untouched (landmine checklist): the fresh floor
        // (len >= inc+2) below, the 257 gates verbatim, the lazy pass's
        // base-field steal test, and add_match/add_rep/add_last_len as the
        // only state mutators.
        size_t dist_f = 0;
        core::uint32 inc_f = 0;
        if (best.length > 0 && best.candidate >= 0) {
            dist_f = static_cast<size_t>(cur_ - static_cast<core::uint64>(best.candidate));
            inc_f = length_increment(dist_f);
        }
        const bool rep_ok = rep_best_len >= min_match_;
        const bool rep_wins = rep_ok && (best.length == 0 || rep_best_len + inc_f >= best.length);

        // RAT-A axis-5 probe (log-only). Coherence note: `current` it compares
        // against is now the SHIPPED rule, so "differs" flags positions where
        // pure static price and the concession cap disagree (near-misses), not
        // corpus economics — with the cap in place the price comparison is
        // nearly tautological (the v1.43 review's caveat). Price model (calibrated):
        //   fresh = msb(L_f - inc_f) + 9 + distance extras;  rep = msb(L_r) + 5 + idx;
        //   a 257-eligible rep prices flat 5 (no length field on the wire).
        if (arb_probe_on_) {
            const bool fresh_ok = best.length >= min_match_ && best.length >= inc_f + 2;
            ++arb_probe_.decisions;
            if (fresh_ok && rep_ok) {
                ++arb_probe_.contested;
                auto msb = [](size_t v) -> uint32_t {
                    return floor_log2(v);
                }; // H-11
                const uint32_t fresh_cost =
                    msb(best.length - inc_f) + 9 + (msb(dist_f) > 3 ? msb(dist_f) - 3 : 0);
                const bool is_257_eligible =
                    rep_idx == 0 && last_length_ != 0 && rep_best_len == last_length_;
                const uint32_t rep_cost =
                    is_257_eligible
                        ? 5
                        : msb(rep_best_len) + 5 + static_cast<uint32_t>(rep_idx > 0 ? rep_idx : 0);
                const uint32_t cur_cost = rep_wins ? rep_cost : fresh_cost;
                const uint32_t alt_cost = rep_wins ? fresh_cost : rep_cost;
                if (alt_cost < cur_cost) {
                    ++arb_probe_.differs;
                    (rep_wins ? arb_probe_.prefer_fresh : arb_probe_.prefer_rep)++;
                    const uint32_t saved = cur_cost - alt_cost;
                    arb_probe_.bits_saved += saved;
                    if (saved > 8) ++arb_probe_.big_disagreements;
                }
            }
        }

        bool have_match = false;
        bool is_rep = false;
        bool emit_last_len = false;
        size_t len = 0;
        size_t dist = 0;

        if (rep_wins) {
            have_match = true;
            is_rep = true;
            len = rep_best_len;
            // Slot 257 (repeat last length): a cheaper encoding of a rep0
            // thread the existing rule already won — one LD Huffman symbol,
            // no extra bits, no OldDist rotation. Fires only on an EXACT
            // continuation of the (old_dist_[0], last_length_) pair:
            //   (a) last_length_ != 0 — a 257 before any match is meaningless:
            //       standard decoders skip it as a no-op,
            //       so emitting one would desync the encoder's model (M-04);
            //   (b) rep_best_len == last_length_ — the rep scan verified by
            //       byte comparison that old_dist_[0] repeats for exactly
            //       the shadow length. Strict equality, not ">=": a 257
            //       with last_length_ < rep_best_len would advance fewer
            //       bytes than the verified span and re-run the full match
            //       search at every intermediate position (measured as a
            //       +53% solid-mode regression before the equality rule).
            //       When the span is longer, rep0 wins and re-anchors a
            //       longer last_length_, so exact continuations follow.
            //   (c) rep_best_len <= cur_max_lz (the rep scan's own cap, so
            //       the copied span fits the current filter region — the
            //       copied length equals rep_best_len equals last_length_);
            //   (d) rep_idx == 0 — 257 reads old_dist_[0] only; rep1-3
            //       threads keep the rep-token path.
            // Coverage is exactly last_length_ bytes; the next position
            // re-decides.
            if (!lastlen_suppressed_ && rep_idx == 0 && last_length_ != 0 &&
                rep_best_len == last_length_) {
                emit_last_len = true;
            }
        } else if (best.length >= min_match_) {
            have_match = true;
            len = best.length;
            dist = dist_f;
            // Filter inadmissible pairs here. add_match owns the encode and
            // reports the covered span, so `len` always equals what the wire
            // carries - it may be shorter than the length the finder returned.
            core::uint32 inc = length_increment(dist);
            if (len < inc + 2) {
                have_match = false;
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
                // M-06: a match at cur_+1 has one byte less of the current
                // filter region left than a match at cur_. Clamping it to
                // cur_max_lz (the region remainder at cur_) let the lazy test
                // favour a literal for a gain that could never be emitted.
                size_t next_max_lz = cur_max_lz;
                if (active_filter_ != FilterType::None && filter_emitted_until_ > cur_ + 1) {
                    next_max_lz = std::min<size_t>(
                        next_max_lz, static_cast<size_t>(filter_emitted_until_ - (cur_ + 1)));
                }
                if (next.length > next_max_lz) next.length = next_max_lz;

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
                if (emit_last_len) {
                    add_last_len();
                    // Advance by exactly the 257 span (last_length_), not
                    // the full rep0 span: the remainder re-decides and is
                    // likely another 257.
                    len = last_length_;
                } else {
                    add_rep(static_cast<core::uint32>(rep_idx), len);
                }
            } else {
                len = add_match(len, dist);
            }

            // Catch-up already inserted every hashable p < old cur_.
            // Insert the match body [old_cur, end) exactly once via 4-wide batching,
            // then sync unhashed_pos_ so the next catch-up never re-inserts the span.
            core::uint64 end = cur_ + len;
            if (end > src_loaded_) end = src_loaded_;
            hash_range(cur_, end);
            cur_ = end;
            input_since_block_ += len;
            unhashed_pos_ = cur_;
        }
        // ────────────────────────────────────────────────────────────────────────
    }
    return 0;
}

core::int64 Compressor50::compress(bool last_block) {
    init_match_params();
    fail_count_ = 0;

    // process_available loads and consumes until cur_ == src_size_ (or fails
    // via L13). The streaming_ flag keeps its stall-point deferring off.
    if (process_available(false) < 0) return -1;

    advance_crc(cur_);
    unhashed_pos_ = cur_;
    if (!write_block(last_block)) return -1;

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

    // Filter-assisted compression. The packer is begun first so the
    // pre-transform below can chunk `filtered` exactly the way this packer
    // emits filter tokens (filter_max_chunk of the packer's ACTUAL window,
    // not the requested one): the decoder un-transforms one token region per
    // token and skips CALLs whose operand crosses the region end, so the
    // transform windows must coincide with the token regions.
    Compressor50 packer;
    if (src_size <= comp_win_size + 0x400000) {
        packer.begin_archive(nullptr, method, comp_win_size);
    } else {
        packer.init(nullptr, nullptr, method, src_size, comp_win_size);
    }
    packer.set_filter_config(filter_cfg);
    packer.set_active_filter(detected_filter, detected_channels, /*pretransformed=*/true);

    size_t max_chunk = filter_max_chunk(packer.window_size());
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
            // H-08: in place (per-thread scratch inside encode_delta).
            Filters50::encode_delta(filtered.data() + chunk_start, filtered.data() + chunk_start,
                                    chunk_len, detected_channels);
        }
        chunk_start += chunk_len;
    }

    if (src_size <= comp_win_size + 0x400000) {
        packer.set_external_buffer(filtered.data(), src_size);
        packer.set_memory_dest(&dest);
        if (packer.compress() < 0) {
            dest.clear();
            return false;
        }
        return !dest.empty();
    }

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

bool Compressor50::compress_buffer_parallel(const core::byte* src, size_t src_size,
                                            std::vector<core::byte>& dest, int method,
                                            size_t win_size, const FilterConfig& filter_cfg,
                                            unsigned threads) {
    if (src_size == 0) {
        dest.clear();
        return true;
    }
    // win_size is a public parameter with no lower bound; the per-chunk seed
    // arithmetic below subtracts one from min(win_size, off), which underflows
    // to SIZE_MAX when win_size is 0 and turns the seed pointer into a wild
    // one. Mirror the declared default so an embedder passing 0 gets the same
    // window the header advertises (v1.41.1).
    if (win_size == 0) win_size = 0x200000;

    unsigned eff_threads = threads == 0 ? core::hardware_thread_hint() : threads;
    if (eff_threads == 0) eff_threads = 1;
    if (eff_threads > 16) eff_threads = 16; // Bound worker count to cap match-finder allocations

    // Files < 1 MiB or single-threaded execution bypass chunking
    if (eff_threads <= 1 || src_size < 1024 * 1024) {
        return compress_buffer(src, src_size, dest, method, win_size, filter_cfg);
    }

    // Filter parity (v1.21.1): the chunked pipeline cannot honor pre-processing
    // transforms — chunk-relative offsets would corrupt the position-dependent
    // E8/E8E9/ARM transforms, and regions must never cross chunk boundaries.
    // Decide exactly the way the sequential path decides (same detect_filter
    // call over the same data): if that decision selects a filter, take the
    // sequential path so the request is honored; otherwise the chunks run
    // filter-free, which is what the sequential path would produce anyway.
    // The decision is content-based only, so -mt1 and -mt>1 always take the
    // same path for the same input.
    if (filter_cfg.mode != FilterMode::DisableAll) {
        core::uint8 det_channels = 1;
        if (Filters50::detect_filter(src, src_size, det_channels, filter_cfg) != FilterType::None) {
            return compress_buffer(src, src_size, dest, method, win_size, filter_cfg);
        }
    }

    size_t chunk_size = parallel_chunk_size(src_size, eff_threads);
    size_t num_chunks = (src_size + chunk_size - 1) / chunk_size;
    if (num_chunks <= 1) {
        return compress_buffer(src, src_size, dest, method, win_size, filter_cfg);
    }

    FilterConfig no_filters;
    no_filters.mode = FilterMode::DisableAll;

    std::vector<std::vector<core::byte>> chunk_outputs(num_chunks);
    std::vector<char> chunk_ok(num_chunks, 0);

    const unsigned worker_count =
        std::min<unsigned>(eff_threads, static_cast<unsigned>(num_chunks));
    std::mutex packer_mu;
    std::vector<std::unique_ptr<Compressor50>> free_packers;
    free_packers.reserve(worker_count);

    core::ThreadPool pool(worker_count);
    core::parallel_for(pool, 0, num_chunks, [&](size_t i) {
        std::unique_ptr<Compressor50> packer;
        {
            std::lock_guard<std::mutex> lk(packer_mu);
            if (!free_packers.empty()) {
                packer = std::move(free_packers.back());
                free_packers.pop_back();
            }
        }
        if (!packer) packer = std::make_unique<Compressor50>();

        size_t off = i * chunk_size;
        size_t len = std::min(chunk_size, src_size - off);
        bool is_last = (i == num_chunks - 1);
        // Seed the chunk with the member bytes that precede it, so matches
        // reach back across the chunk boundary exactly as a single-threaded
        // encoder would. The dictionary is member-scoped (see
        // set_seeded_external_buffer); the reachable span is bounded by
        // seed + chunk, which also bounds the per-worker hash allocation.
        const size_t seed_len = (off == 0) ? 0 : std::min(win_size, off) - 1;
        const size_t worker_win = std::max<size_t>(1, std::min<size_t>(win_size, seed_len + len));
        packer->begin_archive(nullptr, method, worker_win);
        packer->set_filter_config(no_filters);
        packer->set_seeded_external_buffer(src + off - seed_len, seed_len, len);
        packer->set_memory_dest(&chunk_outputs[i]);
        core::int64 r = packer->compress(is_last);
        if (r >= 0) chunk_ok[i] = 1;

        {
            std::lock_guard<std::mutex> lk(packer_mu);
            free_packers.push_back(std::move(packer));
        }
    });

    for (char ok : chunk_ok) {
        if (!ok) {
            dest.clear();
            return false;
        }
    }

    size_t total_sz = 0;
    for (const auto& co : chunk_outputs) total_sz += co.size();
    dest.clear();
    dest.reserve(total_sz);
    for (auto& co : chunk_outputs) {
        dest.insert(dest.end(), co.begin(), co.end());
        std::vector<core::byte>().swap(co);
    }
    return !dest.empty();
}

} // namespace openrar::compress
