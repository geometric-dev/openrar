#include "blake2sp.hpp"
#include <algorithm>
#include <cstring>

namespace openrar::crypto {

namespace {

inline core::uint32 rotr32(core::uint32 w, unsigned int c) {
    return (w >> c) | (w << (32 - c));
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

inline void G(core::uint32& a, core::uint32& b, core::uint32& c, core::uint32& d, core::uint32 x,
              core::uint32 y) {
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
        G(v[0], v[4], v[8], v[12], m[SIGMA[r][0]], m[SIGMA[r][1]]);
        G(v[1], v[5], v[9], v[13], m[SIGMA[r][2]], m[SIGMA[r][3]]);
        G(v[2], v[6], v[10], v[14], m[SIGMA[r][4]], m[SIGMA[r][5]]);
        G(v[3], v[7], v[11], v[15], m[SIGMA[r][6]], m[SIGMA[r][7]]);
        G(v[0], v[5], v[10], v[15], m[SIGMA[r][8]], m[SIGMA[r][9]]);
        G(v[1], v[6], v[11], v[12], m[SIGMA[r][10]], m[SIGMA[r][11]]);
        G(v[2], v[7], v[8], v[13], m[SIGMA[r][12]], m[SIGMA[r][13]]);
        G(v[3], v[4], v[9], v[14], m[SIGMA[r][14]], m[SIGMA[r][15]]);
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
