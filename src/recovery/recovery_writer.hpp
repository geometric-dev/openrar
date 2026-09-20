#ifndef OPENRAR_RECOVERY_RECOVERY_WRITER_HPP
#define OPENRAR_RECOVERY_RECOVERY_WRITER_HPP

#include "../core/types.hpp"
#include "recovery_record.hpp"
#include "../io/file_stream.hpp"
#include <filesystem>
#include <vector>

namespace openrar::recovery {

class RecoveryWriter {
public:
    // Calculate RR params using ceil(data_sectors*percent/100).
    static RecoveryParams calculate_params(core::uint64 protected_size, core::uint32 percent);

    static std::vector<core::byte> generate_parity(const core::byte* protected_data,
                                                   const RecoveryParams& params);

    // Low-level helpers
    static bool write_rr_header(io::FileStream& dest, const std::vector<core::byte>& parity);

    // Generate external .rev recovery volumes for a multi-volume set
    // (spec INTEGRITY_WRITE_SIDE.md §4.7). Each data volume acts as one
    // RS16 data shard; `percent` (1..1000, as for -rr) picks the number of
    // parity shards, written as archive.partNN.rev files next to the data
    // volumes. Returns false when the volume chain can't be enumerated or
    // the set is too large for the 16-bit codec. `threads` > 1 folds each
    // data chunk into the parity shards on a worker pool; output bytes are
    // identical to the serial run.
    static bool write_rev_volumes(const std::filesystem::path& arc_path,
                                  core::uint32 count_or_percent,
                                  bool is_percent,
                                  unsigned threads = 1);
    static bool write_rev_volumes(const std::filesystem::path& arc_path, core::uint32 percent,
                                  unsigned threads = 1) {
        return write_rev_volumes(arc_path, percent, /*is_percent=*/true, threads);
    }
    static bool patch_locator_recovery_offset(const std::filesystem::path& tmp_path,
                                              core::uint64 main_start, core::uint64 rr_offset);

    // High-level API per 05-recovery.md:28.
    // `threads` > 1 parallelizes the parity fold over the NR parity shards
    // (each shard's parity buffer is written by exactly one job); output
    // bytes are identical to the serial run.
    static bool add_recovery_record(const std::filesystem::path& arc_path, core::uint32 percent,
                                    unsigned threads = 1);
    static bool repair(const std::filesystem::path& arc_path);
    static bool has_rev_files(const std::filesystem::path& arc_path);

private:
    // Reconstruct missing/corrupt data volumes from external .rev parity
    // files (spec §4.7). Used by repair() when .rev siblings are present.
    static bool repair_rev_volumes(const std::filesystem::path& arc_path);
};

} // namespace openrar::recovery

#endif // OPENRAR_RECOVERY_RECOVERY_WRITER_HPP
