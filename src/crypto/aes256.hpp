#ifndef OPENRAR_CRYPTO_AES256_HPP
#define OPENRAR_CRYPTO_AES256_HPP

// C4324: the alignas(16) key schedules below are padded by design (AES-NI
// loads 16-byte-aligned pointers); the padding warning is expected noise.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

#include "../core/types.hpp"
#include <cstddef>

namespace openrar::crypto {

class Aes256 {
public:
    static constexpr size_t KEY_SIZE = 32;
    static constexpr size_t BLOCK_SIZE = 16;
    static constexpr size_t ROUNDS = 14;

    Aes256();
    explicit Aes256(const core::byte* key);
    // Wipe round keys: they are password-derived material and must not
    // survive in freed heap memory (spec 08-encryption).
    ~Aes256() noexcept;
    Aes256(const Aes256&) = delete;
    Aes256& operator=(const Aes256&) = delete;

    void set_key(const core::byte* key);

    // Single-block raw primitives
    void encrypt_block(const core::byte* in, core::byte* out) const;
    void decrypt_block(const core::byte* in, core::byte* out) const;

    // CBC mode in-place. Returns false when size is not a multiple of the
    // 16-byte block size (no bytes are then touched): AES-CBC has no stream
    // mode, and silently flooring a trailing partial block would drop
    // ciphertext without a diagnostic (Q5). On success, `iv` holds the LAST
    // CIPHERTEXT block of the call (chaining value for the next buffer).
    bool encrypt_cbc(core::byte* data, size_t size, core::byte* iv) const;
    bool decrypt_cbc(core::byte* data, size_t size, core::byte* iv) const;

private:
    void expand_key(const core::byte* key);

    core::uint32 enc_round_keys_[60]; // 15 round keys of 4 uint32 words
    core::uint32 dec_round_keys_[60];

    // Hardware acceleration state (AES-NI / NEON)
    bool has_ni_;
    alignas(16) core::byte ni_enc_keys_[15 * 16];
    alignas(16) core::byte ni_dec_keys_[15 * 16];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_AES256_HPP
