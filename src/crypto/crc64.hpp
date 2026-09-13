#ifndef OPENRAR_CRYPTO_CRC64_HPP
#define OPENRAR_CRYPTO_CRC64_HPP

#include "../core/types.hpp"
#include <cstddef>

namespace openrar::crypto {

// CRC-64/XZ (aka CRC-64/GO-ECMA-182, reflected form).
//
// polynomial (reflected): 0xC96C5795D7870F42
// init:                   0xFFFFFFFFFFFFFFFF
// final XOR:              0xFFFFFFFFFFFFFFFF
// input bit order:        least-significant first
//
// This is the CRC64 used by every RAR 5.0 inline recovery-record parity
// shard to protect its structured header plus payload. See
// docs/spec/05-recovery.md §4.6.1.
class Crc64Xz {
public:
    Crc64Xz();
    void reset();
    void update(const void* data, size_t n);
    core::uint64 get() const;

    static core::uint64 compute(const void* data, size_t n);

private:
    core::uint64 crc_;
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_CRC64_HPP
