#ifndef OPENRAR_ARCHIVE_COLLISION_DETECTOR_HPP
#define OPENRAR_ARCHIVE_COLLISION_DETECTOR_HPP

#include <string>
#include <vector>

namespace openrar::archive {

// ── Archive-internal collision detection (v1.24.0 plan §3, gate 2) ──────────
//
// An archive that names the same on-disk object twice is lying about its own
// structure: extraction aborts (exit 2 / structural error) BEFORE anything
// is written. Four classes are detected over the final merged entry list
// (service headers filtered), each compared after the trimming pipeline the
// caller applies when building the extraction set:
//   duplicate  — byte-exact name match
//   case_fold  — equal under Unicode simple case folding (Readme vs README)
//   nfc        — equal under NFC canonical composition (NFD vs NFC café;
//                APFS compares normalization-insensitively)
//   file_vs_dir — one name is a strict component-prefix of another and the
//                shorter entry is a file (foo (file) vs foo/bar)
struct CollisionPair {
    std::string first;
    std::string second;
    std::string cls; // "duplicate" | "case_fold" | "nfc" | "file_vs_dir"
};

struct CollisionEntry {
    std::string name; // archive name as it will be extracted
    bool is_dir;
};

class CollisionDetector {
public:
    // Detects all four classes. Returns true when at least one collision
    // was found; `out` receives one report per colliding pair (a name can
    // appear in several reports). Order is deterministic: input order of
    // the first colliding member, then class detection order.
    static bool detect(const std::vector<CollisionEntry>& entries, std::vector<CollisionPair>& out);

    // Unicode data version used by the fold/NFC comparisons (JSON summary
    // surface; v1.24 plan §3.3).
    static const char* unicode_version();
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_COLLISION_DETECTOR_HPP
