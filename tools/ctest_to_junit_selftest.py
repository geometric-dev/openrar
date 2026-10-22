#!/usr/bin/env python3
"""In-process negative tests for tools/ctest_to_junit.py (v1.44 M0).

The fail-closed contract: empty, garbled, truncated, and total-mismatched
logs must all raise FailClosed (exit 1) — never produce an empty-but-green
XML. A valid minimal log must convert cleanly with the right counts.

Run: python tools/ctest_to_junit_selftest.py
Exit 0 = all negative contracts hold; any assertion failure exits non-zero
and the aggregator ctest entry fails.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ctest_to_junit as conv


def expect_fail(name, text, min_tests=1):
    try:
        conv.convert(text, min_tests)
    except conv.FailClosed as exc:
        print(f"[PASS] {name}: refused ({str(exc)[:80]})")
        return
    raise AssertionError(f"{name}: expected FailClosed, got a convert with success")


def expect_ok(name, text, min_tests=1, want_tests=None, want_failures=None):
    body, count, failures = conv.convert(text, min_tests)
    assert body.startswith('<testsuite name="ctest">'), name
    assert body.rstrip().endswith("</testsuite>"), name
    if want_tests is not None:
        assert count == want_tests, f"{name}: {count} != {want_tests}"
    if want_failures is not None:
        assert failures == want_failures, f"{name}: {failures} != {want_failures}"
    print(f"[PASS] {name}: converted ({count} suites, {failures} failures)")
    return body


VALID = (
    "Start testing: Oct 18 08:15 GMT Summer Time\n"
    "----------------------------------------------------------\n"
    "27/47 Testing: cli_tests\n"
    "27/47 Test: cli_tests\n"
    'Command: "clang/openrar"\n'
    'Directory: /tmp/build\n'
    '"cli_tests" start time: Oct 18 08:15 GMT Summer Time\n'
    "Output:\n"
    "----------------------------------------------------------\n"
    "[PASS] CLI help\n"
    "[PASS] CLI --version parity\n"
    "Test Passed.\n"
    '"cli_tests" end time: Oct 18 08:16 GMT Summer Time\n'
    "End testing: Oct 18 08:16 GMT Summer Time\n"
)

FAILED = (
    "Start testing: Oct 18 08:15 GMT Summer Time\n"
    "27/47 Testing: cli_tests\n"
    "27/47 Test: cli_tests\n"
    '"cli_tests" start time: Oct 18 09:15 GMT Summer Time\n'
    "Output:\n"
    "some body junk Test infinite: junk .. Passed 1.0 sec\n"
    "Test Failed.\n"
    '"cli_tests" end time: Oct 18 09:26 GMT Summer Time\n'
    "  2% tests passed, 98% failed out of 1\n"
    "End testing: Oct 18 09:26 GMT Summer Time\n"
)

TIMED_OUT = (
    "Start testing: Oct 18 08:15 GMT Summer Time\n"
    "27/47 Testing: runner_tests\n"
    "27/47 Test: runner_tests\n"
    '"runner_tests" start time: Oct 18 08:15 GMT Summer Time\n'
    "Output:\n"
    "Test Timed Out.\n"
    '"runner_tests" end time: Oct 18 08:18 GMT Summer Time\n'
    "End testing: Oct 18 08:18 GMT Summer Time\n"
)

ABORTED = (
    "Start testing: Oct 18 08:15 GMT Summer Time\n"
    "27/47 Testing: mutation_tests\n"
    "27/47 Test: mutation_tests\n"
    '"mutation_tests" start time: Oct 18 08:15 GMT Summer Time\n'
    "Output:\n"
    "body says: Subprocess aborted yes\n"
    "Test Failed.\n"
    '"mutation_tests" end time: Oct 18 08:16 GMT Summer Time\n'
    "End testing: Oct 18 08:16 GMT Summer Time\n"
)

NOT_RUN = (
    "Start testing: Oct 18 08:15 GMT Summer Time\n"
    "27/47 Testing: phased_out_tests\n"
    "27/47 Test: phased_out_tests\n"
    "\n"
    "0% tests passed, 1 tests failed out of 1\n"
    "End testing: Oct 18 08:15 GMT Summer Time\n"
)


def main():
    # 1. valid single-suite log converts with the right counts.
    body = expect_ok("valid_log", VALID, want_tests=1, want_failures=0)
    assert ' name="cli_tests"' in body
    assert 'time="60.000"' in body      # minute-precision stamps, 08:15 -> 08:16
    assert "<failure" not in body

    # 2. failed suite maps to failure and counts.
    body = expect_ok("failed_suite", FAILED, want_tests=1, want_failures=1)
    assert "<failure" in body and "Failed" in body

    # 3. timeout is a failure with its own keyword, minutes precision.
    body = expect_ok("timeout_suite", TIMED_OUT, want_tests=1, want_failures=1)
    assert "Timed Out" in body and "<failure" in body

    # 4. aborted suite upgrades to "Subprocess aborted" (body text).
    body = expect_ok("aborted_suite", ABORTED, want_tests=1, want_failures=1)
    assert "Subprocess aborted" in body

    # 5. block without any status line is Not Run — failure, not pass evidence.
    body = expect_ok("not_run_suite", NOT_RUN, want_tests=1, want_failures=1)
    assert "Not Run" in body

    # 6..10 fail-closed refusals.
    expect_fail("empty_log", "")
    expect_fail("whitespace_log", "   \n\n  \n")
    expect_fail("garbled_progress_line",
                VALID.replace("Testing:", "TesTing?").replace("Test:", "TesT:"))
    expect_fail("truncated_log", "Start testing: ...")
    expect_fail("missing_end_testing", VALID.replace("End testing:", "Nothing:"))
    expect_fail("total_mismatch",
                "1/3 Testing: a\n"
                '1/2 Testing: b\n')
    expect_fail("below_floor", VALID, min_tests=2)
    expect_fail("rows_exceed_declared",
                "Start testing: ...\n"
                "1/2 Testing: a\n"
                "2/2 Testing: b\n"
                "3/2 Testing: c\n"
                "End testing: ...\n")

    # 11. output-centric noise between tests does not disturb the count.
    rich = (
        "Start testing: ...\n"
        "1/1 Testing: compress_tests\n"
        '"compress_tests" start time: Oct 18 08:15 GMT Summer Time\n'
        "Output:\n"
        "[RUNNER] case=\"x\" result=pass time=0\n"
        "[PASS] some body line\n"
        "Test Passed.\n"
        '"compress_tests" end time: Oct 18 08:15 GMT Summer Time\n'
        "End testing: Oct 18 08:15 GMT Summer Time\n"
    )
    expect_ok("output_block_skipped", rich, want_tests=1, want_failures=0)

    # 12. garbled start stamp (unparsable time) refuses loudly.
    bad_stamp = (
        "Start testing: ...\n"
        "1/1 Testing: compress_tests\n"
        '"compress_tests" start time: GARBAGE\n'
    )
    expect_fail("bad_start_stamp", bad_stamp)

    # 13. minute-precision delta computed correctly (08:15 -> 08:26 = 660).
    body = expect_ok("minute_delta", FAILED)
    assert 'time="660.000"' in body
    print("[PASS] aggregator selftest complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
