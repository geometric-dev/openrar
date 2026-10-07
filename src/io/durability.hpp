#ifndef OPENRAR_IO_DURABILITY_HPP
#define OPENRAR_IO_DURABILITY_HPP

namespace openrar::io {

// ── Durability granularity (v1.39.0) ────────────────────────────────────────
//
// Two levels. `entry` is the shipped v1.24 contract: per-file journal record
// sync, per-file temp flush, journal closed+unlinked at zero in-flight.
// `batch` is the opt-in revision: record sync deferred to a boundary (every
// 32 appends, LRU eviction, session teardown), no temp-data flush, journals
// held for the session under an LRU cap. The durable-first ORDER (record
// appended before the temp exists) is preserved in both modes.
//
// Kept in its own header so ArchiveReader (which surfaces the policy) does
// not have to pull in the whole extraction-journal implementation surface.
enum class DurabilityGranularity { Entry, Batch };

} // namespace openrar::io

#endif // OPENRAR_IO_DURABILITY_HPP
