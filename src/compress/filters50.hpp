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
    Arm = 3,   // ARM BL (1110) translation
    None = 255 // No filter
};

enum class FilterMode { Auto, DisableAll };

struct FilterConfig {
    FilterMode mode{FilterMode::Auto};
    int e8_override{0};            // 0: auto, 1: force, -1: disable
    int arm_override{0};           // 0: auto, 1: force, -1: disable
    int delta_override{0};         // 0: auto, 1: force, -1: disable
    core::uint8 delta_channels{0}; // 0: auto
};

struct FilterBlock {
    FilterType type{FilterType::None};
    core::uint8 channels{1};
    size_t block_start{0};
    core::uint32 block_length{0};
};

class Filters50 {
public:
    // Heuristic filter detection
    static FilterType detect_filter(const core::byte* data, size_t size, core::uint8& out_channels,
                                    const FilterConfig& config = {});
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

    // Forward filter transforms (compression side: transforms uncompressed data to pre-filtered representation)
    static void encode_e8(core::byte* data, size_t size, core::uint64 file_offset,
                          bool include_e9 = false);
    static void encode_e8_scalar(core::byte* data, size_t size, core::uint64 file_offset,
                                 bool include_e9 = false);
    static void encode_arm(core::byte* data, size_t size, core::uint64 file_offset);
    static void encode_arm_scalar(core::byte* data, size_t size, core::uint64 file_offset);
    static bool encode_delta(const core::byte* src, core::byte* dest, size_t size,
                             core::uint8 channels);
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_FILTERS50_HPP
