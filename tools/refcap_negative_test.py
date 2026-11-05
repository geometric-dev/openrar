#!/usr/bin/env python3
"""
tools/refcap_negative_test.py - fail-closed negative test for OPENRAR_REFCAP.

The golden-hash gates in tests/unit/compress_tests.cpp let a maintainer
re-capture reference digests with OPENRAR_REFCAP=1. That switch must never be
able to produce a passing run: with it set, each gate has to print its FATAL
line and exit with status 2 (Directive 5, NT-12). A plain "WILL_FAIL" CTest
property cannot express that, because any crash would also "pass", so this
script asserts the exact exit status and message, and first runs the same case
WITHOUT the switch as a control so the test cannot pass just because the case
is broken.

Usage: refcap_negative_test.py <path to compress_tests executable>
Exit status: 0 when every check holds, 1 otherwise.
"""

import os
import subprocess
import sys

GATE_CASES = (
    "test_slot257_pure_addition_suppressed",
    "test_selection_reference",
)
FATAL_TEXT = "OPENRAR_REFCAP is active"
TIMEOUT_S = 600


def run_case(exe, case, refcap):
    env = dict(os.environ)
    env.pop("OPENRAR_REFCAP", None)
    if refcap:
        env["OPENRAR_REFCAP"] = "1"
    return subprocess.run(
        [exe, "--test-case=" + case],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=TIMEOUT_S,
    )


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    exe = sys.argv[1]
    if not os.path.isfile(exe):
        print("[FAIL] test executable not found: %s" % exe)
        return 1

    failures = 0
    for case in GATE_CASES:
        control = run_case(exe, case, refcap=False)
        if control.returncode != 0:
            print("[FAIL] control run of %s exited %d (expected 0)" % (case, control.returncode))
            failures += 1
            continue

        capped = run_case(exe, case, refcap=True)
        if capped.returncode != 2:
            print(
                "[FAIL] %s with OPENRAR_REFCAP=1 exited %d (expected exactly 2)"
                % (case, capped.returncode)
            )
            failures += 1
        elif FATAL_TEXT not in capped.stderr:
            print("[FAIL] %s exited 2 but did not print the FATAL recapture message" % case)
            failures += 1
        else:
            print("[PASS] %s: control exits 0, OPENRAR_REFCAP=1 exits 2 with FATAL" % case)

    if failures:
        print("[FAIL] refcap fail-closed: %d check(s) failed" % failures)
        return 1
    print("[PASS] refcap fail-closed: recapture can never produce a passing run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
