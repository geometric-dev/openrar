#ifndef OPENRAR_CRYPTO_RNG_HPP
#define OPENRAR_CRYPTO_RNG_HPP

#include "../core/types.hpp"
#include <cstddef>

namespace openrar::crypto {

// Fill buf with cryptographically strong random bytes from the OS.
// Returns true on success. On failure the buffer contents are unspecified;
// callers must abort rather than proceed with weak entropy.
//
// Backend:
//   Windows: BCryptGenRandom (bcrypt.dll)
//   POSIX (recent glibc / macOS / *BSD): getentropy()
//   POSIX fallback: /dev/urandom
bool secure_random_bytes(core::byte* buf, size_t len);

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_RNG_HPP
