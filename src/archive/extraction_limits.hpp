#ifndef OPENRAR_ARCHIVE_EXTRACTION_LIMITS_HPP
#define OPENRAR_ARCHIVE_EXTRACTION_LIMITS_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/archive/extraction_limits.hpp
//
//  Resource limit configuration for extraction operations, inspired by the
//  rars project's WRITER_RESOURCE_CONTRACT.md pattern of explicit, enforceable
//  caller-imposed caps.
//
//  Design notes:
//   - All limits use uint64_t with UINT64_MAX as the "unlimited" sentinel.
//     This avoids std::optional<> on the hot per-chunk accumulation path and
//     lets the compiler keep the sentinel check in a register (no heap touch,
//     no has_value() branch per chunk in the common unlimited case).
//   - LimitState is the mutable accumulator threaded across entries. It is
//     owned by the *caller* (the DLL/WASM orchestration layer), not the reader,
//     so that max_total_output_bytes is cumulative across all entries in an
//     extract_all call without requiring the reader to store per-session state.
//   - ExtractionLimits is immutable after construction and may be shared
//     across concurrent readers (const-only access).
//
//  Contract: documented in docs/EXTRACTION_CONTRACT.md Section 7.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <limits>

namespace openrar::archive {

// Immutable limit configuration. UINT64_MAX in any field means "no limit".
// Pass as const ExtractionLimits* to extraction methods; nullptr == all unlimited.
struct ExtractionLimits {
    static constexpr uint64_t UNLIMITED = std::numeric_limits<uint64_t>::max();

    // Maximum uncompressed output bytes allowed for a single member.
    // Checked in-flight per output chunk -- fires even when FHFL_UNPUNKNOWN
    // is set (unp_size == 0 at scan time, so pre-flight checks are insufficient).
    uint64_t max_member_output_bytes{UNLIMITED};

    // Maximum total uncompressed output bytes across ALL members in an
    // extraction call. Never reset between members. Must be owned and threaded
    // through a shared LimitState by the caller (see below).
    uint64_t max_total_output_bytes{UNLIMITED};

    // Maximum number of block headers parsed across all volumes in the set.
    // Checked during scan_archive. Cumulative across volumes -- not reset
    // per volume (docs/EXTRACTION_CONTRACT.md Section 7).
    uint64_t max_header_count{UNLIMITED};

    // Maximum cumulative header bytes parsed across all volumes in the set.
    uint64_t max_header_bytes{UNLIMITED};

    bool member_limited() const noexcept { return max_member_output_bytes != UNLIMITED; }
    bool total_limited() const noexcept { return max_total_output_bytes != UNLIMITED; }
    bool hdr_count_limited() const noexcept { return max_header_count != UNLIMITED; }
    bool hdr_bytes_limited() const noexcept { return max_header_bytes != UNLIMITED; }
};

// Mutable accumulator for limit tracking. Created and owned by the caller;
// passed into each extraction call by pointer so totals span entry boundaries.
//
//   member_out  -- reset to 0 at the start of EACH member extract call.
//   total_out   -- NEVER reset; accumulates across the entire session.
//   header_count/header_bytes -- cumulative across volumes in scan_archive.
//
// Initialise with LimitState{} (zero-initialised) before the first call.
struct LimitState {
    uint64_t member_out{0};   // bytes produced by current member (reset per entry)
    uint64_t total_out{0};    // cumulative bytes across all members (never reset)
    uint64_t header_count{0}; // cumulative headers parsed across volumes
    uint64_t header_bytes{0}; // cumulative header bytes parsed across volumes
};

// ── Timestamp clamping (v1.24 plan §4.4) ────────────────────────────────────
// Absurd archive mtimes clamp to parameterized bounds; a clamped value feeds
// the skip-with-report path (timestamp_clamped security flag in the JSON
// summary). Defaults: 1970-01-01 .. 3000-01-01 (unix seconds).
struct MtimeBounds {
    int64_t min{0};
    int64_t max{32503680000};
};

inline int64_t clamp_mtime(int64_t t, const MtimeBounds& bounds = MtimeBounds{}) {
    if (t < bounds.min) return bounds.min;
    if (t > bounds.max) return bounds.max;
    return t;
}

inline bool mtime_out_of_bounds(int64_t t, const MtimeBounds& bounds = MtimeBounds{}) {
    return t < bounds.min || t > bounds.max;
}

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_EXTRACTION_LIMITS_HPP
