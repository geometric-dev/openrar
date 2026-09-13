#include "vint.hpp"

namespace openrar::core {

void push_vint(std::vector<byte>& vect, uint64 value) {
    do {
        byte b = static_cast<byte>(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            b |= 0x80;
        }
        vect.push_back(b);
    } while (value != 0);
}

void push_vint_fixed(std::vector<byte>& vect, uint64 value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        byte b = static_cast<byte>(value & 0x7F);
        value >>= 7;
        if (i < width - 1) {
            b |= 0x80;
        }
        vect.push_back(b);
    }
}

bool read_vint(const byte* data, size_t size, uint64& value, size_t& bytes_read) {
    // Failure contract: on `false`, `bytes_read` holds the number of bytes
    // CONSUMED before the failure (truncated run or MAX_VINT_SIZE overrun),
    // not 0 — and it never exceeds `size`. Callers that advance by
    // bytes_read unconditionally are therefore safe, but the contract is
    // fragile: a caller assuming "0 on failure" would stall on the same
    // byte forever. Documented rather than reset to 0 (report INFO 4).
    value = 0;
    bytes_read = 0;
    uint32 shift = 0;

    // INTENTIONALLY LENIENT (interop): do NOT reject non-minimal/padded
    // encodings. Padded vints occur in real archives — locator
    // quick-open offsets (e.g. `D1 80 00` = 81) and file data_size/unp_size
    // fields — and the format specification accepts any
    // encoding up to 10 bytes (shift < 64) with no canonicality requirement.
    // A stricter check here silently zero-scans valid archives.
    for (size_t i = 0; i < size && i < MAX_VINT_SIZE; ++i) {
        byte b = data[i];
        bytes_read++;

        if (shift == 63) {
            // At shift 63, only bit 0 contributes without exceeding 64 bits.
            if ((b & 0x7E) != 0) {
                return false; // overflow
            }
            value |= static_cast<uint64>(b & 0x01) << 63;
        } else {
            value |= static_cast<uint64>(b & 0x7F) << shift;
        }

        shift += 7;

        if ((b & 0x80) == 0) {
            return true;
        }
    }

    return false; // Truncated or exceeded MAX_VINT_SIZE without terminating
}

} // namespace openrar::core
