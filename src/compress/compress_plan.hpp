#ifndef OPENRAR_COMPRESS_COMPRESS_PLAN_HPP
#define OPENRAR_COMPRESS_COMPRESS_PLAN_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace openrar::compress {

// Decision on how a single entry payload is encoded into the archive.
enum class EntryDecision {
    Stored,       // Method 0 (uncompressed copy)
    BlockStream,  // Block-based streaming compression (normal LZ compressor)
    WholeMember   // Entire file buffered and compressed as a unit
};

// Execution plan for a single entry.
struct EntryPlan {
    EntryDecision decision{EntryDecision::Stored};
    uint64_t dict_size{0};
    uint32_t method{0};          // 0–5
    bool is_solid_chain{false};  // member continues solid state from previous compressed entry
    bool is_dir{false};          // directory record (no data payload)
};

// Full compression plan constructed from batch parameters and entry metadata.
//
// SOLID ARCHIVE CONSTRAINT NOTE:
// We adopt Option A (collecting all entry metadata prior to planning). Because solid chain
// membership and window-size decisions require global knowledge across all entries in the
// batch, the planner examines all entries up front to generate an immutable plan before
// execution starts.
struct CompressPlan {
    uint64_t dict_size{0};          // Archive-level dictionary choice
    uint32_t method{3};             // Archive-level default method (0-5)
    bool solid{false};
    bool continue_solid_stream{false};
    bool seen_compressed_entry{false};
    std::vector<EntryPlan> entries; // Parallel to the batch's entry list

    EntryPlan plan_next_entry(const EntryPlan& req) {
        EntryPlan ep = req;
        if (ep.is_dir || ep.method == 0) {
            ep.decision = EntryDecision::Stored;
            ep.is_solid_chain = false;
        } else {
            ep.decision = EntryDecision::BlockStream;
            ep.is_solid_chain = (solid || continue_solid_stream) && seen_compressed_entry;
            seen_compressed_entry = true;
        }
        entries.push_back(ep);
        return ep;
    }

    static CompressPlan plan_entries(const std::vector<EntryPlan>& requests, bool solid_mode,
                                     uint32_t default_method = 3, uint64_t default_dict_size = 0,
                                     bool continue_solid_stream = false) {
        CompressPlan cp;
        cp.solid = solid_mode;
        cp.method = default_method;
        cp.dict_size = default_dict_size;
        cp.continue_solid_stream = continue_solid_stream;
        cp.seen_compressed_entry = continue_solid_stream;
        cp.entries.reserve(requests.size());

        for (const auto& req : requests) {
            cp.plan_next_entry(req);
        }
        return cp;
    }
};

struct ExecutionPlan {
    // Resolved from CompressPlan; includes workspace estimates for admission gating.
    std::vector<EntryPlan> entries;
    uint64_t estimated_workspace_bytes{0};
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_COMPRESS_PLAN_HPP
