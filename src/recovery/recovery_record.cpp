#include "recovery_record.hpp"
#include "../crypto/crc32.hpp"
#include <algorithm>
#include <cstring>

namespace openrar::recovery {

RecoveryParams RecoveryManager::calculate_params(core::uint64 protected_size,
                                                 core::uint32 percent) {
    RecoveryParams params;
    params.protected_size = protected_size;
    params.sector_size = RecoveryParams::DEFAULT_SECTOR_SIZE;

    // All geometry math stays in 64 bits: the old uint32 expressions
    // (data_sectors cast, data_sectors * percent) wrapped for large inputs
    // (report H4/L4) and produced a wrong tiny geometry — and in
    // generate_parity a parity buffer sized by the wrapped product with OOB
    // writes. RS16 cannot represent more than GF_SIZE sectors in total, so
    // when even the maximum sector size cannot fit the data, signal failure
    // with zero sector counts (rs16.init rejects them; generate_parity
    // returns empty).
    constexpr core::uint64 MAX_TOTAL_SECTORS = 65530;
    constexpr core::uint64 MAX_SECTOR_SIZE = 1024 * 1024;

    for (;;) {
        core::uint64 data64 = (protected_size + params.sector_size - 1) / params.sector_size;
        if (data64 == 0) data64 = 1;
        core::uint64 rec64 = (data64 * percent + 99) / 100;
        if (rec64 == 0) rec64 = 1;

        if (data64 + rec64 <= MAX_TOTAL_SECTORS) {
            params.data_sectors = static_cast<core::uint32>(data64);
            params.recovery_sectors = static_cast<core::uint32>(rec64);
            return params;
        }
        if (params.sector_size >= MAX_SECTOR_SIZE) break;
        params.sector_size *= 2;
    }

    params.data_sectors = 0;
    params.recovery_sectors = 0;
    return params;
}

std::vector<core::byte> RecoveryManager::generate_parity(const core::byte* protected_data,
                                                         const RecoveryParams& params) {
    // Size in 64 bits (report H4): recovery_sectors * sector_size computed in
    // uint32 wrapped to 0 near the geometry ceiling, allocating an empty
    // buffer that the ECC loop then wrote past.
    if (params.data_sectors == 0 || params.recovery_sectors == 0) return {};
    std::vector<core::byte> parity(
        static_cast<size_t>(params.recovery_sectors) * static_cast<size_t>(params.sector_size), 0);
    std::vector<core::byte> temp_sector(params.sector_size, 0);

    ReedSolomon16 rs;
    if (!rs.init(params.data_sectors, params.recovery_sectors)) {
        return {};
    }

    for (core::uint32 d = 0; d < params.data_sectors; ++d) {
        const core::byte* sector_ptr = nullptr;
        core::uint64 offset = static_cast<core::uint64>(d) * params.sector_size;

        if (offset + params.sector_size <= params.protected_size) {
            sector_ptr = protected_data + offset;
        } else {
            std::memset(temp_sector.data(), 0, params.sector_size);
            if (offset < params.protected_size) {
                size_t remaining = static_cast<size_t>(params.protected_size - offset);
                std::memcpy(temp_sector.data(), protected_data + offset, remaining);
            }
            sector_ptr = temp_sector.data();
        }

        for (core::uint32 r = 0; r < params.recovery_sectors; ++r) {
            core::byte* ecc_ptr = parity.data() + static_cast<size_t>(r) * params.sector_size;
            rs.update_ecc(d, r, sector_ptr, ecc_ptr, params.sector_size);
        }
    }

    return parity;
}

bool RecoveryManager::repair_data(core::byte* protected_data, const core::byte* parity_data,
                                  const RecoveryParams& params,
                                  const std::vector<core::byte>& valid_sectors) {
    if (valid_sectors.size() !=
        static_cast<size_t>(params.data_sectors) + static_cast<size_t>(params.recovery_sectors)) {
        return false;
    }

    core::uint32 missing_data = 0;
    for (core::uint32 i = 0; i < params.data_sectors; ++i) {
        if (!valid_sectors[i]) missing_data++;
    }

    if (missing_data == 0) {
        return true; // Already intact
    }

    core::uint32 valid_parity = 0;
    for (core::uint32 i = params.data_sectors; i < params.data_sectors + params.recovery_sectors;
         ++i) {
        if (valid_sectors[i]) valid_parity++;
    }

    if (missing_data > valid_parity) {
        return false; // Damage exceeds parity redundancy
    }

    ReedSolomon16 rs;
    if (!rs.init(params.data_sectors, params.recovery_sectors, valid_sectors.data())) {
        return false;
    }

    // Map broken data sectors to replacement valid recovery sectors
    std::vector<const core::byte*> proc_sectors(params.data_sectors, nullptr);
    std::vector<core::byte> last_sector_buf(params.sector_size, 0);

    core::uint32 r_idx = params.data_sectors;
    for (core::uint32 i = 0; i < params.data_sectors; ++i) {
        if (valid_sectors[i]) {
            core::uint64 offset = static_cast<core::uint64>(i) * params.sector_size;
            if (offset + params.sector_size <= params.protected_size) {
                proc_sectors[i] = protected_data + offset;
            } else {
                std::memset(last_sector_buf.data(), 0, params.sector_size);
                if (offset < params.protected_size) {
                    size_t rem = static_cast<size_t>(params.protected_size - offset);
                    std::memcpy(last_sector_buf.data(), protected_data + offset, rem);
                }
                proc_sectors[i] = last_sector_buf.data();
            }
        } else {
            while (r_idx < valid_sectors.size() && !valid_sectors[r_idx]) {
                r_idx++;
            }
            if (r_idx >= valid_sectors.size()) {
                return false;
            }
            core::uint32 rec_num = r_idx - params.data_sectors;
            proc_sectors[i] = parity_data + static_cast<size_t>(rec_num) * params.sector_size;
            r_idx++;
        }
    }

    // Decode reconstructed sectors (outer loop j data_num, inner loop e ecc_num)
    std::vector<core::byte> reconstructed_buf(
        static_cast<size_t>(missing_data) * static_cast<size_t>(params.sector_size), 0);

    for (core::uint32 j = 0; j < params.data_sectors; ++j) {
        for (core::uint32 e = 0; e < missing_data; ++e) {
            core::byte* dest =
                reconstructed_buf.data() + static_cast<size_t>(e) * params.sector_size;
            rs.update_ecc(j, e, proc_sectors[j], dest, params.sector_size);
        }
    }

    // Write repaired sectors back into protected_data
    core::uint32 cur_missing = 0;
    for (core::uint32 i = 0; i < params.data_sectors; ++i) {
        if (!valid_sectors[i]) {
            core::uint64 offset = static_cast<core::uint64>(i) * params.sector_size;
            size_t copy_len = params.sector_size;
            if (offset + copy_len > params.protected_size) {
                // offset can exceed protected_size only with caller-supplied
                // params inconsistent with the buffer; clamp to 0 instead of
                // underflowing into a huge memcpy (report H4).
                copy_len = offset < params.protected_size
                               ? static_cast<size_t>(params.protected_size - offset)
                               : 0;
            }
            if (copy_len > 0) {
                std::memcpy(protected_data + offset,
                            reconstructed_buf.data() +
                                static_cast<size_t>(cur_missing) * params.sector_size,
                            copy_len);
            }
            cur_missing++;
        }
    }

    return true;
}

} // namespace openrar::recovery
