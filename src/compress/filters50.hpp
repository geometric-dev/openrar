#ifndef OPENRAR_COMPRESS_FILTERS50_HPP
#define OPENRAR_COMPRESS_FILTERS50_HPP

#include "../core/types.hpp"
#include <cstddef>
#include <vector>

namespace openrar::compress {

enum class FilterType : core::uint8 {
    Delta = 0, // Delta predictive byte decorrelation
    E8 = 1,    // x86 CALL rel32 translation
    E8E9 = 2,  // x86 CALL+JMP rel32 translation
    Arm = 3    // ARM BL (1110) translation
};

struct FilterBlock {
    FilterType type{FilterType::Delta};
    core::uint8 channels{1};
    size_t block_start{0};
    core::uint32 block_length{0};
};

class Filters50 {
public:
    // x86 CALL/JMP relative address translation
    static void apply_e8(core::byte* data, size_t size, core::uint64 file_offset,
                         bool include_e9 = false);
    static void apply_e8_scalar(core::byte* data, size_t size, core::uint64 file_offset,
                                bool include_e9 = false);

    // ARM BL branch instruction relative address translation
    static void apply_arm(core::byte* data, size_t size, core::uint64 file_offset);
    static void apply_arm_scalar(core::byte* data, size_t size, core::uint64 file_offset);

    // Delta audio/image channel decorrelation inverse (returns false if channels invalid)
    static bool apply_delta(const core::byte* src, core::byte* dest, size_t size,
                            core::uint8 channels);
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_FILTERS50_HPP
