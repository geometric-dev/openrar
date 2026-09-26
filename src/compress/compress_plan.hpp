#ifndef OPENRAR_COMPRESS_COMPRESS_PLAN_HPP
#define OPENRAR_COMPRESS_COMPRESS_PLAN_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace openrar::compress {

// Default dictionary size helper for RAR5 compression methods 0-5.
inline uint64_t default_dict_size_for_method(uint32_t method) {
    switch (method) {
    case 0:
        return 0x20000ULL; // 128 KB
    case 1:
        return 0x80000ULL; // 512 KB
    case 2:
        return 0x100000ULL; // 1 MB
    case 3:
        return 0x800000ULL; // 8 MB  (balanced speed, ratio & L3 cache)
    case 4:
        return 0x1000000ULL; // 16 MB
    case 5:
        return 0x4000000ULL; // 64 MB (matches WinRAR Best profile)
    default:
        return 0x800000ULL;
    }
}

// Decision on how a single entry payload is encoded into the archive.
enum class EntryDecision {
    Stored,      // Method 0 (uncompressed copy)
    BlockStream, // Block-based streaming compression (normal LZ compressor)
    WholeMember  // Entire file buffered and compressed as a unit
};

// Execution plan for a single entry.
struct EntryPlan {
    EntryDecision decision{EntryDecision::Stored};
    uint64_t dict_size{0};
    uint32_t method{0};             // 0–5
    bool is_solid_chain{false};     // member continues solid state from previous compressed entry
    bool is_dir{false};             // directory record (no data payload)
    bool breaks_solid_chain{false}; // no-data-area entry the run breaks around (FILECOPY redir)
    uint64_t raw_size{0};           // input uncompressed size
    uint64_t estimated_workspace_bytes{0}; // estimated workspace RAM to prepare/compress
};

// Full compression plan constructed from batch parameters and entry metadata.
//
// SOLID ARCHIVE CONSTRAINT NOTE:
// We adopt Option A (collecting all entry metadata prior to planning). Because solid chain
// membership and window-size decisions require global knowledge across all entries in the
// batch, the planner examines all entries up front to generate an immutable plan before
// execution starts.
struct CompressPlan {
    uint64_t dict_size{0}; // Archive-level dictionary choice
    uint32_t method{3};    // Archive-level default method (0-5)
    bool solid{false};
    bool continue_solid_stream{false};
    bool seen_compressed_entry{false};
    std::vector<EntryPlan> entries; // Parallel to the batch's entry list

    EntryPlan plan_next_entry(const EntryPlan& req) {
        EntryPlan ep = req;
        if (ep.breaks_solid_chain) {
            // FILECOPY references carry no data area (v1.26 plan §2.2): they
            // can never be solid-chain members, and the run BREAKS around
            // them — the next data-bearing entry starts a fresh chain
            // (pinned by the cdc_filecopy_precedence test). Directory and
            // stored entries keep the opposite behavior: they do not break
            // the chain, because the window state simply persists across a
            // data area that is never decompressed.
            ep.decision = EntryDecision::Stored;
            ep.is_solid_chain = false;
            ep.dict_size = 0;
            seen_compressed_entry = false;
        } else if (ep.is_dir || ep.method == 0) {
            ep.decision = EntryDecision::Stored;
            ep.is_solid_chain = false;
            ep.dict_size = 0;
        } else {
            ep.decision = EntryDecision::BlockStream;
            if (ep.dict_size == 0) {
                ep.dict_size =
                    (dict_size != 0) ? dict_size : default_dict_size_for_method(ep.method);
            }
            ep.is_solid_chain = (solid || continue_solid_stream) && seen_compressed_entry;
            seen_compressed_entry = true;
        }
        uint64_t ws = 0;
        if (ep.decision == EntryDecision::BlockStream) {
            // Compressor50 allocates buf_ (win_size + 5 MB), head_ (512 KB), and prev_ (4 * win_size)
            ws = (ep.dict_size * 5) + (6ULL * 1024ULL * 1024ULL);
        } else {
            // Stored: in-memory payload if < 16 MiB, otherwise 1 MiB streaming buffer
            ws = std::min<uint64_t>(ep.raw_size, 16ULL * 1024ULL * 1024ULL);
        }
        if (ws == 0) ws = 1; // 1-byte floor keeps empty files inside budget tracking
        ep.estimated_workspace_bytes = ws;
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
    uint64_t estimated_workspace_bytes{0}; // Cumulative estimated workspace across all entries
    uint64_t peak_entry_workspace{0};      // Peak single-entry workspace

    static ExecutionPlan from_compress_plan(const CompressPlan& cp) {
        ExecutionPlan ep;
        ep.entries = cp.entries;
        uint64_t total = 0;
        uint64_t peak = 0;
        for (const auto& entry : ep.entries) {
            total += entry.estimated_workspace_bytes;
            if (entry.estimated_workspace_bytes > peak) {
                peak = entry.estimated_workspace_bytes;
            }
        }
        ep.estimated_workspace_bytes = total;
        ep.peak_entry_workspace = peak;
        return ep;
    }
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_COMPRESS_PLAN_HPP
