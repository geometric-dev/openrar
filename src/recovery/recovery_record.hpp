#ifndef OPENRAR_RECOVERY_RECOVERY_RECORD_HPP
#define OPENRAR_RECOVERY_RECOVERY_RECORD_HPP

#include "../core/types.hpp"
#include "rs16.hpp"
#include <vector>
#include <optional>
#include <limits>

namespace openrar::recovery {

static constexpr core::uint64 MAX_PARITY_BUFFER_CAP = 2ULL * 1024 * 1024 * 1024; // 2 GiB cap

inline std::optional<core::uint64> calculate_parity_buffer_size(core::uint64 count,
                                                                core::uint64 unit_size) {
    if (count == 0 || unit_size == 0) return std::nullopt;
    if (count > MAX_PARITY_BUFFER_CAP / unit_size) return std::nullopt;
    core::uint64 total = count * unit_size;
    if (total > std::numeric_limits<size_t>::max() || total > MAX_PARITY_BUFFER_CAP) {
        return std::nullopt;
    }
    return total;
}

// RAR 5.0 Recovery Record specification
struct RecoveryParams {
    static constexpr core::uint32 DEFAULT_SECTOR_SIZE = 512;
    static constexpr core::uint16 RECOVERY_RECORD_MAGIC = 0x4252; // "RB"

    core::uint32 sector_size{DEFAULT_SECTOR_SIZE};
    core::uint64 protected_size{0};
    core::uint32 data_sectors{0};
    core::uint32 recovery_sectors{0};
    core::uint32 protected_crc{0};
};

class RecoveryManager {
public:
    // Calculate required sector size and counts based on protected size and desired percent (e.g. 5%)
    static RecoveryParams calculate_params(core::uint64 protected_size, core::uint32 percent);

    // Generate parity payload across a buffer of data
    static std::vector<core::byte> generate_parity(const core::byte* protected_data,
                                                   const RecoveryParams& params);

    // Verify and repair damaged sectors in-place
    static bool repair_data(core::byte* protected_data, const core::byte* parity_data,
                            const RecoveryParams& params,
                            const std::vector<core::byte>& valid_sectors);
};

} // namespace openrar::recovery

#endif // OPENRAR_RECOVERY_RECOVERY_RECORD_HPP
