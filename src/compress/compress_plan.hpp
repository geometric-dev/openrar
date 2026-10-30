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
        return 0x4000000ULL; // 64 MB (matches Best profile)
    default:
        return 0x800000ULL;
    }
}

// Sentinel for -md=auto (automatic input-scaled dictionary sizing)
inline constexpr uint64_t DICT_SIZE_AUTO = UINT64_MAX;

// Quantize a requested window to the FCI grid the header can represent
// (power-of-two base + base/32 fractions, 128 KiB floor). Single copy shared
// by the mutator's header bookkeeping and the CLI's solid-session
// construction — the packer's window and the recorded win_size must match
// exactly for every entry of a solid run.
inline uint64_t snap_window_to_fci_grid(uint64_t win) {
    if (win < 0x20000ULL) return win; // store/small windows are not FCI-encoded
    uint64_t pow2 = 0x20000ULL;
    uint32_t bits = 0;
    while (2 * pow2 <= win && bits < 23) {
        pow2 *= 2;
        bits++;
    }
    if (win <= pow2) return pow2;
    if (bits == 23) return pow2 + (pow2 / 32) * 31; // clamp to max representable
    uint64_t fraction = (win - pow2) * 32 / pow2;
    return pow2 + (pow2 / 32) * fraction;
}

// Ceiling for automatic dictionary sizing per RAR5 method 0-5.
inline uint64_t auto_dict_ceiling_for_method(uint32_t method) {
    switch (method) {
    case 0:
        return 0x20000ULL; // 128 KiB
    case 1:
        return 0x80000ULL; // 512 KiB
    case 2:
        return 0x200000ULL; // 2 MiB
    case 3:
        return 0x4000000ULL; // 64 MiB
    case 4:
        return 0x4000000ULL; // 64 MiB
    case 5:
        return 0x10000000ULL; // 256 MiB
    default:
        return 0x4000000ULL; // 64 MiB
    }
}

// Automatic dictionary sizing for an input of known size (v1.46 -md=auto):
// Single pure decision function f(method, input_size) -> window.
// No RAM input is permitted (Invariant G-3, cross-host byte-identity).
// - Floor: 128 KiB (0x20000ULL).
// - Small inputs (< method default): round_up_pow2(max(128k, input_size)).
// - Large inputs (>= method default): scales above method default up to the per-method ceiling.
inline uint64_t resolve_auto_dict(uint32_t method, uint64_t input_size) {
    const uint64_t def = default_dict_size_for_method(method);
    if (input_size == 0) return def;
    if (input_size < def) {
        uint64_t w = 0x20000ULL;
        while (w < input_size && w < def) w <<= 1;
        return snap_window_to_fci_grid(w);
    }
    const uint64_t ceiling = auto_dict_ceiling_for_method(method);
    uint64_t w = def;
    while (w < input_size && w < ceiling) w <<= 1;
    return snap_window_to_fci_grid(w);
}

// Full dictionary-window resolution shared by every archive pack path (the
// non-volume prepare, the multivolume add, the CLI's solid-session and
// workspace math):
// - DICT_SIZE_AUTO: dynamic input-scaled auto-sizing (resolve_auto_dict).
// - 1..15: legacy 128 KiB<<n scale.
// - 0: method-tuned default (with shrink-only clamp if input_size > 0).
// - >15: exact bytes.
inline uint64_t resolve_dict_window_size(uint64_t dict_size, uint32_t method,
                                         uint64_t input_size = 0) {
    if (dict_size == DICT_SIZE_AUTO) return resolve_auto_dict(method, input_size);
    if (dict_size >= 1 && dict_size <= 15) return 0x20000ULL << (dict_size - 1);
    if (dict_size == 0) {
        if (input_size > 0) {
            uint64_t def = default_dict_size_for_method(method);
            uint64_t w = 0x20000ULL;
            while (w < input_size && w < def) w <<= 1;
            return w;
        }
        return default_dict_size_for_method(method);
    }
    return dict_size;
}

// LZ allocation window for an input of known size: the smallest power of two
// covering the input (128 KiB floor), never above the declared dictionary.
// The header keeps the DECLARED size - a decoder window larger than every
// distance the stream uses is always valid - so this only bounds encoder
// memory (match-finder state is ~5 bytes per window byte: -md8g on a 24 MiB
// file used to try to allocate ~40 GiB). Safe because the table flavor no
// longer depends on the window (see Compressor50::make_tables). Solid
// sessions pass the whole run's input size.
inline uint64_t lz_window_for_input(uint64_t declared, uint64_t input_bytes) {
    if (declared <= 0x20000ULL || input_bytes == 0) return declared;
    uint64_t w = 0x20000ULL;
    while (w < input_bytes && w < declared) w <<= 1;
    return w < declared ? w : declared;
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
    uint32_t method{0};          // 0–5
    bool is_solid_chain{false};  // member continues solid state from previous compressed entry
    bool is_dir{false};          // directory record (no data payload)
    bool is_filecopy_ref{false}; // FILECOPY redir (no data area; window persists across it)
    uint64_t raw_size{0};        // input uncompressed size
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
        if (ep.is_filecopy_ref) {
            // FILECOPY references carry no data area (v1.26 plan §2.2) and can
            // never BE data-bearing chain members, but since v1.40.0 they no
            // longer BREAK the run either: the window persists across them
            // exactly like a stored entry, and the reference header itself
            // carries the solid bit while a chain is active. This ensures
            // one continuous run with refs interleaved and marked solid;
            // breaking runs around refs produces a mid-archive run-restart
            // pattern that multithreaded unpackers can mishandle.
            ep.decision = EntryDecision::Stored;
            ep.is_solid_chain = (solid || continue_solid_stream) && seen_compressed_entry;
            ep.dict_size = 0;
        } else if (ep.is_dir || ep.method == 0) {
            ep.decision = EntryDecision::Stored;
            ep.is_solid_chain = false;
            ep.dict_size = 0;
        } else {
            ep.decision = EntryDecision::BlockStream;
            if (ep.dict_size == 0) {
                ep.dict_size = resolve_dict_window_size(
                    (dict_size != 0) ? dict_size : default_dict_size_for_method(ep.method),
                    ep.method, ep.raw_size);
            }
            ep.is_solid_chain = (solid || continue_solid_stream) && seen_compressed_entry;
            seen_compressed_entry = true;
        }
        uint64_t ws = 0;
        if (ep.decision == EntryDecision::BlockStream) {
            constexpr uint64_t MAX_ALLOWED_DICT = 64ULL * 1024ULL * 1024ULL * 1024ULL; // 64 GiB
            const uint64_t clamped_dict = std::min(ep.dict_size, MAX_ALLOWED_DICT);
            // Compressor50 allocates buf_ (win_size + 5 MB), head_ (512 KB), and prev_ (4 * win_size)
            ws = (clamped_dict * 5) + (6ULL * 1024ULL * 1024ULL);
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
