#include "cdc_planner.hpp"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace openrar::compress {

namespace {

// Fingerprint-index entry: (chunk hash, batch file index). The plan §1.2
// entry shape also carries a u32 chunk index; grouping never reads it, so
// the planner's index is 12 bytes/entry — under the 16-byte budget.
struct HashEntry {
    core::uint64 hash;
    core::uint32 file;
};

bool hash_entry_less(const HashEntry& a, const HashEntry& b) {
    if (a.hash != b.hash) return a.hash < b.hash;
    return a.file < b.file;
}

} // namespace

size_t cdc_fingerprint_cap() {
    size_t cap = 2000000; // plan §1.2: 2M entries, 32 MiB
    const char* env = std::getenv("OPENRAR_CDC_INDEX_CAP");
    if (env && *env) {
        size_t v = 0;
        bool ok = true;
        for (const char* p = env; *p; ++p) {
            if (*p < '0' || *p > '9') {
                ok = false;
                break;
            }
            if (v > cap) { // already above the ceiling: stop growing
                v = cap + 1;
                break;
            }
            v = v * 10 + static_cast<size_t>(*p - '0');
        }
        if (ok && v >= 1 && v < cap) cap = v;
    }
    return cap;
}

CdcPlanResult plan_cdc_order(const std::vector<std::vector<core::uint64>>& hashes,
                             const std::vector<size_t>& reference_of, size_t affinity_permille) {
    const size_t n = hashes.size();
    CdcPlanResult result;
    result.order.resize(n);
    if (n == 0) return result;

    // Flat index over every fingerprinted chunk, sorted by (hash, file): a
    // file's per-chunk lookups equal_range into it and see earlier files in
    // ascending index order.
    std::vector<HashEntry> index;
    size_t total_chunks = 0;
    for (const auto& h : hashes) total_chunks += h.size();
    index.reserve(total_chunks);
    for (size_t i = 0; i < n; ++i) {
        for (core::uint64 h : hashes[i]) index.push_back({h, static_cast<core::uint32>(i)});
    }
    std::sort(index.begin(), index.end(), hash_entry_less);

    // Greedy grouping in original order. For file i, count shared chunk
    // hashes against earlier files; at most the first 8 earlier sharers per
    // hash are consulted (representatives) so a hash shared by many files
    // cannot blow the pass up (crafted-input bound, plan FMM row C).
    constexpr size_t kMaxRepresentatives = 8;
    std::vector<size_t> group_of(n, n); // n = "no group yet"
    std::vector<std::vector<size_t>> groups;
    std::unordered_map<core::uint32, core::uint32> shared;
    shared.reserve(64);
    for (size_t i = 0; i < n; ++i) {
        shared.clear();
        if (!hashes[i].empty()) {
            for (core::uint64 h : hashes[i]) {
                const HashEntry probe{h, 0};
                auto lo = std::lower_bound(index.begin(), index.end(), probe, hash_entry_less);
                size_t reps = 0;
                for (auto it = lo; it != index.end() && it->hash == h; ++it) {
                    if (it->file >= i) break; // (hash, file) order: earlier sharers exhausted
                    if (reps++ >= kMaxRepresentatives) break;
                    shared[it->file]++;
                }
            }
        }
        // Best partner: highest shared count; ties break to the smallest
        // file index. The argmax scan is order-independent, so the result
        // is deterministic even though map iteration order is not.
        size_t best = n;
        core::uint32 best_count = 0;
        for (const auto& kv : shared) {
            if (kv.second > best_count || (kv.second == best_count && kv.first < best)) {
                best = kv.first;
                best_count = kv.second;
            }
        }
        bool joined = false;
        if (best < i) {
            const size_t smaller = std::min(hashes[i].size(), hashes[best].size());
            if (smaller > 0 && static_cast<core::uint64>(best_count) * 1000 >=
                                   static_cast<core::uint64>(affinity_permille) * smaller) {
                group_of[i] = group_of[best];
                groups[group_of[i]].push_back(i);
                joined = true;
            }
        }
        if (!joined) {
            group_of[i] = groups.size();
            groups.push_back({i});
        }
    }

    // Reference fixup: a FILECOPY/hardlink reference joins its master's
    // group (the master always has the smaller batch index). Members are
    // re-sorted ascending afterwards, so within-group emission stays in
    // original order and a reference can never precede its target.
    if (!reference_of.empty()) {
        for (size_t i = 0; i < n; ++i) {
            if (i >= reference_of.size()) break;
            const size_t master = reference_of[i];
            if (master >= i || master >= n) continue; // defensive: masters precede references
            const size_t from = group_of[i];
            const size_t to = group_of[master];
            if (from == to) continue;
            auto& src = groups[from];
            src.erase(std::remove(src.begin(), src.end(), i), src.end());
            groups[to].push_back(i);
            group_of[i] = to;
        }
        for (auto& g : groups) std::sort(g.begin(), g.end());
    }

    // Emit: groups in opener order (groups vector order), members ascending.
    size_t k = 0;
    for (const auto& g : groups) {
        for (size_t i : g) result.order[k++] = i;
    }
    result.reordered = false;
    for (size_t i = 0; i < n; ++i) {
        if (result.order[i] != i) {
            result.reordered = true;
            break;
        }
    }
    return result;
}

} // namespace openrar::compress
