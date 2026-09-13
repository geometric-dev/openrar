#ifndef OPENRAR_CRYPTO_SHA256_HPP
#define OPENRAR_CRYPTO_SHA256_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <cstring>

namespace openrar::crypto {

class Sha256 {
public:
    static constexpr size_t DIGEST_SIZE = 32;
    static constexpr size_t BLOCK_SIZE = 64;

    Sha256();
    void reset();
    void update(const void* data, size_t size);
    void finish(void* out_digest);

    static void compute(const void* data, size_t size, void* out_digest);

    // Midstate access for HMAC-style fast restart: reading/writing the
    // compression state after a whole number of 64-byte blocks. The
    // caller guarantees bytes_processed is 64-aligned when restoring.
    void get_state(core::uint32 out[8], core::uint64& bytes_processed) const {
        std::memcpy(out, state_, sizeof(state_));
        bytes_processed = count_;
    }
    void set_state(const core::uint32 st[8], core::uint64 bytes_processed) {
        std::memcpy(state_, st, sizeof(state_));
        count_ = bytes_processed;
    }

private:
    void transform(const core::byte* block);

    core::uint32 state_[8];
    core::uint64 count_;
    core::byte buffer_[BLOCK_SIZE];
};

// HMAC-SHA256
class HmacSha256 {
public:
    static constexpr size_t DIGEST_SIZE = 32;

    HmacSha256(const void* key, size_t key_len);
    ~HmacSha256() noexcept;
    void reset();
    void update(const void* data, size_t size);
    void finish(void* out_digest);

    static void compute(const void* key, size_t key_len, const void* data, size_t size,
                        void* out_digest);

    // (Re)build the cached pad midstates from the current pads.
    void init_midstate();

private:
    Sha256 inner_;
    Sha256 outer_;
    core::byte k_opad_[Sha256::BLOCK_SIZE];
    core::byte k_ipad_[Sha256::BLOCK_SIZE];
    core::uint32 ipad_state_[8];
    core::uint32 opad_state_[8];
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_SHA256_HPP
