#include "filters50.hpp"
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace openrar::compress {

#if defined(__aarch64__) || defined(_M_ARM64)
// NEON movemask equivalent: after vshrn(…, 4) each compared byte's 0xFF lands
// in its own 4-bit nibble (byte k -> bits 4k..4k+3), so the index of the
// first set nibble's low bit divided by 4 is the first matching byte index.
inline unsigned first_match_byte(core::uint64 nibble_mask) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(nibble_mask)) >> 2;
#else
    unsigned idx = 0;
    while (((nibble_mask >> (4 * idx)) & 0xF) == 0) ++idx;
    return idx;
#endif
}
#endif

void Filters50::apply_e8_scalar(core::byte* data, size_t size, core::uint64 file_offset,
                                bool include_e9) {
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    core::byte cmp_byte2 = include_e9 ? 0xE9 : 0xE8;
    size_t cur_pos = 0;

    while (cur_pos + 11 < size) {
        core::uint64 q = core::read_le64(data + cur_pos);
        core::uint64 v_e8 = q ^ 0xE8E8E8E8E8E8E8E8ULL;
        core::uint64 v_e9 = q ^ (include_e9 ? 0xE9E9E9E9E9E9E9E9ULL : 0xE8E8E8E8E8E8E8E8ULL);
        core::uint64 has_e8 = (v_e8 - 0x0101010101010101ULL) & ~v_e8 & 0x8080808080808080ULL;
        core::uint64 has_e9 = (v_e9 - 0x0101010101010101ULL) & ~v_e9 & 0x8080808080808080ULL;

        if ((has_e8 | has_e9) == 0) {
            cur_pos += 8;
            continue;
        }

        size_t chunk_end = cur_pos + 8;
        while (cur_pos < chunk_end) {
            core::byte cur_byte = data[cur_pos++];
            if (cur_byte == 0xE8 || cur_byte == cmp_byte2) {
                core::uint32 offset =
                    (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
                core::uint32 addr = core::read_le32(data + cur_pos);
                if ((addr & 0x80000000u) != 0) {
                    if (((addr + offset) & 0x80000000u) == 0) {
                        core::write_le32(data + cur_pos, addr + file_size_mask);
                    }
                } else {
                    if (((addr - file_size_mask) & 0x80000000u) != 0) {
                        core::write_le32(data + cur_pos, addr - offset);
                    }
                }
                cur_pos += 4;
            }
        }
    }

    // cur_pos + 4 <= size ensures operand (4 bytes) is strictly inside data buffer
    for (; cur_pos + 4 < size;) {
        core::byte cur_byte = data[cur_pos++];
        if (cur_byte == 0xE8 || cur_byte == cmp_byte2) {
            core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
            core::uint32 addr = core::read_le32(data + cur_pos);

            // Match legacy 0x80000000 bit checks to avoid signed integer overflow UB
            if ((addr & 0x80000000u) != 0) {
                if (((addr + offset) & 0x80000000u) == 0) {
                    core::write_le32(data + cur_pos, addr + file_size_mask);
                }
            } else {
                if (((addr - file_size_mask) & 0x80000000u) != 0) {
                    core::write_le32(data + cur_pos, addr - offset);
                }
            }
            cur_pos += 4;
        }
    }
}

void Filters50::apply_e8(core::byte* data, size_t size, core::uint64 file_offset, bool include_e9) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    core::byte cmp_byte2 = include_e9 ? 0xE9 : 0xE8;
    size_t cur_pos = 0;

    __m128i ve8 = _mm_set1_epi8(static_cast<char>(0xE8));
    __m128i ve9 = _mm_set1_epi8(static_cast<char>(cmp_byte2));
    while (cur_pos + 19 < size) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + cur_pos));
        __m128i cmp1 = _mm_cmpeq_epi8(v, ve8);
        __m128i cmp2 = _mm_cmpeq_epi8(v, ve9);
        int mask = _mm_movemask_epi8(_mm_or_si128(cmp1, cmp2));
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        int tz;
#if defined(_MSC_VER)
        unsigned long bsf;
        _BitScanForward(&bsf, mask);
        tz = bsf;
#else
        tz = __builtin_ctz(mask);
#endif
        // SIMD already confirmed cur_pos + tz is an E8/E9 opcode byte; just
        // step past it — no need to re-read via cur_byte, which was
        // triggering an unused-variable warning on gcc/clang -Wunused.
        cur_pos += tz + 1;
        core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
        core::uint32 addr = core::read_le32(data + cur_pos);
        if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) {
                core::write_le32(data + cur_pos, addr + file_size_mask);
            }
        } else {
            if (((addr - file_size_mask) & 0x80000000u) != 0) {
                core::write_le32(data + cur_pos, addr - offset);
            }
        }
        cur_pos += 4;
    }
    apply_e8_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos, include_e9);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // NEON mirror of the SSE2 path above: identical scan windows, resume
    // points and per-site transform, so output is bit-identical to the
    // scalar version. The 16-byte window is reloaded after every processed
    // match, so a wider skip never hides a call-site the scalar scan would
    // have seen.
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;

    const uint8x16_t ve8 = vdupq_n_u8(0xE8);
    const uint8x16_t ve9 = vdupq_n_u8(include_e9 ? 0xE9 : 0xE8);
    while (cur_pos + 19 < size) {
        uint8x16_t v = vld1q_u8(data + cur_pos);
        uint8x16_t any = vorrq_u8(vceqq_u8(v, ve8), vceqq_u8(v, ve9));
        uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(any), 4);
        uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        // SIMD already confirmed cur_pos + tz is an E8/E9 opcode byte.
        cur_pos += first_match_byte(mask) + 1;
        core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
        core::uint32 addr = core::read_le32(data + cur_pos);
        if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) {
                core::write_le32(data + cur_pos, addr + file_size_mask);
            }
        } else {
            if (((addr - file_size_mask) & 0x80000000u) != 0) {
                core::write_le32(data + cur_pos, addr - offset);
            }
        }
        cur_pos += 4;
    }
    apply_e8_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos, include_e9);
#else
    apply_e8_scalar(data, size, file_offset, include_e9);
#endif
}

void Filters50::apply_arm_scalar(core::byte* data, size_t size, core::uint64 file_offset) {
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;

    while (cur_pos + 7 < size) {
        core::uint64 q = core::read_le64(data + cur_pos);
        if (static_cast<core::byte>(q >> 24) == 0xEB || static_cast<core::byte>(q >> 56) == 0xEB) {
            if (static_cast<core::byte>(q >> 24) == 0xEB) {
                core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                                      (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                                      (static_cast<core::uint32>(data[cur_pos + 2]) << 16);
                offset -= (file_off + static_cast<core::uint32>(cur_pos)) / 4;
                data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
                data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
                data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
            }
            if (static_cast<core::byte>(q >> 56) == 0xEB) {
                core::uint32 offset = static_cast<core::uint32>(data[cur_pos + 4]) |
                                      (static_cast<core::uint32>(data[cur_pos + 5]) << 8) |
                                      (static_cast<core::uint32>(data[cur_pos + 6]) << 16);
                offset -= (file_off + static_cast<core::uint32>(cur_pos + 4)) / 4;
                data[cur_pos + 4] = static_cast<core::byte>(offset & 0xFF);
                data[cur_pos + 5] = static_cast<core::byte>((offset >> 8) & 0xFF);
                data[cur_pos + 6] = static_cast<core::byte>((offset >> 16) & 0xFF);
            }
        }
        cur_pos += 8;
    }

    for (; cur_pos + 3 < size; cur_pos += 4) {
        if (data[cur_pos + 3] == 0xEB) { // BL instruction (0xEB prefix)
            core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                                  (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                                  (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

            offset -= (file_off + static_cast<core::uint32>(cur_pos)) / 4;

            data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
            data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
            data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        }
    }
}

void Filters50::apply_arm(core::byte* data, size_t size, core::uint64 file_offset) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;
    __m128i veb = _mm_set1_epi8(static_cast<char>(0xEB));
    while (cur_pos + 18 < size) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + cur_pos + 3));
        __m128i cmp = _mm_cmpeq_epi8(v, veb);
        int mask = _mm_movemask_epi8(cmp);
        mask &= 0x1111;
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        int tz;
#if defined(_MSC_VER)
        unsigned long bsf;
        _BitScanForward(&bsf, mask);
        tz = bsf;
#else
        tz = __builtin_ctz(mask);
#endif
        cur_pos += tz;

        core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                              (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                              (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

        offset -= (file_off + static_cast<core::uint32>(cur_pos)) / 4;

        data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
        data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
        data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        cur_pos += 4;
    }
    apply_arm_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // NEON mirror of the SSE2 path above: window starts at cur_pos+3 and only
    // every 4th window byte can be the EB opcode, so keep nibbles 0,4,8,12
    // (the SSE2 mask's 0x1111 equivalent).
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;
    const uint8x16_t veb = vdupq_n_u8(0xEB);
    while (cur_pos + 18 < size) {
        uint8x16_t v = vld1q_u8(data + cur_pos + 3);
        uint8x16_t cmp = vceqq_u8(v, veb);
        uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
        uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
        mask &= 0x000F000F000F000FULL;
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        cur_pos += first_match_byte(mask);

        core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                              (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                              (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

        offset -= (file_off + static_cast<core::uint32>(cur_pos)) / 4;

        data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
        data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
        data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        cur_pos += 4;
    }
    apply_arm_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos);
#else
    apply_arm_scalar(data, size, file_offset);
#endif
}

bool Filters50::apply_delta(const core::byte* src, core::byte* dest, size_t size,
                            core::uint8 channels) {
    if (channels == 0 || channels > 32 || size == 0) {
        return false;
    }

    // Guard against in-place aliasing (src == dest)
    std::vector<core::byte> tmp;
    core::byte* out = dest;
    if (src == dest) {
        tmp.resize(size);
        out = tmp.data();
    }

    size_t src_pos = 0;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (channels == 1) {
        core::byte prev = 0;
        __m128i vprev = _mm_setzero_si128();
        while (src_pos + 16 <= size) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + src_pos));
            // Delta decode is Prev - Src (spec 04-filters): negate source then prefix-sum (addition)
            __m128i vneg = _mm_sub_epi8(_mm_setzero_si128(), v);
            vneg = _mm_add_epi8(vneg, _mm_slli_si128(vneg, 1));
            vneg = _mm_add_epi8(vneg, _mm_slli_si128(vneg, 2));
            vneg = _mm_add_epi8(vneg, _mm_slli_si128(vneg, 4));
            vneg = _mm_add_epi8(vneg, _mm_slli_si128(vneg, 8));
            vneg = _mm_add_epi8(vneg, vprev);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + src_pos), vneg);
            int top = _mm_extract_epi16(vneg, 7) >> 8;
            vprev = _mm_set1_epi8(static_cast<char>(top));
            src_pos += 16;
        }
        prev = static_cast<core::byte>(_mm_extract_epi16(vprev, 0));
        while (src_pos < size) {
            prev = static_cast<core::byte>(prev - src[src_pos]);
            out[src_pos++] = prev;
        }
        if (src == dest) {
            std::memcpy(dest, out, size);
        }
        return true;
    }
#endif

    for (core::uint8 ch = 0; ch < channels; ++ch) {
        core::byte prev = 0;
        for (size_t dest_pos = ch; dest_pos < size; dest_pos += channels) {
            prev = static_cast<core::byte>(prev - src[src_pos++]);
            out[dest_pos] = prev;
        }
    }

    if (src == dest) {
        std::memcpy(dest, out, size);
    }
    return true;
}

void Filters50::encode_e8_scalar(core::byte* data, size_t size, core::uint64 file_offset,
                                 bool include_e9) {
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    core::byte cmp_byte2 = include_e9 ? 0xE9 : 0xE8;
    size_t cur_pos = 0;

    while (cur_pos + 11 < size) {
        core::uint64 q = core::read_le64(data + cur_pos);
        core::uint64 v_e8 = q ^ 0xE8E8E8E8E8E8E8E8ULL;
        core::uint64 v_e9 = q ^ (include_e9 ? 0xE9E9E9E9E9E9E9E9ULL : 0xE8E8E8E8E8E8E8E8ULL);
        core::uint64 has_e8 = (v_e8 - 0x0101010101010101ULL) & ~v_e8 & 0x8080808080808080ULL;
        core::uint64 has_e9 = (v_e9 - 0x0101010101010101ULL) & ~v_e9 & 0x8080808080808080ULL;

        if ((has_e8 | has_e9) == 0) {
            cur_pos += 8;
            continue;
        }

        size_t chunk_end = cur_pos + 8;
        while (cur_pos < chunk_end) {
            core::byte cur_byte = data[cur_pos++];
            if (cur_byte == 0xE8 || cur_byte == cmp_byte2) {
                core::uint32 offset =
                    (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
                core::uint32 addr = core::read_le32(data + cur_pos);
                if ((addr & 0x80000000u) != 0) {
                    if (((addr + offset) & 0x80000000u) == 0) {
                        core::write_le32(data + cur_pos, addr + offset);
                    }
                } else {
                    if (addr < file_size_mask) {
                        if (addr + offset >= file_size_mask) {
                            core::write_le32(data + cur_pos, addr - file_size_mask);
                        } else {
                            core::write_le32(data + cur_pos, addr + offset);
                        }
                    }
                }
                cur_pos += 4;
            }
        }
    }

    for (; cur_pos + 4 < size;) {
        core::byte cur_byte = data[cur_pos++];
        if (cur_byte == 0xE8 || cur_byte == cmp_byte2) {
            core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
            core::uint32 addr = core::read_le32(data + cur_pos);
            if ((addr & 0x80000000u) != 0) {
                if (((addr + offset) & 0x80000000u) == 0) {
                    core::write_le32(data + cur_pos, addr + offset);
                }
            } else {
                if (addr < file_size_mask) {
                    if (addr + offset >= file_size_mask) {
                        core::write_le32(data + cur_pos, addr - file_size_mask);
                    } else {
                        core::write_le32(data + cur_pos, addr + offset);
                    }
                }
            }
            cur_pos += 4;
        }
    }
}

void Filters50::encode_e8(core::byte* data, size_t size, core::uint64 file_offset, bool include_e9) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    core::byte cmp_byte2 = include_e9 ? 0xE9 : 0xE8;
    size_t cur_pos = 0;

    __m128i ve8 = _mm_set1_epi8(static_cast<char>(0xE8));
    __m128i ve9 = _mm_set1_epi8(static_cast<char>(cmp_byte2));
    while (cur_pos + 19 < size) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + cur_pos));
        __m128i cmp1 = _mm_cmpeq_epi8(v, ve8);
        __m128i cmp2 = _mm_cmpeq_epi8(v, ve9);
        int mask = _mm_movemask_epi8(_mm_or_si128(cmp1, cmp2));
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        int tz;
#if defined(_MSC_VER)
        unsigned long bsf;
        _BitScanForward(&bsf, mask);
        tz = bsf;
#else
        tz = __builtin_ctz(mask);
#endif
        cur_pos += tz + 1;
        core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
        core::uint32 addr = core::read_le32(data + cur_pos);
        if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) {
                core::write_le32(data + cur_pos, addr + offset);
            }
        } else {
            if (addr < file_size_mask) {
                if (addr + offset >= file_size_mask) {
                    core::write_le32(data + cur_pos, addr - file_size_mask);
                } else {
                    core::write_le32(data + cur_pos, addr + offset);
                }
            }
        }
        cur_pos += 4;
    }
    encode_e8_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos, include_e9);
#elif defined(__aarch64__) || defined(_M_ARM64)
    const core::uint32 file_size_mask = 0x01000000u; // 16MB modular cycle
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;

    const uint8x16_t ve8 = vdupq_n_u8(0xE8);
    const uint8x16_t ve9 = vdupq_n_u8(include_e9 ? 0xE9 : 0xE8);
    while (cur_pos + 19 < size) {
        uint8x16_t v = vld1q_u8(data + cur_pos);
        uint8x16_t any = vorrq_u8(vceqq_u8(v, ve8), vceqq_u8(v, ve9));
        uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(any), 4);
        uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        cur_pos += first_match_byte(mask) + 1;
        core::uint32 offset = (static_cast<core::uint32>(cur_pos) + file_off) % file_size_mask;
        core::uint32 addr = core::read_le32(data + cur_pos);
        if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) {
                core::write_le32(data + cur_pos, addr + offset);
            }
        } else {
            if (addr < file_size_mask) {
                if (addr + offset >= file_size_mask) {
                    core::write_le32(data + cur_pos, addr - file_size_mask);
                } else {
                    core::write_le32(data + cur_pos, addr + offset);
                }
            }
        }
        cur_pos += 4;
    }
    encode_e8_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos, include_e9);
#else
    encode_e8_scalar(data, size, file_offset, include_e9);
#endif
}

void Filters50::encode_arm_scalar(core::byte* data, size_t size, core::uint64 file_offset) {
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;

    while (cur_pos + 7 < size) {
        core::uint64 q = core::read_le64(data + cur_pos);
        if (static_cast<core::byte>(q >> 24) == 0xEB || static_cast<core::byte>(q >> 56) == 0xEB) {
            if (static_cast<core::byte>(q >> 24) == 0xEB) {
                core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                                      (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                                      (static_cast<core::uint32>(data[cur_pos + 2]) << 16);
                offset += (file_off + static_cast<core::uint32>(cur_pos)) / 4;
                data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
                data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
                data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
            }
            if (static_cast<core::byte>(q >> 56) == 0xEB) {
                core::uint32 offset = static_cast<core::uint32>(data[cur_pos + 4]) |
                                      (static_cast<core::uint32>(data[cur_pos + 5]) << 8) |
                                      (static_cast<core::uint32>(data[cur_pos + 6]) << 16);
                offset += (file_off + static_cast<core::uint32>(cur_pos + 4)) / 4;
                data[cur_pos + 4] = static_cast<core::byte>(offset & 0xFF);
                data[cur_pos + 5] = static_cast<core::byte>((offset >> 8) & 0xFF);
                data[cur_pos + 6] = static_cast<core::byte>((offset >> 16) & 0xFF);
            }
        }
        cur_pos += 8;
    }

    for (; cur_pos + 3 < size; cur_pos += 4) {
        if (data[cur_pos + 3] == 0xEB) {
            core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                                  (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                                  (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

            offset += (file_off + static_cast<core::uint32>(cur_pos)) / 4;

            data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
            data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
            data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        }
    }
}

void Filters50::encode_arm(core::byte* data, size_t size, core::uint64 file_offset) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;
    __m128i veb = _mm_set1_epi8(static_cast<char>(0xEB));
    while (cur_pos + 18 < size) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + cur_pos + 3));
        __m128i cmp = _mm_cmpeq_epi8(v, veb);
        int mask = _mm_movemask_epi8(cmp);
        mask &= 0x1111;
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        int tz;
#if defined(_MSC_VER)
        unsigned long bsf;
        _BitScanForward(&bsf, mask);
        tz = bsf;
#else
        tz = __builtin_ctz(mask);
#endif
        cur_pos += tz;

        core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                              (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                              (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

        offset += (file_off + static_cast<core::uint32>(cur_pos)) / 4;

        data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
        data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
        data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        cur_pos += 4;
    }
    encode_arm_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos);
#elif defined(__aarch64__) || defined(_M_ARM64)
    core::uint32 file_off = static_cast<core::uint32>(file_offset);
    size_t cur_pos = 0;
    const uint8x16_t veb = vdupq_n_u8(0xEB);
    while (cur_pos + 18 < size) {
        uint8x16_t v = vld1q_u8(data + cur_pos + 3);
        uint8x16_t cmp = vceqq_u8(v, veb);
        uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
        uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
        mask &= 0x000F000F000F000FULL;
        if (mask == 0) {
            cur_pos += 16;
            continue;
        }
        cur_pos += first_match_byte(mask);

        core::uint32 offset = static_cast<core::uint32>(data[cur_pos]) |
                              (static_cast<core::uint32>(data[cur_pos + 1]) << 8) |
                              (static_cast<core::uint32>(data[cur_pos + 2]) << 16);

        offset += (file_off + static_cast<core::uint32>(cur_pos)) / 4;

        data[cur_pos] = static_cast<core::byte>(offset & 0xFF);
        data[cur_pos + 1] = static_cast<core::byte>((offset >> 8) & 0xFF);
        data[cur_pos + 2] = static_cast<core::byte>((offset >> 16) & 0xFF);
        cur_pos += 4;
    }
    encode_arm_scalar(data + cur_pos, size - cur_pos, file_offset + cur_pos);
#else
    encode_arm_scalar(data, size, file_offset);
#endif
}

bool Filters50::encode_delta(const core::byte* src, core::byte* dest, size_t size,
                             core::uint8 channels) {
    if (channels == 0 || channels > 32 || size == 0) {
        return false;
    }

    // Guard against in-place aliasing (src == dest)
    std::vector<core::byte> tmp;
    core::byte* out = dest;
    if (src == dest) {
        tmp.resize(size);
        out = tmp.data();
    }

    size_t dest_pos = 0;
    for (core::uint8 ch = 0; ch < channels; ++ch) {
        core::byte prev = 0;
        for (size_t src_pos = ch; src_pos < size; src_pos += channels) {
            core::byte val = src[src_pos];
            out[dest_pos++] = static_cast<core::byte>(prev - val);
            prev = val;
        }
    }

    if (src == dest) {
        std::memcpy(dest, out, size);
    }
    return true;
}

} // namespace openrar::compress
