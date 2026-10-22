#!/usr/bin/env python3
"""ctest LastTest.log -> single JUnit XML (v1.44 Verification Substrate, M0).

The CMake floor is 3.16, so `ctest --output-junit` (3.21+) is unavailable; this
aggregator converts the VERBOSE LastTest.log written by every ctest run
(suite-level granularity: one <testcase> per suite binary). Parse target:
the typed blocks ctest emits —

    Start testing: <date>
    27/47 Testing: cli_tests
    27/47 Test: cli_tests
    Command: "..."
    "cli_tests" start time: Oct 18 08:15 GMT Summer Time
    Output:
    ...test stdout/stderr...
    Test Passed.                       (or: Test Failed. / Test Timed Out.)
    "cli_tests" end time: Oct 18 08:16 GMT Summer Time
    End testing: <date>

Fail-closed by design (G-9): an empty log, a declared count that the parsed
blocks fail to meet, or a garbled start/end line refuses instead of emitting
an empty-but-green XML. A test that aborted maps to failure regardless of its
"Test Passed." print. Suite times are minute-precision (ctest verbose stamps
carry no seconds) or omitted.

Usage: python tools/ctest_to_junit.py --log <Testing/Temporary/LastTest.log>
                 --out <junit.xml> [--min-tests N]
Exit codes: 0 all suites passed; 1 any fail-closed condition or failing suite.
"""
import argparse
import datetime as _dt
import re
import sys
from xml.sax.saxutils import escape


class FailClosed(RuntimeError):
    pass


HEADER_RE = re.compile(r"^\s*(\d+)/(\d+)\s+Testing:\s+(.+)$")
ORDINAL_RE = re.compile(r"^\s*(\d+)/(\d+)\s+Test:\s+(.+)$")
START_RE = re.compile(r'^"([^"]+)" start time:\s*(.+)$')
END_RE = re.compile(r'^"([^"]+)" end time:\s*(.+)$')
STATUS_RE = re.compile(r"^Test (Passed|Failed|Timed Out)\.$")
ABORT_RE = re.compile(r"(Subprocess aborted|^\*\*\*Exception|^\*\*\*Failed|The test failed)")


def _parse_stamp(text):
    # "Oct 18 08:15 GMT Summer Time" -> naive datetime (minute precision).
    # ctest stamps carry no year; explicit construction avoids the
    # yearless-strptime ambiguity deprecation.
    m = re.match(r"([A-Za-z]{3})\s+(\d{1,2})\s+(\d{2}):(\d{2})", text.strip())
    if not m:
        return None
    months = {"Jan": 1, "Feb": 2, "Mar": 3, "Apr": 4, "May": 5, "Jun": 6,
              "Jul": 7, "Aug": 8, "Sep": 9, "Oct": 10, "Nov": 11, "Dec": 12}
    mon = months.get(m.group(1))
    if mon is None:
        return None
    try:
        return _dt.datetime(1900, mon, int(m.group(2)),
                            int(m.group(3)), int(m.group(4)))
    except ValueError:
        return None


class _Block:
    __slots__ = ("num", "total", "name", "status", "start", "end", "output_flag", "abort_seen")

    def __init__(self, num, total, name):
        self.num, self.total, self.name = num, total, name
        self.status = "pending"   # pending -> Passed/Failed/Timed Out/aborted
        self.start = None
        self.end = None
        self.output_flag = False
        self.abort_seen = False


def convert(log_text, min_tests=1):
    """Parses log_text. Returns (xml_body, testcase_count, failure_count).
    Raises FailClosed on any unparseable/garbled/over-constrained input."""
    if not log_text.strip():
        raise FailClosed("empty LastTest.log")

    blocks = {}          # name -> _Block (latest ordinal wins)
    order = []           # first-appearance order of names
    cur = None
    expected_total = None
    end_testing_seen = False

    for lineno, raw in enumerate(log_text.splitlines(), 1):
        line = raw.rstrip("\r\n")
        if line.startswith("End testing:"):
            end_testing_seen = True

        m = HEADER_RE.match(line) or ORDINAL_RE.match(line)
        if m:
            num, total, name = int(m.group(1)), int(m.group(2)), m.group(3).strip()
            if expected_total is None:
                expected_total = total
            elif expected_total != total:
                raise FailClosed(
                    f"line {lineno}: declared total {total} contradicts "
                    f"{expected_total}")
            if name in blocks:
                prev = blocks[name]
                if num != prev.num or total != prev.total:
                    raise FailClosed(
                        f"line {lineno}: contradictory ordinals for '{name}'")
                cur = blocks[name]  # repeated header for an open block
            else:
                cur = _Block(num, total, name)
                blocks[name] = cur
                order.append(name)
                continue

        if cur is not None:
            sm = START_RE.match(line)
            if sm:
                if sm.group(1) != cur.name:
                    raise FailClosed(
                        f"line {lineno}: start stamp '{sm.group(1)}' does not "
                        f"match open block '{cur.name}'")
                cur.start = _parse_stamp(sm.group(2))
                if cur.start is None:
                    raise FailClosed(
                        f"line {lineno}: unparsable start time: {line[:120]!r}")
                cur.output_flag = True
                continue
            em = END_RE.match(line)
            if em:
                if em.group(1) != cur.name:
                    raise FailClosed(
                        f"line {lineno}: end stamp '{em.group(1)}' does not "
                        f"match open block '{cur.name}'")
                cur.end = _parse_stamp(em.group(2))
                if cur.end is None:
                    raise FailClosed(
                        f"line {lineno}: unparsable end time: {line[:120]!r}")
                cur = None
                continue

        sm = STATUS_RE.match(line)
        if sm and cur is not None:
            cur.status = sm.group(1)
            continue
        if cur is not None and (ABORT_RE.search(line) or " exited or not launched" in line):
                cur.abort_seen = True
                continue
        # Block output and any other noise: skip (a truncated/garbled log is
        # caught by the count check below).
        continue

    if expected_total is None:
        raise FailClosed("no 'N/M Testing:' blocks found")
    if not end_testing_seen:
        raise FailClosed("log truncated: no 'End testing:' footer")
    rows = [blocks[name] for name in order]
    # 'N/M' ordinals span the full ctest matrix even under -R filtering, so
    # the declared total bounds but does not equal this run's row count.
    if len(rows) > expected_total:
        raise FailClosed(
            f"parsed {len(rows)} test blocks but log declares {expected_total} "
            f"(corrupted log)")
    if len(rows) < min_tests:
        raise FailClosed(
            f"{len(rows)} tests below the required floor {min_tests} (G-9)")

    failures = 0
    tc = []
    for b in rows:
        keyword = ("Subprocess aborted" if b.abort_seen
                    else (b.status if b.status != "pending" else "Not Run"))
        if keyword == "Passed":
            fail_msg = None
        elif keyword == "Not Run":
            fail_msg = "Status: Not Run (G-9)"
        else:
            fail_msg = f"Status: {keyword}"
        is_pass = fail_msg is None
        if not is_pass:
            failures += 1
        t = ""
        if b.start is not None and b.end is not None:
            delta = (b.end - b.start).total_seconds()
            if delta < 0:
                delta += 86400.0  # minute-precision stamps, midnight wrap
            t = f' time="{delta:.3f}"'
        if is_pass:
            entry = f'    <testcase classname="ctest" name="{escape(b.name)}"{t}/>'
        else:
            entry = (f'    <testcase classname="ctest" name="{escape(b.name)}"{t}>\n'
                     f'      <failure message="{escape(fail_msg)}"/>\n'
                     f'    </testcase>')
        tc.append(entry)

    body = '<testsuite name="ctest">\n' + "\n".join(tc) + "\n</testsuite>"
    return body, len(rows), failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-tests", type=int, default=1)
    args = ap.parse_args()

    try:
        with open(args.log, "r", encoding="utf-8", errors="strict") as f:
            log_text = f.read()
    except (OSError, UnicodeDecodeError) as exc:
        print(f"aggregator: cannot read log '{args.log}': {exc}", file=sys.stderr)
        return 1

    try:
        body, count, failures = convert(log_text, args.min_tests)
    except FailClosed as exc:
        print(f"aggregator: fail-closed refusal: {exc}", file=sys.stderr)
        return 1

    xml = '<?xml version="1.0" encoding="UTF-8"?>\n' + body + "\n"
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(xml)
    print(f"aggregator: wrote {args.out} ({count} suites, {failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
