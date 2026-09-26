// M2b (v1.26 plan §1.3): affinity planner — grouping semantics, tie-break
// determinism, threshold math, degenerate inputs, reference fixup, and the
// fingerprint-cap environment override.

#include "../../src/compress/cdc_planner.hpp"

#include <cassert>
#include <iostream>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

compress::CdcPlanResult plan(const std::vector<std::vector<core::uint64>>& hashes,
                             const std::vector<size_t>& reference_of = {}) {
    return compress::plan_cdc_order(hashes, reference_of);
}

bool is_identity(const std::vector<size_t>& order) {
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] != i) return false;
    }
    return true;
}

void set_cap_env(const char* value) {
#ifdef _WIN32
    _putenv_s("OPENRAR_CDC_INDEX_CAP", value);
#else
    setenv("OPENRAR_CDC_INDEX_CAP", value, 1);
#endif
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif

    // A file joins the group of its best earlier partner (>= 30% of the
    // smaller file's chunks shared); the emitted order concatenates groups,
    // so the joiner is pulled forward next to its partner: [A, B, A'] ->
    // [A, A', B].
    {
        auto r = plan({{1, 2, 3, 4}, {10, 11, 12, 13}, {1, 2, 3, 99}});
        assert(r.reordered);
        assert((r.order == std::vector<size_t>{0, 2, 1}));
        std::cout << "[PASS] affinity grouping pulls the joiner forward\n";
    }

    // Equal shared counts tie-break to the SMALLEST partner index: file 2
    // shares 2 chunks with file 0 and 2 with file 1 -> joins file 0's group.
    {
        auto r = plan({{1, 2, 3, 4}, {5, 6, 7, 8}, {1, 2, 5, 6}});
        assert(r.reordered);
        assert((r.order == std::vector<size_t>{0, 2, 1}));
        std::cout << "[PASS] tie-break: smallest partner index wins\n";
    }

    // Threshold math (integer permille): 3 shared of the smaller file's 3
    // chunks is exactly 300 permille -> join. A third, unrelated file sits
    // between them so the join is observable as a reorder.
    {
        const std::vector<core::uint64> ten = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        auto at_threshold = plan({ten, {51, 52, 53, 54}, {1, 2, 3}});
        assert(at_threshold.reordered); // 300 permille of 3 == 300 -> join
        assert((at_threshold.order == std::vector<size_t>{0, 2, 1}));
        std::cout << "[PASS] threshold boundary accepted at exactly 30%\n";
    }
    {
        // smaller file has 10 chunks, 2 shared -> 200 permille < 300: no edge.
        const std::vector<core::uint64> big = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const std::vector<core::uint64> small = {1, 2, 21, 22, 23, 24, 25, 26, 27, 28};
        auto r = plan({big, small, {31, 32, 33, 34, 35, 36, 37, 38, 39, 40}});
        assert(!r.reordered);
        std::cout << "[PASS] below-threshold similarity leaves original order\n";
    }

    // Degenerate inputs (plan directive 7): empty hash lists never match —
    // original order, no crash, no affinity edges.
    {
        auto r = plan({{}, {1, 2, 3}, {}});
        assert(!r.reordered);
        assert(is_identity(r.order));
        auto empty_batch = plan({});
        assert(empty_batch.order.empty());
        std::cout << "[PASS] degenerate inputs stay in original order\n";
    }

    // Reference fixup: a reference with NO usable fingerprints (cap-fallback
    // simulation) still joins its master's group, and the master always
    // precedes it in the emitted order.
    {
        auto r = plan({{1, 2, 3, 4}, {9, 9, 9, 9}, {}}, {SIZE_MAX, SIZE_MAX, 0});
        assert(r.reordered);
        assert((r.order == std::vector<size_t>{0, 2, 1}));
        const auto pos = [&](size_t file) {
            for (size_t i = 0; i < r.order.size(); ++i) {
                if (r.order[i] == file) return i;
            }
            return r.order.size();
        };
        assert(pos(0) < pos(2)); // master before its reference
        std::cout << "[PASS] reference fixup keeps references behind masters\n";
    }

    // Determinism: the same input plans identically, twice.
    {
        const std::vector<std::vector<core::uint64>> hashes = {
            {1, 2, 3, 4}, {10, 11, 12, 13}, {1, 2, 3, 99}, {1, 2, 4, 100}, {77, 78}};
        auto a = plan(hashes);
        auto b = plan(hashes);
        assert(a.order == b.order && a.reordered == b.reordered);
        std::cout << "[PASS] planning is deterministic\n";
    }

    // Fingerprint cap: OPENRAR_CDC_INDEX_CAP may lower the default, never
    // raise it; garbage and zero fall back to the default.
    {
        set_cap_env("");
        assert(compress::cdc_fingerprint_cap() == 2000000);
        set_cap_env("5");
        assert(compress::cdc_fingerprint_cap() == 5);
        set_cap_env("99999999999999");
        assert(compress::cdc_fingerprint_cap() == 2000000);
        set_cap_env("0");
        assert(compress::cdc_fingerprint_cap() == 2000000);
        set_cap_env("abc");
        assert(compress::cdc_fingerprint_cap() == 2000000);
        set_cap_env("");
        std::cout << "[PASS] cdc_fingerprint_cap env override bounds\n";
    }

    std::cout << "All cdc_planner_tests passed.\n";
    return 0;
}
