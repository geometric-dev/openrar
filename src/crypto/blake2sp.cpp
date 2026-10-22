#include "blake2sp.hpp"
#include <algorithm>
#include <cstring>

// H-14: 8-way AVX2 leaf kernel, x86-64 only, selected at runtime.
#if (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))) ||                               \
    ((defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__))
#include "../core/cpu.hpp"
#include <immintrin.h>
#define OPENRAR_BLAKE2SP_AVX2 1
#if defined(_MSC_VER) && !defined(__clang__)
#define OPENRAR_AVX2_TARGET
#else
#define OPENRAR_AVX2_TARGET __attribute__((target("avx2")))
#endif
#endif

namespace openrar::crypto {

namespace {

inline core::uint32 rotr32(core::uint32 w, unsigned int c) {
    // L-03: UB-free for every c (incl. 0); compiles to a single ror.
    return (w >> (c & 31u)) | (w << ((0u - c) & 31u));
}

const core::uint32 BLAKE2S_IV[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

const core::byte SIGMA[10][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                  {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                                  {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
                                  {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                                  {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
                                  {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                                  {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
                                  {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                                  {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
                                  {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

inline void mix_g(core::uint32& a, core::uint32& b, core::uint32& c, core::uint32& d,
                  core::uint32 x, core::uint32 y) {
    a = a + b + x;
    d = rotr32(d ^ a, 16);
    c = c + d;
    b = rotr32(b ^ c, 12);
    a = a + b + y;
    d = rotr32(d ^ a, 8);
    c = c + d;
    b = rotr32(b ^ c, 7);
}

} // namespace

void Blake2sp::Blake2sState::init(core::uint32 node_offset, core::uint32 node_depth) {
    std::memset(buf_, 0, sizeof(buf_));
    buflen_ = 0;
    t_[0] = 0;
    t_[1] = 0;
    f_[0] = 0;
    f_[1] = 0;
    last_node_ = false;

    for (int i = 0; i < 8; ++i) {
        h_[i] = BLAKE2S_IV[i];
    }

    // RFC 7693 tree parameters: digest_len=32, fanout=8, depth=2, leaf_length=0
    h_[0] ^= 0x02080020u;
    h_[2] ^= node_offset;
    h_[3] ^= (node_depth << 16) | 0x20000000u; // inner hash length = 32
}

void Blake2sp::Blake2sState::compress(const core::byte* block) {
    core::uint32 m[16];
    core::uint32 v[16];

    for (size_t i = 0; i < 16; ++i) {
        m[i] = core::read_le32(block + i * 4);
    }

    for (size_t i = 0; i < 8; ++i) {
        v[i] = h_[i];
    }
    v[8] = BLAKE2S_IV[0];
    v[9] = BLAKE2S_IV[1];
    v[10] = BLAKE2S_IV[2];
    v[11] = BLAKE2S_IV[3];
    v[12] = t_[0] ^ BLAKE2S_IV[4];
    v[13] = t_[1] ^ BLAKE2S_IV[5];
    v[14] = f_[0] ^ BLAKE2S_IV[6];
    v[15] = f_[1] ^ BLAKE2S_IV[7];

    for (int r = 0; r < 10; ++r) {
        mix_g(v[0], v[4], v[8], v[12], m[SIGMA[r][0]], m[SIGMA[r][1]]);
        mix_g(v[1], v[5], v[9], v[13], m[SIGMA[r][2]], m[SIGMA[r][3]]);
        mix_g(v[2], v[6], v[10], v[14], m[SIGMA[r][4]], m[SIGMA[r][5]]);
        mix_g(v[3], v[7], v[11], v[15], m[SIGMA[r][6]], m[SIGMA[r][7]]);
        mix_g(v[0], v[5], v[10], v[15], m[SIGMA[r][8]], m[SIGMA[r][9]]);
        mix_g(v[1], v[6], v[11], v[12], m[SIGMA[r][10]], m[SIGMA[r][11]]);
        mix_g(v[2], v[7], v[8], v[13], m[SIGMA[r][12]], m[SIGMA[r][13]]);
        mix_g(v[3], v[4], v[9], v[14], m[SIGMA[r][14]], m[SIGMA[r][15]]);
    }

    for (size_t i = 0; i < 8; ++i) {
        h_[i] ^= v[i] ^ v[i + 8];
    }
}

void Blake2sp::Blake2sState::update(const core::byte* in, size_t inlen) {
    while (inlen > 0) {
        size_t left = buflen_;
        size_t fill = 2 * BLOCK_SIZE - left;

        if (inlen > fill) {
            std::memcpy(buf_ + left, in, fill);
            buflen_ += fill;
            t_[0] += BLOCK_SIZE;
            if (t_[0] < BLOCK_SIZE) t_[1]++;

            compress(buf_);

            std::memcpy(buf_, buf_ + BLOCK_SIZE, BLOCK_SIZE);
            buflen_ -= BLOCK_SIZE;
            in += fill;
            inlen -= fill;
        } else {
            std::memcpy(buf_ + left, in, inlen);
            buflen_ += inlen;
            in += inlen;
            inlen = 0;
        }
    }
}

void Blake2sp::Blake2sState::finish(core::byte* out) {
    if (buflen_ > BLOCK_SIZE) {
        t_[0] += BLOCK_SIZE;
        if (t_[0] < BLOCK_SIZE) t_[1]++;
        compress(buf_);
        buflen_ -= BLOCK_SIZE;
        std::memcpy(buf_, buf_ + BLOCK_SIZE, buflen_);
    }

    t_[0] += static_cast<core::uint32>(buflen_);
    if (t_[0] < buflen_) t_[1]++;

    f_[0] = 0xFFFFFFFFu; // Last block flag
    if (last_node_) {
        f_[1] = 0xFFFFFFFFu; // Last node flag
    }
    std::memset(buf_ + buflen_, 0, 2 * BLOCK_SIZE - buflen_);
    compress(buf_);

    for (size_t i = 0; i < 8; ++i) {
        core::write_le32(out + i * 4, h_[i]);
    }
}

Blake2sp::Blake2sp() {
    reset();
}

void Blake2sp::reset() {
    std::memset(buf_, 0, sizeof(buf_));
    buflen_ = 0;

    root_state_.init(0, 1); // Root instance at depth 1
    root_state_.last_node_ = true;

    for (core::uint32 i = 0; i < PARALLELISM; ++i) {
        states_[i].init(i, 0); // Leaf instances at depth 0
    }
    states_[PARALLELISM - 1].last_node_ = true;
}

#if defined(OPENRAR_BLAKE2SP_AVX2)
namespace {

// H-14: 8-way vertical BLAKE2s. Lane l of every __m256i is leaf l's word, so
// one pass of G over 16 vectors advances all eight leaves at once. Inputs are
// eight 64-byte blocks given by row pointers; the message is transposed into
// word-major form with two 8x8 32-bit transposes.
OPENRAR_AVX2_TARGET inline __m256i rotr16(__m256i x) {
    const __m256i k = _mm256_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13, 2, 3,
                                       0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
    return _mm256_shuffle_epi8(x, k);
}
OPENRAR_AVX2_TARGET inline __m256i rotr8(__m256i x) {
    const __m256i k = _mm256_setr_epi8(1, 2, 3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12, 1, 2,
                                       3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12);
    return _mm256_shuffle_epi8(x, k);
}
OPENRAR_AVX2_TARGET inline __m256i rotr12(__m256i x) {
    return _mm256_or_si256(_mm256_srli_epi32(x, 12), _mm256_slli_epi32(x, 20));
}
OPENRAR_AVX2_TARGET inline __m256i rotr7(__m256i x) {
    return _mm256_or_si256(_mm256_srli_epi32(x, 7), _mm256_slli_epi32(x, 25));
}

OPENRAR_AVX2_TARGET inline void g8(__m256i& a, __m256i& b, __m256i& c, __m256i& d, __m256i x,
                                   __m256i y) {
    a = _mm256_add_epi32(_mm256_add_epi32(a, b), x);
    d = rotr16(_mm256_xor_si256(d, a));
    c = _mm256_add_epi32(c, d);
    b = rotr12(_mm256_xor_si256(b, c));
    a = _mm256_add_epi32(_mm256_add_epi32(a, b), y);
    d = rotr8(_mm256_xor_si256(d, a));
    c = _mm256_add_epi32(c, d);
    b = rotr7(_mm256_xor_si256(b, c));
}

// out[i] lane l = 32-bit word i of rows[l] (8 rows x 8 words, 32 bytes each).
OPENRAR_AVX2_TARGET inline void transpose8x8(const core::byte* const rows[8], size_t off,
                                             __m256i out[8]) {
    __m256i r[8];
    for (int l = 0; l < 8; ++l) {
        r[l] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[l] + off));
    }
    const __m256i t0 = _mm256_unpacklo_epi32(r[0], r[1]), t1 = _mm256_unpackhi_epi32(r[0], r[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(r[2], r[3]), t3 = _mm256_unpackhi_epi32(r[2], r[3]);
    const __m256i t4 = _mm256_unpacklo_epi32(r[4], r[5]), t5 = _mm256_unpackhi_epi32(r[4], r[5]);
    const __m256i t6 = _mm256_unpacklo_epi32(r[6], r[7]), t7 = _mm256_unpackhi_epi32(r[6], r[7]);
    const __m256i u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);
    const __m256i u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);
    const __m256i u4 = _mm256_unpacklo_epi64(t4, t6), u5 = _mm256_unpackhi_epi64(t4, t6);
    const __m256i u6 = _mm256_unpacklo_epi64(t5, t7), u7 = _mm256_unpackhi_epi64(t5, t7);
    out[0] = _mm256_permute2x128_si256(u0, u4, 0x20);
    out[1] = _mm256_permute2x128_si256(u1, u5, 0x20);
    out[2] = _mm256_permute2x128_si256(u2, u6, 0x20);
    out[3] = _mm256_permute2x128_si256(u3, u7, 0x20);
    out[4] = _mm256_permute2x128_si256(u0, u4, 0x31);
    out[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
    out[6] = _mm256_permute2x128_si256(u2, u6, 0x31);
    out[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}

// One compression of all eight leaves. The byte counter t and the (zero)
// finalization flags are identical across leaves inside update(), so they
// are broadcast.
OPENRAR_AVX2_TARGET void compress8(__m256i h[8], const core::byte* const rows[8], core::uint32 t0,
                                   core::uint32 t1) {
    __m256i m[16];
    transpose8x8(rows, 0, m);
    transpose8x8(rows, 32, m + 8);

    __m256i v[16];
    for (int i = 0; i < 8; ++i) v[i] = h[i];
    for (int i = 0; i < 4; ++i) v[8 + i] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[i]));
    v[12] = _mm256_set1_epi32(static_cast<int>(t0 ^ BLAKE2S_IV[4]));
    v[13] = _mm256_set1_epi32(static_cast<int>(t1 ^ BLAKE2S_IV[5]));
    v[14] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[6])); // f0 == 0 in update()
    v[15] = _mm256_set1_epi32(static_cast<int>(BLAKE2S_IV[7])); // f1 == 0 in update()

    for (int r = 0; r < 10; ++r) {
        const core::byte* s = SIGMA[r];
        g8(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        g8(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        g8(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        g8(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        g8(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        g8(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        g8(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        g8(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h[i] = _mm256_xor_si256(h[i], _mm256_xor_si256(v[i], v[i + 8]));
}

// Advance the shared 64-bit byte counter by one block, then compress (the
// scalar leaf update() increments t_ before each compress the same way).
OPENRAR_AVX2_TARGET void step8(__m256i h[8], const core::byte* const rows[8], core::uint32& t0,
                               core::uint32& t1) {
    t0 += static_cast<core::uint32>(Blake2sp::BLOCK_SIZE);
    if (t0 < Blake2sp::BLOCK_SIZE) ++t1;
    compress8(h, rows, t0, t1);
}

} // namespace

// Bulk path for `chunks` whole 512-byte chunks at `in`. Precondition: every
// leaf holds two pending blocks (buflen_ == 128), the steady state the
// scalar leaf update() reaches after two blocks, and all leaves share t_.
// Each new block makes a leaf compress its OLDEST pending block, so across
// the batch the leaves compress: pending[0], pending[1], then input chunks
// 0 .. chunks-3 straight from `in`; the last two chunks become the new
// pending pair. State and counters end exactly as the scalar path leaves
// them, so finish() is unchanged.
OPENRAR_AVX2_TARGET void Blake2sp::update_bulk_avx2(const core::byte* in, size_t chunks) {
    alignas(32) core::uint32 lanes[8][8];
    for (int i = 0; i < 8; ++i) {
        for (int l = 0; l < 8; ++l) lanes[i][l] = states_[l].h_[i];
    }
    __m256i h[8];
    for (int i = 0; i < 8; ++i)
        h[i] = _mm256_load_si256(reinterpret_cast<const __m256i*>(lanes[i]));

    core::uint32 t0 = states_[0].t_[0], t1 = states_[0].t_[1];
    const core::byte* rows[8];
    for (int l = 0; l < 8; ++l) rows[l] = states_[l].buf_;
    step8(h, rows, t0, t1); // pending[0]
    for (int l = 0; l < 8; ++l) rows[l] = states_[l].buf_ + BLOCK_SIZE;
    step8(h, rows, t0, t1); // pending[1]
    for (size_t c = 0; c + 2 < chunks; ++c) {
        const core::byte* base = in + c * PARALLELISM * BLOCK_SIZE;
        for (int l = 0; l < 8; ++l) rows[l] = base + static_cast<size_t>(l) * BLOCK_SIZE;
        step8(h, rows, t0, t1);
    }

    for (int i = 0; i < 8; ++i) _mm256_store_si256(reinterpret_cast<__m256i*>(lanes[i]), h[i]);
    const core::byte* last2 = in + (chunks - 2) * PARALLELISM * BLOCK_SIZE;
    const core::byte* last1 = in + (chunks - 1) * PARALLELISM * BLOCK_SIZE;
    for (int l = 0; l < 8; ++l) {
        Blake2sState& s = states_[l];
        for (int i = 0; i < 8; ++i) s.h_[i] = lanes[i][l];
        s.t_[0] = t0;
        s.t_[1] = t1;
        std::memcpy(s.buf_, last2 + static_cast<size_t>(l) * BLOCK_SIZE, BLOCK_SIZE);
        std::memcpy(s.buf_ + BLOCK_SIZE, last1 + static_cast<size_t>(l) * BLOCK_SIZE, BLOCK_SIZE);
        s.buflen_ = 2 * BLOCK_SIZE;
    }
}
#endif // OPENRAR_BLAKE2SP_AVX2

void Blake2sp::update(const void* data, size_t size) {
    const auto* in = static_cast<const core::byte*>(data);
    size_t left = buflen_;
    size_t fill = sizeof(buf_) - left;

    if (left > 0 && size >= fill) {
        std::memcpy(buf_ + left, in, fill);
        for (size_t i = 0; i < PARALLELISM; ++i) {
            states_[i].update(buf_ + i * BLOCK_SIZE, BLOCK_SIZE);
        }
        in += fill;
        size -= fill;
        left = 0;
    }

#if defined(OPENRAR_BLAKE2SP_AVX2)
    // H-14: once the leaves reach their steady state (two pending blocks),
    // batches of >= 2 chunks run 8-way. Smaller inputs and the warm-up use
    // the scalar per-leaf path below, which yields identical state.
    if (size >= 2 * sizeof(buf_) && states_[0].buflen_ == 2 * BLOCK_SIZE &&
        core::get_cpu_features().avx2) {
        const size_t chunks = size / sizeof(buf_);
        update_bulk_avx2(in, chunks);
        in += chunks * sizeof(buf_);
        size -= chunks * sizeof(buf_);
    }
#endif

    while (size >= sizeof(buf_)) {
        for (size_t i = 0; i < PARALLELISM; ++i) {
            states_[i].update(in + i * BLOCK_SIZE, BLOCK_SIZE);
        }
        in += sizeof(buf_);
        size -= sizeof(buf_);
    }

    if (size > 0) {
        std::memcpy(buf_ + left, in, size);
    }
    buflen_ = left + size;
}

void Blake2sp::finish(void* out_digest) {
    core::byte hash[PARALLELISM][DIGEST_SIZE];

    for (size_t i = 0; i < PARALLELISM; ++i) {
        // INTENDED: the `buflen_ - i * BLOCK_SIZE` subtraction below looks
        // like it can underflow, but the guard makes it safe; the shape
        // (including the BLOCK_SIZE clamp) mirrors the RFC 7693 blake2sp
        // reference implementation. The guard IS the fix — keep both.
        if (buflen_ > i * BLOCK_SIZE) {
            size_t left = buflen_ - i * BLOCK_SIZE;
            if (left > BLOCK_SIZE) left = BLOCK_SIZE;
            states_[i].update(buf_ + i * BLOCK_SIZE, left);
        }
        states_[i].finish(hash[i]);
    }

    for (size_t i = 0; i < PARALLELISM; ++i) {
        root_state_.update(hash[i], DIGEST_SIZE);
    }
    root_state_.finish(static_cast<core::byte*>(out_digest));
}

void Blake2sp::compute(const void* data, size_t size, void* out_digest) {
    Blake2sp hasher;
    hasher.update(data, size);
    hasher.finish(out_digest);
}

} // namespace openrar::crypto
