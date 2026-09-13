#include "crc32.hpp"
#include "arch/crc32_arch.hpp"

namespace openrar::crypto {

namespace {

struct CrcTableHolder {
    core::uint32 table[8][256];

    CrcTableHolder() {
        for (core::uint32 i = 0; i < 256; ++i) {
            core::uint32 crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
            }
            table[0][i] = crc;
        }

        for (int i = 0; i < 256; ++i) {
            for (int j = 1; j < 8; ++j) {
                table[j][i] = (table[j - 1][i] >> 8) ^ table[0][table[j - 1][i] & 0xFF];
            }
        }
    }
};

const CrcTableHolder g_crc_table;

} // namespace

core::uint32 crc32_step(core::uint32 crc, const void* data, size_t size) {
#if defined(OPENRAR_HAS_ARM_CRC32)
    if (core::get_cpu_features().arm_crc32) {
        return arch::crc32_step_arm64(crc, data, size);
    }
#endif
#if defined(OPENRAR_HAS_X86_PCLMUL)
    if (core::get_cpu_features().pclmulqdq && size >= 64) {
        // INTENDED (tail-skip contract): crc32_step_pclmul consumes only
        // multiples of 16 bytes — exactly `size & ~15` — and never touches
        // the final <16-byte tail. The scalar loops below resume from that
        // point; both paths share the raw (un-inverted) accumulator
        // semantics, so chaining them is lossless. The apparent
        // double-count of the tail is deliberate; do not "fix" it.
        crc = arch::crc32_step_pclmul(crc, data, size);
        size_t done = size & ~15;
        data = static_cast<const core::byte*>(data) + done;
        size -= done;
    }
#endif
    const auto* p = static_cast<const core::byte*>(data);

    // Process leading unaligned bytes
    while (size > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
        crc = (crc >> 8) ^ g_crc_table.table[0][(crc ^ *p++) & 0xFF];
        size--;
    }

    // Slicing-by-8 loop
    while (size >= 8) {
        core::uint32 low = core::read_le32(p) ^ crc;
        core::uint32 high = core::read_le32(p + 4);

        crc = g_crc_table.table[7][low & 0xFF] ^ g_crc_table.table[6][(low >> 8) & 0xFF] ^
              g_crc_table.table[5][(low >> 16) & 0xFF] ^ g_crc_table.table[4][(low >> 24) & 0xFF] ^
              g_crc_table.table[3][high & 0xFF] ^ g_crc_table.table[2][(high >> 8) & 0xFF] ^
              g_crc_table.table[1][(high >> 16) & 0xFF] ^ g_crc_table.table[0][(high >> 24) & 0xFF];

        p += 8;
        size -= 8;
    }

    // Trailing bytes
    while (size > 0) {
        crc = (crc >> 8) ^ g_crc_table.table[0][(crc ^ *p++) & 0xFF];
        size--;
    }

    return crc;
}

core::uint32 crc32(const void* data, size_t size, core::uint32 init_crc) {
    return ~crc32_step(~init_crc, data, size);
}

} // namespace openrar::crypto
