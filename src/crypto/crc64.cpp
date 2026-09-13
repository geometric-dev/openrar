#include "crc64.hpp"
#include <cstring>

namespace openrar::crypto {

namespace {

// Slicing-by-4 tables for the reflected CRC-64/XZ polynomial
// 0xC96C5795D7870F42. tK[i] advances the CRC state over K bytes that
// have been pre-XORed into the low K bytes of the state.
struct Crc64Tables {
    core::uint64 t[4][256];
    Crc64Tables() {
        constexpr core::uint64 POLY = 0xC96C5795D7870F42ULL;
        for (int i = 0; i < 256; ++i) {
            core::uint64 c = static_cast<core::uint64>(i);
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (POLY ^ (c >> 1)) : (c >> 1);
            }
            t[0][i] = c;
        }
        for (int k = 1; k < 4; ++k) {
            for (int i = 0; i < 256; ++i) {
                core::uint64 c = t[k - 1][i];
                t[k][i] = (c >> 8) ^ t[0][c & 0xFF];
            }
        }
    }
};

const Crc64Tables& tables() {
    static const Crc64Tables k;
    return k;
}

} // namespace

Crc64Xz::Crc64Xz() : crc_(~0ULL) {}

void Crc64Xz::reset() {
    crc_ = ~0ULL;
}

void Crc64Xz::update(const void* data, size_t n) {
    const core::byte* p = static_cast<const core::byte*>(data);
    core::uint64 c = crc_;
    const auto& tab = tables();

    while (n >= 4) {
        core::uint64 chunk = 0;
        std::memcpy(&chunk, p, 4); // little-endian load via memcpy (portable)
        c ^= chunk;
        c = (c >> 32) ^ tab.t[3][c & 0xFF] ^ tab.t[2][(c >> 8) & 0xFF] ^
            tab.t[1][(c >> 16) & 0xFF] ^ tab.t[0][(c >> 24) & 0xFF];
        p += 4;
        n -= 4;
    }
    while (n--) {
        c = tab.t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    }
    crc_ = c;
}

core::uint64 Crc64Xz::get() const {
    return crc_ ^ ~0ULL;
}

core::uint64 Crc64Xz::compute(const void* data, size_t n) {
    Crc64Xz c;
    c.update(data, n);
    return c.get();
}

} // namespace openrar::crypto
