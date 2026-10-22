#!/usr/bin/env python3
"""RAT-B differential harness (v1.44 Verification Substrate, M1).

    python tools/rat_b_diff.py <pre_dir> <post_dir> [--suite=a,b,...]

Standing rule RAT-B (roadmap §7.2): doctest preserves the verified assert
set. For every suite (all tests/unit/*.cpp by default, --suite to restrict)
this tool compares a PRE binary tree against a POST binary tree and proves,
per suite, that a change changed nothing observable:

  (i)   --list-test-cases: the ordered case-name sequence must be identical
        (source-order execution is part of the contract, so order too).
        Any unmapped delta = hard fail; the first divergent case is reported.
  (ii)  full runs of both binaries: per-case outcome vectors
        ([RUNNER] case="..." result=...) identical in names, order, results;
        wall times excluded (non-deterministic); [RUNNER] summary counts must
        reconcile with the observed vector.
  (iii) a per-suite OK/FAIL table is emitted.

Fail-closed (G-9): a missing executable in either tree, a garbled listing
(no [RUNNER] header, count mismatch), a run without runner dispatch (e.g.
the [SKIP] pre-hook path), a summary/vector mismatch, or fewer than
--min-suites compared suites — all hard failures. The identity run
(pre = post = same build) must be green at M1 close; thereafter the harness
runs per-file locally with the parent-commit pre-binary, and one full-tree
run at tag pre-flight. Never CI-per-file.

Environment/config needs:
  * archive_api_tests / buffer_archive_tests require -DOPENRAR_INMEM_ARCHIVE=ON:
    build them in a second config dir and run this tool against it with
    --suite=archive_api_tests,buffer_archive_tests.
  * sfx_e2e_tests requires OPENRAR_SFX_STUB=... in the environment; child
    binaries inherit this process's environment.

Exit codes: 0 = all compared suites identical, 1 = any divergence.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
REPO = Path(__file__).resolve().parent.parent
UNIT_DIR = REPO / "tests" / "unit"

LIST_HEAD_RE = re.compile(r'^\[RUNNER\] suite="([^"]+)" cases=(\d+)', re.M)
CASE_RE = re.compile(r'^\[RUNNER\] case="([^"]+)" result=(\w+) time=', re.M)
SUMMARY_RE = re.compile(
    r'^\[RUNNER\] summary suite="([^"]+)" cases=(\d+) selected=(\d+) '
    r'pass=(\d+) fail=(\d+) expected_fail=(\d+) unexpected_pass=(\d+)', re.M)


def strip_ansi(data):
    return ANSI_RE.sub("", data)


def suite_roster():
    return sorted(p.stem for p in UNIT_DIR.glob("*.cpp"))


def exe_path(directory, stem):
    # Resolved ABSOLUTE path: suites that re-exec themselves (crash-hook hop,
    # runner crash demo) build cmd strings from argv[0]; a relative argv0
    # resolves against the child's cwd and breaks the re-exec. ctest always
    # passes the resolved location too, so this matches production context.
    names = (stem + ".exe", stem) if os.name == "nt" else (stem, stem + ".exe")
    for name in names:
        cand = directory / name
        if cand.is_file():
            return cand.resolve()
    return None


def child_run(exe, cwd, timeout, extra_args=()):
    try:
        proc = subprocess.run([str(exe), *extra_args], cwd=str(cwd), capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              env=os.environ.copy(), timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, None, "timeout after %ss" % timeout, True
    except OSError as exc:
        return None, None, "spawn failed: %s" % exc, True
    return strip_ansi(proc.stdout), proc.returncode, strip_ansi(proc.stderr), False


def parse_listing(text):
    head = LIST_HEAD_RE.search(text)
    if not head:
        return None, 'no "[RUNNER] suite=" header in listing'
    declared = int(head.group(2))
    names = []
    for line in text.splitlines():
        line = line.strip()
        if line == head.group(0) or not line:
            continue
        if re.fullmatch(r"[A-Za-z0-9_.-]+", line):
            names.append(line)
        else:
            return None, "unexpected listing line: %.80s" % line
    if len(names) != declared:
        return None, "count mismatch: declared=%d parsed=%d" % (declared, len(names))
    if len(set(names)) != len(names):
        dups = sorted(n for n in set(names) if names.count(n) > 1)
        return None, "duplicate names in listing: %s" % ",".join(dups[:4])
    return names, None


def parse_run(text):
    cases = [(m.group(1), m.group(2)) for m in CASE_RE.finditer(text)]
    summary = SUMMARY_RE.search(text)
    if not summary:
        hint = "suite skipped without runner dispatch ([SKIP] path)" if "[SKIP]" in text \
            else "no [RUNNER] summary line"
        return None, hint
    counts = {"pass": int(summary.group(4)), "fail": int(summary.group(5)),
              "expected_fail": int(summary.group(6)),
              "unexpected_pass": int(summary.group(7))}
    observed = {}
    for _, result in cases:
        observed[result] = observed.get(result, 0) + 1
    if any(counts[k] != sum(1 for _, r in cases if r == k) for k in counts):
        return None, "summary counts %s != observed vector %s" % (counts, observed)
    total, selected = int(summary.group(2)), int(summary.group(3))
    if total != len(cases) or selected != len(cases):
        return None, ("summary cases=%d selected=%d but %d case lines emitted"
                      % (total, selected, len(cases)))
    names = [n for n, _ in cases]
    if len(set(names)) != len(names):
        dups = sorted(n for n in set(names) if names.count(n) > 1)
        return None, "duplicate case entries in outcome vector: %s" % ",".join(dups[:4])
    return (summary.group(1), cases), None


def first_divergence(pre, post):
    if pre == post:
        return None
    for i, (a, b) in enumerate(zip(pre, post)):
        if a != b:
            return "index %d: pre='%s' post='%s'" % (i, a, b)
    if len(pre) < len(post):
        return "extra post case, first: '%s'" % post[len(pre)]
    return "extra pre case, first: '%s'" % pre[len(post)]


def run_suite_pair(stem, pre_dir, post_dir, timeout, skip_run):
    detail, exes = [], {}
    for tag, directory in (("pre", pre_dir), ("post", post_dir)):
        exe = exe_path(directory, stem)
        if exe is None:
            return False, "missing executable in %s tree" % tag, detail
        exes[tag] = exe

    listings = {}
    for tag, exe in exes.items():
        out, rc, err, fatal = child_run(exe, exe.parent, timeout, ("--list-test-cases",))
        if fatal:
            return False, "listing: %s" % err, detail
        names, parse_err = parse_listing(out)
        if parse_err:
            return False, "listing garbled (rc=%d): %s" % (rc, parse_err), detail
        detail.append("%s_list=%d" % (tag, len(names)))
        if rc != 0:
            return False, "listing rc=%d (err: %.120s)" % (rc, err), detail
        listings[tag] = names

    div = first_divergence(listings["pre"], listings["post"])
    if div:
        return False, "listing divergence: " + div, detail

    if skip_run:
        return True, "cases=%d (list-only)" % len(listings["pre"]), detail

    vectors, rcs = {}, {}
    for tag, exe in exes.items():
        out, rc, err, fatal = child_run(exe, exe.parent, timeout)
        if fatal:
            return False, "run: %s" % err, detail
        parsed, parse_err = parse_run(out)
        if parse_err:
            return False, "run (rc=%d): %s" % (rc, parse_err), detail
        vectors[tag], rcs[tag] = parsed, rc
        detail.append("%s_run=%d@rc=%d" % (tag, len(parsed[1]), rc))

    pre_suite, pre_vec = vectors["pre"]
    post_suite, post_vec = vectors["post"]
    if pre_suite != post_suite:
        return False, "suite-name divergence: %s vs %s" % (pre_suite, post_suite), detail
    name_div = first_divergence([n for n, _ in pre_vec], [n for n, _ in post_vec])
    res_div = first_divergence([r for _, r in pre_vec], [r for _, r in post_vec])
    if name_div or res_div:
        return False, "outcome divergence: %s %s" % (name_div or "", res_div or ""), detail

    status = "cases=%d identical" % len(pre_vec)
    non_green = []
    if rcs["pre"] != 0:
        non_green.append("pre_rc=%d" % rcs["pre"])
    if rcs["post"] != 0:
        non_green.append("post_rc=%d" % rcs["post"])
    if non_green:
        status += " (same outcome, nonzero rc: %s)" % " ".join(non_green)
    return True, status, detail


def main(argv=None):
    ap = argparse.ArgumentParser(description="RAT-B differential harness (pre vs post)")
    ap.add_argument("pre_dir", type=Path)
    ap.add_argument("post_dir", type=Path)
    ap.add_argument("--suite", default="", help="comma list to restrict roster")
    ap.add_argument("--skip-run", action="store_true", help="listing comparison only")
    ap.add_argument("--min-suites", type=int, default=1, help="floor (G-9 no vacuous pass)")
    ap.add_argument("--timeout", type=int, default=900, help="per-child exec timeout s")
    args = ap.parse_args(argv)

    if not args.pre_dir.is_dir() or not args.post_dir.is_dir():
        print("usage error: both dirs must exist", file=sys.stderr)
        return 1
    roster = [s.strip() for s in args.suite.split(",") if s.strip()] if args.suite \
        else suite_roster()
    if not roster:
        print("roster empty", file=sys.stderr)
        return 1

    rows, failed = [], []
    for stem in roster:
        ok, status, detail = run_suite_pair(stem, args.pre_dir, args.post_dir,
                                            args.timeout, args.skip_run)
        rows.append((stem, ok, status, detail))
        if not ok:
            failed.append((stem, status))

    print("%-30s %-5s %s" % ("suite", "verdict", "detail"))
    for stem, ok, status, _ in rows:
        print("%-30s %-5s %s" % (stem, "OK" if ok else "FAIL", status[:120]))
    print("---")
    compared, bad = len(rows), len(failed)
    green = bad == 0 and compared >= args.min_suites
    print("RAT-B %s: suites=%d ok=%d fail=%d (pre=%s post=%s)"
          % ("GREEN" if green else "RED", compared, compared - bad, bad,
             args.pre_dir, args.post_dir))
    if compared < args.min_suites:
        print("FAIL: %d suites compared < --min-suites %d (vacuous run)"
              % (compared, args.min_suites), file=sys.stderr)
        return 1
    for stem, status in failed:
        print("FAIL %s: %s" % (stem, status[:200]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
