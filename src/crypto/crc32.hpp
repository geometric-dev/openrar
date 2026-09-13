#ifndef OPENRAR_CRYPTO_CRC32_HPP
#define OPENRAR_CRYPTO_CRC32_HPP

#include "../core/types.hpp"
#include <cstddef>

namespace openrar::crypto {

// Computes the standard IEEE 802.3 CRC32 checksum of data[0..size-1].
// init_crc is the initial accumulator (defaults to 0 for a fresh checksum).
core::uint32 crc32(const void* data, size_t size, core::uint32 init_crc = 0);

// Raw step accumulator without pre-inversion or post-inversion
core::uint32 crc32_step(core::uint32 crc, const void* data, size_t size);

// Streaming CRC32 calculator class
class Crc32 {
public:
    Crc32() : value_(0) {}
    explicit Crc32(core::uint32 init) : value_(init) {}

    void reset(core::uint32 init = 0) { value_ = init; }
    void update(const void* data, size_t size) { value_ = crc32(data, size, value_); }
    core::uint32 get() const { return value_; }

private:
    core::uint32 value_;
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_CRC32_HPP
