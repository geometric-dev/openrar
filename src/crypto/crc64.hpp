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

// Raw CRC-64: the same reflected polynomial and slicing tables as Crc64Xz,
// but init 0 and no final XOR. This is the per-data-chunk checksum carried
// in the inline recovery-record shard-header state region (v1.35.0; probe
// record docs/v1.35.0-rr-probe.md §5). Init 0 makes the checksum GF(2)-
// linear (all-zero input folds to 0), which the reference repairer relies
// on; the unpadded chunk length is the canonical input.
class RawCrc64 {
public:
    RawCrc64() : crc_(0) {}
    void reset() { crc_ = 0; }
    void update(const void* data, size_t n);
    core::uint64 get() const { return crc_; }

    static core::uint64 compute(const void* data, size_t n);

private:
    core::uint64 crc_;
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_CRC64_HPP
