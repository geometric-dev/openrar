#ifndef OPENRAR_TESTS_TEST_RUNNER_HPP
#define OPENRAR_TESTS_TEST_RUNNER_HPP

// In-repo unit-test runner (v1.44 Verification Substrate, M0).
//
// Every tests/unit/*.cpp suite wraps its existing static test_*() functions
// in a static Case table and delegates main() to openrar_test_main(). The
// runner owns:
//   * CLI: doctest-compatible flag names (--test-case= / -tc=,
//     --list-test-cases / -ltc, --out= / --junit=) so the RAT-B differential
//     harness treats pre-/post-migration binaries identically, and the M4/M5
//     doctest swap changes no CLI consumer.
//   * Per-case timing and status: [RUN] <name> printed + flushed BEFORE each
//     case starts, [RUNNER] case="<name>" result=... after it completes. A
//     crash mid-case therefore leaves the previous cases' statuses visible
//     on stdout (piped stdio is fully buffered; flushes matter).
//   * M5 doctest bridge: at depth 1 the runner registers each selected case
//     with the vendored doctest (tests/unit suites assert via REQUIRE after
//     the M5 assert->REQUIRE swap) and installs a doctest reporter that
//     translates doctest's events back into the [RUN]/[RUNNER]/JUnit
//     protocol below; the returned doctest rc maps to 0/1 only. A doctest
//     is_crash (SEH-translated process failure, e.g. the harness's own
//     deliberate deprivation) is re-aborted so the observable is the
//     pre-doctest one: process death, partial statuses flushed, JUnit temp
//     orphaned. Nested dispatches (depth > 1) keep the legacy direct-fn
//     loop (doctest must stay at the outermost dispatch).
//   * JUnit XML (--out=): streamed to a temp sibling, footer written only on
//     clean completion, then atomically renamed to the real path. An abort
//     mid-suite leaves the temp orphaned and the real path never written
//     (fail-closed; see plan §9 commit-protocol row).
//   * G-9 no-vacuous-pass: a filter that matches nothing exits non-zero, a
//     JUnit writer failure makes the final exit non-zero, and registration
//     errors (non-names / duplicates) are rejected before any case runs.
//   * Exit codes are 0 or 1 only; abort statuses pass through untouched.
//
// Case-name grammar: [A-Za-z0-9_.-]+, enforced at dispatch — the RAT-B
// harness and JUnit rely on stable ASCII names.
//
// Suites whose argv has file-specific meaning (sfx child hops, the
// extraction crash-hook child) parse THEIR reserved flags in their own
// prehook first; openrar_test_main is only reached once those are settled.
// The runner hard-errors only on flags in its own namespace (--*).

#include <cstddef>
#include <cstring>

// Portable CRT assert routing (commented below)
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace openrar::test {

// True when argv only wants runner meta-services (listing/help) rather than
// a run, so pre-dispatch suite banners can stay off listing output.
inline bool is_list_only(int argc, char* const argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        if (std::strcmp(s, "--list-test-cases") == 0 || std::strcmp(s, "-ltc") == 0 ||
            std::strcmp(s, "--runner-help") == 0)
            return true;
    }
    return false;
}

// Mirror of tests/unit/test_support.hpp's OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR:
// under ctest the MSVC CRT defaults pop modal dialogs on assert/abort and
// hang the run; redirect to stderr and disable the WER report-fault dialog.
// Lives in the runner header so wrapped suites need no test_support dependency.
#if defined(_MSC_VER)
inline void route_crt_asserts_to_stderr() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}
#else
inline void route_crt_asserts_to_stderr() {}
#endif

struct Case {
    const char* name;
    void (*fn)();
    // FMM-44: an expected-fail case's failure is recorded as expected_fail
    // and does not affect the exit code (ctest WILL_FAIL expressibility is
    // preserved from M0; full wiring lands with the suite that needs it).
    bool expect_fail;
};

#define OPENRAR_CASE(fn)                                                                           \
    { #fn, fn, false }
#define OPENRAR_CASE_EXPECT(fn)                                                                    \
    { #fn, fn, true }

// Case-name grammar predicate: ASCII identifier subset, non-empty.
bool name_ok(const char* name);

// First duplicate found, or nullptr when all names are unique and valid.
const Case* find_duplicate(const Case* cases, std::size_t count);

// doctest-style wildcard match: '*' matches any run of characters;
// everything else is an exact character. Empty pattern matches the empty
// name only.
bool glob_match(const char* pattern, const char* name);

// Full dispatch cycle: arg parsing, validation, list/filter/selection,
// per-case run with timing, optional JUnit output. Returns the process
// exit code (0 = ok, 1 = failure/usage/G-9 trigger). May NOT be called
// concurrently with itself; nested calls are marked [NESTED] in their
// console output so the M1 harness can skip them.
int run(const Case* cases, std::size_t count, int argc, char** argv);

// True when the most recent dispatch used a --test-case filter (suites use
// this to suppress their "all passed" banner under a filtered run).
bool last_run_was_filtered();

// Validate (grammar + duplicates) then run. Registration errors print to
// stderr and return 1 before any case executes.
int openrar_test_main(const Case* cases, std::size_t count, int argc, char** argv);

} // namespace openrar::test

#endif // OPENRAR_TESTS_TEST_RUNNER_HPP
