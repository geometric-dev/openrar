#include "pbkdf2.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace openrar::crypto {

bool Pbkdf2Rar5::derive_keys(const void* password, size_t password_len, const core::byte* salt,
                             size_t salt_len, core::uint32 count, Rar5Keys& out_keys) {
    // Defensive guard (report L2): the iteration split below computes
    // count - 1, so count == 0 wraps to 0xFFFFFFFF (~4.3 billion HMAC
    // iterations). All current callers validate lg2_count, but this is
    // public API: fail cleanly instead, leaving the keys zeroed.
    if (count == 0) {
        std::memset(&out_keys, 0, sizeof(out_keys));
        return false;
    }
    // Passwords longer than 127 bytes are truncated before key derivation.
    if (password_len > 127) password_len = 127;
    // SaltData = Salt || 0x00 0x00 0x00 0x01
    std::vector<core::byte> salt_data(salt, salt + salt_len);
    salt_data.push_back(0x00);
    salt_data.push_back(0x00);
    salt_data.push_back(0x00);
    salt_data.push_back(0x01);

    // Initial HMAC computation: U1 = HMAC(pwd, SaltData)
    core::byte u1[Sha256::DIGEST_SIZE];
    HmacSha256 hmac(password, password_len);
    hmac.update(salt_data.data(), salt_data.size());
    hmac.finish(u1);

    core::byte fn[Sha256::DIGEST_SIZE];
    std::memcpy(fn, u1, sizeof(fn));

    core::byte psw_check_value[Sha256::DIGEST_SIZE];

    // INTENDED (RAR5 KDF spec): the {count-1, 16, 16} split is not an off-by-N.
    // The RAR5 KDF spends count-1 iterations on the AES-256 key and 16 each
    // on the hash key and PswCheck per specification. Do not "fix" the loop bounds.
    const core::uint32 cur_count[3] = {count - 1, 16, 16};
    core::byte* cur_value[3] = {out_keys.aes_key, out_keys.hash_key, psw_check_value};

    core::byte u2[Sha256::DIGEST_SIZE];
    for (int step = 0; step < 3; ++step) {
        for (core::uint32 iter = 0; iter < cur_count[step]; ++iter) {
            hmac.reset();
            hmac.update(u1, sizeof(u1));
            hmac.finish(u2);

            std::memcpy(u1, u2, sizeof(u1));
            for (size_t k = 0; k < sizeof(fn); ++k) {
                fn[k] ^= u1[k];
            }
        }
        std::memcpy(cur_value[step], fn, Sha256::DIGEST_SIZE);
    }

    // Fold PswCheck: 32 bytes into 8 bytes (PswCheck[i % 8] ^= PswCheckValue[i])
    std::memset(out_keys.psw_check, 0, sizeof(out_keys.psw_check));
    for (size_t i = 0; i < 32; ++i) {
        out_keys.psw_check[i % 8] ^= psw_check_value[i];
    }

    // PswCheckCsum: first 4 bytes of Sha256(PswCheck)
    core::byte csum_digest[Sha256::DIGEST_SIZE];
    Sha256::compute(out_keys.psw_check, sizeof(out_keys.psw_check), csum_digest);
    std::memcpy(out_keys.psw_check_csum, csum_digest, sizeof(out_keys.psw_check_csum));

    // Zero out sensitive temporaries
    std::memset(u1, 0, sizeof(u1));
    std::memset(u2, 0, sizeof(u2));
    std::memset(fn, 0, sizeof(fn));
    std::memset(psw_check_value, 0, sizeof(psw_check_value));
    return true;
}

} // namespace openrar::crypto
