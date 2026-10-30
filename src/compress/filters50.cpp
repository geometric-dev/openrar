#include "filters50.hpp"
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#ifdef _MSC_VER
#include <intrin.h>
#endif
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
#elif defined(_MSC_VER)
    unsigned long bsf;
    _BitScanForward64(&bsf, nibble_mask);
    return static_cast<unsigned>(bsf) >> 2;
#else
    unsigned idx = 0;
    while (((nibble_mask >> (4 * idx)) & 0xF) == 0) ++idx;
    return idx;
#endif
}
#endif

FilterType Filters50::detect_filter(const core::byte* data, size_t size, core::uint8& out_channels,
                                    const FilterConfig& config) {
    out_channels = 1;
    if (config.mode == FilterMode::DisableAll) return FilterType::None;

    // 1. Check explicit overrides
    if (config.e8_override > 0) return FilterType::E8;
    if (config.arm_override > 0) return FilterType::Arm;
    if (config.delta_override > 0) {
        out_channels = config.delta_channels > 0 ? config.delta_channels : 2;
        return FilterType::Delta;
    }

    if (!data || size < 64) return FilterType::None;

    // 2. PE executable detection
    if (config.e8_override >= 0) {
        if (data[0] == 'M' && data[1] == 'Z' && size >= 128) {
            core::uint32 pe_off = core::read_le32(data + 0x3C);
            // 64-bit arithmetic: pe_off is untrusted and near-UINT32_MAX values
            // would wrap below in 32-bit and pass the guard (OOB read).
            if (static_cast<core::uint64>(pe_off) + 6 < size && data[pe_off] == 'P' &&
                data[pe_off + 1] == 'E' && data[pe_off + 2] == 0 && data[pe_off + 3] == 0) {
                core::uint16 machine = core::read_le16(data + pe_off + 4);
                if (config.arm_override >= 0 &&
                    (machine == 0x01C0 || machine == 0x01C2 || machine == 0x01C4)) {
                    return FilterType::Arm;
                }
                return FilterType::E8; // Default PE to E8 (x86/x64)
            }
        }
    }

    // 3. ELF executable detection
    if (size >= 20 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        core::uint16 machine = core::read_le16(data + 0x12);
        if (config.e8_override >= 0 && (machine == 0x03 || machine == 0x3E)) { // x86 or x86_64
            return FilterType::E8;
        }
        if (config.arm_override >= 0 && machine == 0x28) { // 32-bit ARM (EM_ARM)
            return FilterType::Arm;
        }
    }

    // 4. Mach-O detection
    if (size >= 8) {
        core::uint32 magic = core::read_le32(data);
        if (magic == 0xFEEDFACEu || magic == 0xFEEDFACFu) { // Mach-O 32 / 64
            core::uint32 cputype = core::read_le32(data + 4);
            if (config.e8_override >= 0 &&
                (cputype == 7 || cputype == 0x01000007u)) { // x86 / x86_64
                return FilterType::E8;
            }
            if (config.arm_override >= 0 && cputype == 12) { // 32-bit ARM (CPU_TYPE_ARM)
                return FilterType::Arm;
            }
        }
    }

    // 5. Opcode density scan on the first 64 KiB (x86 CALL)
    size_t scan_len = std::min<size_t>(size, 65536);
    if (config.e8_override >= 0 && scan_len >= 512) {
        size_t call_count = 0;
        const size_t limit = scan_len - 4;
        size_t i = 0;
        while (i < limit) {
            const void* p = std::memchr(data + i, 0xE8, limit - i);
            if (!p) break;
            i = static_cast<size_t>(static_cast<const core::byte*>(p) - data);
            core::uint32 addr = core::read_le32(data + i + 1);
            // Valid CALL offset heuristic: local jump within 16MB modular cycle
            if (addr < 0x01000000u || addr >= 0xFF000000u) {
                if (++call_count >= 32) return FilterType::E8;
                i += 5;
            } else {
                i += 1;
            }
        }
    }

    // 6. ARM branch density scan (32-bit ARM BL)
    if (config.arm_override >= 0 && scan_len >= 512) {
        size_t arm_count = 0;
        for (size_t i = 0; i + 3 < scan_len; i += 4) {
            if (data[i + 3] == 0xEB) {
                // Check that the branch offset points to a local target within +-2 MB
                core::uint32 off = core::read_le32(data + i) & 0x00FFFFFFu;
                if (off < 0x00080000u || off >= 0x00F80000u) {
                    arm_count++;
                }
            }
        }
        if (arm_count >= 32 && (arm_count * 100 / (scan_len / 4)) >= 1) {
            return FilterType::Arm;
        }
    }

    // 7. WAV / PCM audio detection
    if (config.delta_override >= 0 && size >= 44) {
        if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVEfmt ", 8) == 0) {
            const core::uint32 ch = core::read_le16(data + 22);
            const core::uint32 bps = core::read_le16(data + 34);
            const core::uint32 total_bits = ch * bps;
            const core::uint32 stride = total_bits / 8;
            if ((total_bits % 8 == 0) && stride >= 1 && stride <= 32) {
                out_channels = static_cast<core::uint8>(stride);
                return FilterType::Delta;
            }
        }
    }

    return FilterType::None;
}

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

namespace {

// Delta planar layout (spec 04-filters): channel c's bytes are stored
// contiguously, channels in order. With full = size / channels rows and
// rem = size % channels, channel c holds full + (c < rem) bytes, so its plane
// starts at c * full + min(c, rem).
inline size_t delta_plane_base(size_t full, size_t rem, size_t c) {
    return c * full + (c < rem ? c : rem);
}

// H-08: in-place calls (src == dest) need one scratch buffer the size of the
// region, because the planar<->interleaved transform is a permutation. Reuse
// a per-thread buffer instead of allocating and zero-filling one per call.
core::byte* delta_scratch(size_t size) {
    thread_local std::vector<core::byte> scratch;
    constexpr size_t MAX_CACHED_SCRATCH = 2 * 1024 * 1024;
    if (scratch.capacity() > MAX_CACHED_SCRATCH && size <= MAX_CACHED_SCRATCH) {
        scratch.resize(size);
        scratch.shrink_to_fit();
    } else if (scratch.size() < size) {
        scratch.resize(size);
    }
    return scratch.data();
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
// Broadcast byte 15 of v to all lanes (SSE2 only, no GPR round-trip).
inline __m128i broadcast_last_byte(__m128i v) {
    __m128i t = _mm_unpackhi_epi8(v, v); // words 4..7 = b12b12 .. b15b15
    t = _mm_shufflehi_epi16(t, 0xFF);    // words 4..7 = b15b15
    return _mm_shuffle_epi32(t, 0xFF);   // all dwords = b15b15b15b15
}

// Delta decode of one 16-byte channel run: out[i] = carry - sum(src[0..i]).
// carry holds the previous run's last output, broadcast; updated in place.
inline __m128i delta_decode16(__m128i v, __m128i& carry) {
    __m128i x = _mm_sub_epi8(_mm_setzero_si128(), v);
    x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
    x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
    x = _mm_add_epi8(x, _mm_slli_si128(x, 4));
    x = _mm_add_epi8(x, _mm_slli_si128(x, 8));
    x = _mm_add_epi8(x, carry);
    carry = broadcast_last_byte(x);
    return x;
}

// Delta encode of one 16-byte channel run: out[i] = prev[i] - cur[i], where
// prev is cur shifted by one byte with the previous run's last byte in front.
inline __m128i delta_encode16(__m128i cur, __m128i& last) {
    const __m128i prev = _mm_or_si128(_mm_slli_si128(cur, 1), _mm_srli_si128(last, 15));
    last = cur;
    return _mm_sub_epi8(prev, cur);
}

// Even / odd byte lanes of two vectors, packed (32 interleaved -> 2 x 16).
inline __m128i even_bytes(__m128i a, __m128i b) {
    const __m128i m = _mm_set1_epi16(0x00FF);
    return _mm_packus_epi16(_mm_and_si128(a, m), _mm_and_si128(b, m));
}
inline __m128i odd_bytes(__m128i a, __m128i b) {
    return _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
}
#elif defined(__aarch64__) || defined(_M_ARM64)
inline uint8x16_t delta_decode16(uint8x16_t v, uint8x16_t& carry) {
    const uint8x16_t zero = vdupq_n_u8(0);
    uint8x16_t x = vsubq_u8(zero, v);
    x = vaddq_u8(x, vextq_u8(zero, x, 15));
    x = vaddq_u8(x, vextq_u8(zero, x, 14));
    x = vaddq_u8(x, vextq_u8(zero, x, 12));
    x = vaddq_u8(x, vextq_u8(zero, x, 8));
    x = vaddq_u8(x, carry);
    carry = vdupq_n_u8(vgetq_lane_u8(x, 15));
    return x;
}
inline uint8x16_t delta_encode16(uint8x16_t cur, uint8x16_t& last) {
    const uint8x16_t prev = vextq_u8(last, cur, 15); // [last[15], cur[0..14]]
    last = cur;
    return vsubq_u8(prev, cur);
}
#endif

// H-09: multi-channel Delta decode (planar src -> interleaved out) in ONE
// sequential pass over `out`. The old form made `channels` strided passes
// over the whole region, defeating both vectorisation and the cache; this
// reads `channels` sequential planes in lockstep. SIMD kernels cover the
// common 2- and 4-channel (16-bit mono/stereo PCM) shapes.
void delta_decode_multi(const core::byte* src, core::byte* out, size_t size, size_t channels) {
    const size_t full = size / channels;
    const size_t rem = size % channels;
    size_t base[32];
    for (size_t c = 0; c < channels; ++c) base[c] = delta_plane_base(full, rem, c);

    size_t r = 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (channels == 2) {
        __m128i c0 = _mm_setzero_si128(), c1 = _mm_setzero_si128();
        for (; r + 16 <= full; r += 16) {
            const __m128i a = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[0] + r)), c0);
            const __m128i b = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[1] + r)), c1);
            core::byte* o = out + 2 * r;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o), _mm_unpacklo_epi8(a, b));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 16), _mm_unpackhi_epi8(a, b));
        }
    } else if (channels == 4) {
        __m128i c0 = _mm_setzero_si128(), c1 = c0, c2 = c0, c3 = c0;
        for (; r + 16 <= full; r += 16) {
            const __m128i a = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[0] + r)), c0);
            const __m128i b = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[1] + r)), c1);
            const __m128i c = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[2] + r)), c2);
            const __m128i d = delta_decode16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + base[3] + r)), c3);
            const __m128i ab_lo = _mm_unpacklo_epi8(a, b), ab_hi = _mm_unpackhi_epi8(a, b);
            const __m128i cd_lo = _mm_unpacklo_epi8(c, d), cd_hi = _mm_unpackhi_epi8(c, d);
            core::byte* o = out + 4 * r;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o), _mm_unpacklo_epi16(ab_lo, cd_lo));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 16), _mm_unpackhi_epi16(ab_lo, cd_lo));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 32), _mm_unpacklo_epi16(ab_hi, cd_hi));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 48), _mm_unpackhi_epi16(ab_hi, cd_hi));
        }
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (channels == 2) {
        uint8x16_t c0 = vdupq_n_u8(0), c1 = c0;
        for (; r + 16 <= full; r += 16) {
            uint8x16x2_t v;
            v.val[0] = delta_decode16(vld1q_u8(src + base[0] + r), c0);
            v.val[1] = delta_decode16(vld1q_u8(src + base[1] + r), c1);
            vst2q_u8(out + 2 * r, v);
        }
    } else if (channels == 4) {
        uint8x16_t c0 = vdupq_n_u8(0), c1 = c0, c2 = c0, c3 = c0;
        for (; r + 16 <= full; r += 16) {
            uint8x16x4_t v;
            v.val[0] = delta_decode16(vld1q_u8(src + base[0] + r), c0);
            v.val[1] = delta_decode16(vld1q_u8(src + base[1] + r), c1);
            v.val[2] = delta_decode16(vld1q_u8(src + base[2] + r), c2);
            v.val[3] = delta_decode16(vld1q_u8(src + base[3] + r), c3);
            vst4q_u8(out + 4 * r, v);
        }
    }
#endif
    // Scalar continuation (all other channel counts, and SIMD tails): the
    // running value per channel is the last byte already emitted for it.
    core::byte prev[32];
    for (size_t c = 0; c < channels; ++c) prev[c] = r ? out[(r - 1) * channels + c] : 0;
    for (; r < full; ++r) {
        core::byte* o = out + r * channels;
        for (size_t c = 0; c < channels; ++c) {
            prev[c] = static_cast<core::byte>(prev[c] - src[base[c] + r]);
            o[c] = prev[c];
        }
    }
    for (size_t c = 0; c < rem; ++c) { // channels with one extra (partial-row) byte
        out[full * channels + c] = static_cast<core::byte>(prev[c] - src[base[c] + full]);
    }
}

// H-09: inverse of delta_decode_multi (interleaved src -> planar out), one
// sequential pass over `src`.
void delta_encode_multi(const core::byte* src, core::byte* out, size_t size, size_t channels) {
    const size_t full = size / channels;
    const size_t rem = size % channels;
    size_t base[32];
    for (size_t c = 0; c < channels; ++c) base[c] = delta_plane_base(full, rem, c);

    size_t r = 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (channels == 2) {
        __m128i l0 = _mm_setzero_si128(), l1 = l0;
        for (; r + 16 <= full; r += 16) {
            const core::byte* s = src + 2 * r;
            const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[0] + r),
                             delta_encode16(even_bytes(v0, v1), l0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[1] + r),
                             delta_encode16(odd_bytes(v0, v1), l1));
        }
    } else if (channels == 4) {
        __m128i l0 = _mm_setzero_si128(), l1 = l0, l2 = l0, l3 = l0;
        for (; r + 16 <= full; r += 16) {
            const core::byte* s = src + 4 * r;
            const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
            const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
            const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
            // Two de-interleave rounds: positions 0,2,.. / 1,3,.. then mod 4.
            const __m128i e01 = even_bytes(v0, v1), o01 = odd_bytes(v0, v1);
            const __m128i e23 = even_bytes(v2, v3), o23 = odd_bytes(v2, v3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[0] + r),
                             delta_encode16(even_bytes(e01, e23), l0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[1] + r),
                             delta_encode16(even_bytes(o01, o23), l1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[2] + r),
                             delta_encode16(odd_bytes(e01, e23), l2));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + base[3] + r),
                             delta_encode16(odd_bytes(o01, o23), l3));
        }
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (channels == 2) {
        uint8x16_t l0 = vdupq_n_u8(0), l1 = l0;
        for (; r + 16 <= full; r += 16) {
            const uint8x16x2_t v = vld2q_u8(src + 2 * r);
            vst1q_u8(out + base[0] + r, delta_encode16(v.val[0], l0));
            vst1q_u8(out + base[1] + r, delta_encode16(v.val[1], l1));
        }
    } else if (channels == 4) {
        uint8x16_t l0 = vdupq_n_u8(0), l1 = l0, l2 = l0, l3 = l0;
        for (; r + 16 <= full; r += 16) {
            const uint8x16x4_t v = vld4q_u8(src + 4 * r);
            vst1q_u8(out + base[0] + r, delta_encode16(v.val[0], l0));
            vst1q_u8(out + base[1] + r, delta_encode16(v.val[1], l1));
            vst1q_u8(out + base[2] + r, delta_encode16(v.val[2], l2));
            vst1q_u8(out + base[3] + r, delta_encode16(v.val[3], l3));
        }
    }
#endif
    core::byte prev[32];
    for (size_t c = 0; c < channels; ++c) prev[c] = r ? src[(r - 1) * channels + c] : 0;
    for (; r < full; ++r) {
        const core::byte* s = src + r * channels;
        for (size_t c = 0; c < channels; ++c) {
            out[base[c] + r] = static_cast<core::byte>(prev[c] - s[c]);
            prev[c] = s[c];
        }
    }
    for (size_t c = 0; c < rem; ++c) {
        out[base[c] + full] = static_cast<core::byte>(prev[c] - src[full * channels + c]);
    }
}

} // namespace

bool Filters50::apply_delta(const core::byte* src, core::byte* dest, size_t size,
                            core::uint8 channels) {
    if (channels == 0 || channels > 32 || size == 0) {
        return false;
    }

    if (channels > 1) {
        // Planar -> interleaved is a permutation: in-place needs scratch.
        core::byte* out = (src == dest) ? delta_scratch(size) : dest;
        delta_decode_multi(src, out, size, channels);
        if (out != dest) std::memcpy(dest, out, size);
        return true;
    }

    // channels == 1 below. The kernel reads src[i] before writing dest[i] at
    // the same index, so it is in-place safe: no scratch (H-08).
    size_t pos = 0;
    core::byte prev = 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __m128i carry = _mm_setzero_si128();
    for (; pos + 16 <= size; pos += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + pos));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dest + pos), delta_decode16(v, carry));
    }
    if (pos) prev = dest[pos - 1];
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint8x16_t carry = vdupq_n_u8(0);
    for (; pos + 16 <= size; pos += 16) {
        vst1q_u8(dest + pos, delta_decode16(vld1q_u8(src + pos), carry));
    }
    if (pos) prev = dest[pos - 1];
#endif
    for (; pos < size; ++pos) {
        prev = static_cast<core::byte>(prev - src[pos]);
        dest[pos] = prev;
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

void Filters50::encode_e8(core::byte* data, size_t size, core::uint64 file_offset,
                          bool include_e9) {
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

    if (channels > 1) {
        // Interleaved -> planar is a permutation: in-place needs scratch,
        // reused per thread instead of allocated per call (H-08).
        core::byte* out = (src == dest) ? delta_scratch(size) : dest;
        delta_encode_multi(src, out, size, channels);
        if (out != dest) std::memcpy(dest, out, size);
        return true;
    }

    // channels == 1: out[i] = src[i-1] - src[i]. The previous input byte is
    // carried in a register (never re-read from memory), so this is in-place
    // safe as well (H-08).
    size_t pos = 0;
    core::byte prev = 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __m128i last = _mm_setzero_si128();
    for (; pos + 16 <= size; pos += 16) {
        const __m128i cur = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + pos));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dest + pos), delta_encode16(cur, last));
    }
    if (pos) prev = static_cast<core::byte>(_mm_cvtsi128_si32(_mm_srli_si128(last, 15)));
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint8x16_t last = vdupq_n_u8(0);
    for (; pos + 16 <= size; pos += 16) {
        vst1q_u8(dest + pos, delta_encode16(vld1q_u8(src + pos), last));
    }
    if (pos) prev = vgetq_lane_u8(last, 15);
#endif
    for (; pos < size; ++pos) {
        const core::byte val = src[pos];
        dest[pos] = static_cast<core::byte>(prev - val);
        prev = val;
    }
    return true;
}
} // namespace openrar::compress
