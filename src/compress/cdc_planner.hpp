#ifndef OPENRAR_COMPRESS_CDC_PLANNER_HPP
#define OPENRAR_COMPRESS_CDC_PLANNER_HPP

#include "../core/types.hpp"

#include <string>
#include <vector>

namespace openrar::compress {

// Hard cap on fingerprint-index entries (v1.26 plan §1.2): 2,000,000 entries
// x 16 bytes (u64 hash + u32 file-idx + u32 chunk-idx) = 32 MiB. Files whose
// fingerprints no longer fit keep their original packing order, and the
// fallback is reported on output (challenge directive 5 — no silent
// degradation). OPENRAR_CDC_INDEX_CAP may LOWER the cap (tests); it can
// never raise it.
size_t cdc_fingerprint_cap();

struct CdcPlanResult {
    std::vector<size_t> order; // permutation of 0..n-1: the packing order
    bool reordered{false};     // order differs from the original order
};

// Affinity planner (v1.26 plan §1.3): greedy grouping over chunk-hash
// overlap. A file joins the group of its best earlier partner when they
// share >= affinity_permille/1000 of the smaller file's chunk hashes; the
// emitted order concatenates groups — members in original index order,
// groups in the order their first member appears. Deterministic by
// construction: integer threshold math, canonical tie-breaks (smallest
// index wins), no time or thread-order inputs. Files with an empty hash
// list (degenerate inputs, index-cap fallback) never match: each becomes
// its own group, i.e. original relative order (plan directive 7).
//
// reference_of (optional, parallel to `hashes`): for -oi FILECOPY /
// hardlink reference items, the batch index of the stored master (skip with
// SIZE_MAX or any out-of-range value). A reference always joins its
// master's group so it can never land before the entry it references —
// extraction materializes references only after their target exists.
CdcPlanResult plan_cdc_order(const std::vector<std::vector<core::uint64>>& hashes,
                             const std::vector<size_t>& reference_of = {},
                             size_t affinity_permille = 300);

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_CDC_PLANNER_HPP
