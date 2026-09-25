#include "collision_detector.hpp"
#include "../unicode/unicode_tables.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace openrar::archive {

namespace {

// Component-split of a '/'-separated archive path (no separators in
// components — the sanitizer guarantees that shape upstream).
std::vector<std::string> split_components(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : path) {
        if (c == '/') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// True when `prefix` (itself a full path) is a strict component-prefix of
// `longer` — i.e. longer == prefix + "/" + something.
bool is_strict_component_prefix(const std::vector<std::string>& prefix,
                                const std::vector<std::string>& longer) {
    if (prefix.size() >= longer.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i] != longer[i]) return false;
    }
    return true;
}

} // namespace

const char* CollisionDetector::unicode_version() {
    return unicode::kUnicodeVersion;
}

bool CollisionDetector::detect(const std::vector<CollisionEntry>& entries,
                               std::vector<CollisionPair>& out) {
    // Canonical views computed once per entry: the raw name (duplicate
    // check), the case-folded name (class_fold), the NFC name (nfc), and
    // the NFC name's component list (file_vs_dir).
    struct Views {
        std::string folded;
        std::string nfc;
        std::vector<std::string> nfc_components;
    };
    std::vector<Views> views(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        views[i].folded = unicode::simple_case_fold(entries[i].name);
        views[i].nfc = unicode::nfc(entries[i].name);
        views[i].nfc_components = split_components(views[i].nfc);
    }

    // name → first index holding it (duplicate-identical + case-fold +
    // nfc maps all report the EARLIEST member so reports are stable).
    std::map<std::string, size_t> by_exact;
    std::map<std::string, size_t> by_folded;
    std::map<std::string, size_t> by_nfc;
    std::set<std::pair<size_t, size_t>> reported; // (i, j) already reported

    const auto add = [&](size_t i, size_t j, const char* cls) {
        if (reported.emplace(i, j).second) {
            out.push_back(CollisionPair{entries[i].name, entries[j].name, cls});
        }
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const CollisionEntry& e = entries[i];
        const Views& v = views[i];

        // duplicate-identical: byte-exact match with an earlier entry.
        const auto dup = by_exact.find(e.name);
        if (dup != by_exact.end())
            add(dup->second, i, "duplicate");
        else
            by_exact.emplace(e.name, i);

        // case-fold: fold-equal to an earlier entry with a DIFFERENT exact
        // name (identical names are reported as duplicates above).
        const auto fold = by_folded.find(v.folded);
        if (fold != by_folded.end() && fold->second != i && entries[fold->second].name != e.name) {
            add(fold->second, i, "case_fold");
        } else if (fold == by_folded.end()) {
            by_folded.emplace(v.folded, i);
        }

        // NFC: NFC-equal to an earlier entry with a different raw name (a
        // byte-equal or fold-equal pair is already reported above).
        const auto norm = by_nfc.find(v.nfc);
        if (norm != by_nfc.end() && norm->second != i && entries[norm->second].name != e.name &&
            views[norm->second].folded != v.folded) {
            add(norm->second, i, "nfc");
        } else if (norm == by_nfc.end()) {
            by_nfc.emplace(v.nfc, i);
        }

        // file-vs-dir: a FILE whose path is a strict component-prefix
        // (after NFC) of another entry — in either registration order —
        // makes the tree un-satisfiable on disk.
        if (!e.is_dir) {
            for (size_t j = 0; j < i; ++j) {
                // (a) this FILE is a prefix of an earlier entry
                if (is_strict_component_prefix(views[i].nfc_components, views[j].nfc_components)) {
                    add(i, j, "file_vs_dir");
                }
                // (b) an earlier FILE is a prefix of this entry
                if (!entries[j].is_dir &&
                    is_strict_component_prefix(views[j].nfc_components, views[i].nfc_components)) {
                    add(j, i, "file_vs_dir");
                }
            }
        }
    }
    return !out.empty();
}

} // namespace openrar::archive
