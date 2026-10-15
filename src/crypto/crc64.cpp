#include "crc64.hpp"
#include <cstring>

namespace openrar::crypto {

namespace {

// Slicing-by-8 tables for the reflected CRC-64/XZ polynomial
// 0xC96C5795D7870F42. tK[i] advances the CRC state over K bytes that
// have been pre-XORed into the low K bytes of the state.
struct Crc64Tables {
    core::uint64 t[8][256];
    Crc64Tables() {
        constexpr core::uint64 POLY = 0xC96C5795D7870F42ULL;
        for (int i = 0; i < 256; ++i) {
            core::uint64 c = static_cast<core::uint64>(i);
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (POLY ^ (c >> 1)) : (c >> 1);
            }
            t[0][i] = c;
        }
        for (int k = 1; k < 8; ++k) {
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

inline core::uint64 crc64_update_slice8(core::uint64 c, const void* data, size_t n) {
    const core::byte* p = static_cast<const core::byte*>(data);
    const auto& tab = tables();

    while (n >= 8) {
        core::uint64 chunk = core::read_le64(p);
        c ^= chunk;
        c = tab.t[7][c & 0xFF] ^ tab.t[6][(c >> 8) & 0xFF] ^ tab.t[5][(c >> 16) & 0xFF] ^
            tab.t[4][(c >> 24) & 0xFF] ^ tab.t[3][(c >> 32) & 0xFF] ^ tab.t[2][(c >> 40) & 0xFF] ^
            tab.t[1][(c >> 48) & 0xFF] ^ tab.t[0][(c >> 56) & 0xFF];
        p += 8;
        n -= 8;
    }
    while (n--) {
        c = tab.t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

} // namespace

Crc64Xz::Crc64Xz() : crc_(~0ULL) {}

void Crc64Xz::reset() {
    crc_ = ~0ULL;
}

void Crc64Xz::update(const void* data, size_t n) {
    crc_ = crc64_update_slice8(crc_, data, n);
}

core::uint64 Crc64Xz::get() const {
    return crc_ ^ ~0ULL;
}

core::uint64 Crc64Xz::compute(const void* data, size_t n) {
    Crc64Xz c;
    c.update(data, n);
    return c.get();
}

void RawCrc64::update(const void* data, size_t n) {
    crc_ = crc64_update_slice8(crc_, data, n);
}

core::uint64 RawCrc64::compute(const void* data, size_t n) {
    RawCrc64 c;
    c.update(data, n);
    return c.get();
}

} // namespace openrar::crypto
