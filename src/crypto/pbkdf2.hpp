#ifndef OPENRAR_CRYPTO_PBKDF2_HPP
#define OPENRAR_CRYPTO_PBKDF2_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <string>

namespace openrar::crypto {

struct Rar5Keys {
    core::byte aes_key[32];
    core::byte hash_key[32];
    core::byte psw_check[8];
    core::byte psw_check_csum[4];
};

// Clean-room implementation of RAR 5.0 PBKDF2 key derivation
// Based on RAR 5.0 Encryption Specification (AES-256-CBC + PBKDF2-HMAC-SHA256)
class Pbkdf2Rar5 {
public:
    // Derives AES key, HMAC hash key, and password check fields
    // count is the iteration count (typically 1 << lg2_count, e.g. 1 << 15 = 32768)
    // Returns false and leaves out_keys zeroed when count == 0: the RAR5
    // iteration split subtracts one from count, so a zero count would wrap
    // to ~4.3 billion HMAC iterations.
    static bool derive_keys(const void* password, size_t password_len, const core::byte* salt,
                            size_t salt_len, core::uint32 count, Rar5Keys& out_keys);

    // Constant-time equality for fixed-length byte strings: accumulates the
    // XOR of every byte pair instead of exiting at the first mismatch, so
    // the comparison time does not leak the offset of the first differing
    // byte. Used for the PswCheck password-verification compare, where an
    // early-exit memcmp would make header-decode timing a password oracle.
    static inline bool constant_time_equal(const core::byte* a, const core::byte* b, size_t n) {
        core::byte diff = 0;
        for (size_t i = 0; i < n; ++i) {
            diff |= static_cast<core::byte>(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    // Convenience method accepting std::string (UTF-8)
    static bool derive_keys(const std::string& password, const core::byte* salt, size_t salt_len,
                            core::uint32 count, Rar5Keys& out_keys) {
        return derive_keys(password.data(), password.size(), salt, salt_len, count, out_keys);
    }
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_PBKDF2_HPP
