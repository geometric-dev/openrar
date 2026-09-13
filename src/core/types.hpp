#ifndef OPENRAR_CORE_TYPES_HPP
#define OPENRAR_CORE_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace openrar::core {

using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

using byte = std::uint8_t;

// Endian conversion utilities
inline uint16 read_le16(const void* ptr) {
    const auto* p = static_cast<const uint8*>(ptr);
    return static_cast<uint16>(p[0]) | (static_cast<uint16>(p[1]) << 8);
}

inline uint32 read_le32(const void* ptr) {
    const auto* p = static_cast<const uint8*>(ptr);
    return static_cast<uint32>(p[0]) | (static_cast<uint32>(p[1]) << 8) |
           (static_cast<uint32>(p[2]) << 16) | (static_cast<uint32>(p[3]) << 24);
}

inline uint64 read_le64(const void* ptr) {
    const auto* p = static_cast<const uint8*>(ptr);
    return static_cast<uint64>(read_le32(p)) | (static_cast<uint64>(read_le32(p + 4)) << 32);
}

inline void write_le16(void* ptr, uint16 val) {
    auto* p = static_cast<uint8*>(ptr);
    p[0] = static_cast<uint8>(val & 0xFF);
    p[1] = static_cast<uint8>((val >> 8) & 0xFF);
}

inline void write_le32(void* ptr, uint32 val) {
    auto* p = static_cast<uint8*>(ptr);
    p[0] = static_cast<uint8>(val & 0xFF);
    p[1] = static_cast<uint8>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8>((val >> 24) & 0xFF);
}

inline void write_le64(void* ptr, uint64 val) {
    auto* p = static_cast<uint8*>(ptr);
    write_le32(p, static_cast<uint32>(val & 0xFFFFFFFF));
    write_le32(p + 4, static_cast<uint32>((val >> 32) & 0xFFFFFFFF));
}

inline uint32 read_be32(const void* ptr) {
    const auto* p = static_cast<const uint8*>(ptr);
    return (static_cast<uint32>(p[0]) << 24) | (static_cast<uint32>(p[1]) << 16) |
           (static_cast<uint32>(p[2]) << 8) | static_cast<uint32>(p[3]);
}

inline void write_be32(void* ptr, uint32 val) {
    auto* p = static_cast<uint8*>(ptr);
    p[0] = static_cast<uint8>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8>(val & 0xFF);
}

inline bool is_power_of_two(uint64 n) {
    return n != 0 && (n & (n - 1)) == 0;
}

} // namespace openrar::core

#endif // OPENRAR_CORE_TYPES_HPP
