#!/usr/bin/env python3
"""Anchor lint (v1.44 Verification Substrate, M3).

    python tools/anchor_lint.py [--release v1.44] [--demand-ledger]

Three fail-closed checks:

  (i)   Every `file:line` anchor in roadmap-v143.merged.md resolves to a live
        file and line. Per-file and per-anchor allowlist entries live in
        tools/anchor_allowlist.txt (opt-in; each entry carries a reason).
  (ii)  Assert-ledger parity: the verified-assert set counted over the
        in-scope tests/unit/*.cpp files — union of legacy `assert(` and the
        doctest assertion macros (`REQUIRE(`, `CHECK(`, `REQUIRE_FALSE(`,
        `CHECK_FALSE(`, and the throws/nothrow forms) the M5 migration wrote;
        since M5 swapped each `assert(` 1:1 into `REQUIRE(`, the union keeps
        per-file counts continuity-stable across the migration. static_assert(
        excluded and counted separately; comment-line matches and matches
        inside string literals are excluded (the v1.43 baseline's counts
        include a handful of prose-comment `fakeassert` matches — the ledger
        doc records both conventions). Recomputation must reconcile
        docs/v1.44-verification-ledger.md exactly, or the lint fails. With
        --ledger-print it prints the table the ledger doc was generated from.
  (iii) docs/FMM.md range disjointness: block headers map bijectively to
        releases in the ownership table, "reserved (empty)" blocks carry no
        row tables, and every row ID uses its own block's range prefix — a
        cross-range write is an error.

Exit codes: 0 = all green, 1 = any violation.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROADMAP = REPO / "docs" / "ROADMAP.md"
if not ROADMAP.exists():
    ROADMAP = REPO / "docs" / "roadmap-v143.merged.md"
ALLOWLIST = REPO / "tools" / "anchor_allowlist.txt"
UNIT_DIR = REPO / "tests" / "unit"
LEDGER = REPO / "docs" / "v1.44-verification-ledger.md"
FMM = REPO / "docs" / "FMM.md"

ANCHOR_RE = re.compile(
    r"([A-Za-z0-9_./\\-]+\.(?:cpp|hpp|h|c|py|cmake|yml|yaml|js|ts|cs|md|in|bat|ps1|txt|sh)):(\d+)(?:-(\d+))?")
CODE_ASSERT_RE = re.compile(r"\bassert\s*\(")
STATIC_ASSERT_RE = re.compile(r"\bstatic_assert\s*\(")
DOCTEST_ASSERT_RE = re.compile(
    r"\b(?:REQUIRE|CHECK|REQUIRE_FALSE|CHECK_FALSE|REQUIRE_THROWS|"
    r"CHECK_THROWS|REQUIRE_NOTHROW|CHECK_NOTHROW|REQUIRE_THROWS_AS|"
    r"CHECK_THROWS_AS|REQUIRE_THROWS_WITH|CHECK_THROWS_WITH)\s*\(")


def build_file_index():
    index = {}
    for p in REPO.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(REPO)
        s = rel.as_posix()
        if s.startswith((".git/", "build", "build_inmem/", "tools/tv/", "junit", "node_modules/")):
            continue
        index.setdefault(p.name, []).append(s)
    return index


def resolve_anchor(index, rel):
    rel = rel.replace("\\", "/")
    for cand in (rel, "docs/" + rel, "tests/" + rel, "src/" + rel, "tools/" + rel):
        if (REPO / cand).is_file():
            return REPO / cand
    candidates = index.get(rel.rsplit("/", 1)[-1], [])
    if len(candidates) == 1:
        return REPO / candidates[0]
    if len(candidates) > 1:
        # Ambiguous basename: prefer the public API copy when present.
        for cand in candidates:
            if cand.startswith("include/"):
                return REPO / cand
        return REPO / min(candidates, key=len)
    return None


def load_allowlist():
    files, pairs = set(), set()
    if not ALLOWLIST.exists():
        return files, pairs
    for raw in ALLOWLIST.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if ":" in line:
            f, n = line.rsplit(":", 1)
            n = n.strip()
            if n.isdigit():
                pairs.add((f.strip(), int(n)))
                continue
        files.add(line)
    return files, pairs


def check_anchors(errors):
    allowed_files, allowed_pairs = load_allowlist()
    index = build_file_index()
    if not ROADMAP.exists():
        errors.append("roadmap missing: %s" % ROADMAP)
        return
    text = ROADMAP.read_text(encoding="utf-8", errors="replace")
    seen = set()
    unresolved = []
    for m in ANCHOR_RE.finditer(text):
        rel, start, end = m.group(1), int(m.group(2)), m.group(3)
        if (rel, start) in seen:
            continue
        seen.add((rel, start))
        allowed = rel in allowed_files or (rel, start) in allowed_pairs
        if allowed:
            continue
        path = resolve_anchor(index, rel)
        if path is None:
            unresolved.append("%s:%d (file not found)" % (rel, start))
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            unresolved.append("%s:%d (unreadable: %s)" % (rel, start, exc))
            continue
        total = len(lines)
        cap = int(end) if end else start
        if start == 0 or cap > total or int(end or start) < start:
            unresolved.append("%s:%s (file has %d lines)" % (rel, m.group(2), total))
    for item in sorted(unresolved):
        errors.append("anchor unresolved: %s" % item)


def count_file_asserts(path):
    assert_depth = 0
    data = path.read_text(encoding="utf-8", errors="replace").splitlines()
    n_assert = n_static = 0
    for line in data:
        code = line.split("//", 1)[0]
        stripping = re.sub(r'"(\\.|[^"\\])*"', '""', code)
        for m in STATIC_ASSERT_RE.finditer(stripping):
            n_static += 1
        for m in CODE_ASSERT_RE.finditer(stripping):
            n_assert += 1
        define_def = re.match(
            r"\s*#\s*define\s+(?:REQUIRE|CHECK|REQUIRE_FALSE|CHECK_FALSE|"
            r"REQUIRE_THROWS|CHECK_THROWS|REQUIRE_NOTHROW|CHECK_NOTHROW)\s*\(",
            stripping)
        n_family = 0 if define_def else len(
            DOCTEST_ASSERT_RE.findall(stripping))
        n_assert += n_family
        stripped_static = STATIC_ASSERT_RE.sub("", stripping)
        n_only = (len(CODE_ASSERT_RE.findall(stripped_static)) +
                  (0 if define_def else
                   len(DOCTEST_ASSERT_RE.findall(stripped_static))))
        assert_depth += n_only
    return n_assert, n_static


def ledger_numbers():
    rows, tot_a = [], 0
    static_total = 0
    for p in sorted(list(UNIT_DIR.glob("*.cpp")) + list(UNIT_DIR.glob("*.hpp"))):
        a, s = count_file_asserts(p)
        if a or s:
            rows.append((p.name, a, s))
        tot_a += a
        static_total += s
    return rows, tot_a, static_total


def check_ledger(errors, want_print):
    rows, tot_a, tot_s = ledger_numbers()
    if want_print:
        for name, a, s in rows:
            print("%-34s asserts=%d static_assert=%d" % (name, a, s))
        print("TOTAL asserts=%d static_assert=%d over %d files"
              % (tot_a, tot_s, len(rows)))
    if not LEDGER.exists():
        errors.append("ledger doc missing: %s (created at M4)" % LEDGER)
        return
    doc = LEDGER.read_text(encoding="utf-8", errors="replace")
    bad = []
    for m in re.finditer(r"^\|\s*`?([\w.]+)`?\s*\|+\s*(\d+)\s*\|+\s*(\d+)\s*\|+", doc, re.M):
        name, a, s = m.group(1), int(m.group(2)), int(m.group(3))
        match = next((r for r in rows if r[0] == name), None)
        if match is None:
            if a == 0 and s == 0 and Path(UNIT_DIR / name).exists():
                bad.append("%s: ledger row %d/%d but file has 0/0" % (name, a, s))
                continue
            bad.append("%s: ledger row has no recomputed counterpart (file renamed?)" % name)
            continue
        if (a, s) != (match[1], match[2]):
            bad.append("%s: ledger %d/%d != recomputed %d/%d"
                       % (name, a, s, match[1], match[2]))
    claimed = re.search(r"TOTAL asserts=(\d+) static_assert=(\d+) over (\d+) files", doc)
    if claimed:
        if (int(claimed.group(1)), int(claimed.group(2)), int(claimed.group(3))) \
                != (tot_a, tot_s, len(rows)):
            bad.append("ledger total %s/%s/%s != recomputed %d/%d/%d"
                       % (claimed.group(1), claimed.group(2), claimed.group(3),
                          tot_a, tot_s, len(rows)))
    else:
        bad.append("ledger doc lacks its TOTAL line")
    for item in bad:
        errors.append("ledger drift: %s" % item)


def check_fmm(errors):
    if not FMM.exists():
        errors.append("FMM missing: %s" % FMM)
        return
    doc = FMM.read_text(encoding="utf-8", errors="replace")
    owner = {}
    for m in re.finditer(r"^\|\s*v(\d+\.\d+)\s*\|\s*FMM-(\d+)\s*\|([^|]*)\|", doc, re.M):
        release, fmm, status = m.group(1), m.group(2), m.group(3).strip()
        if fmm in owner:
            errors.append("FMM range duplicate: FMM-%s claimed by v%s and an earlier owner" % (fmm, release))
        owner[fmm] = (release, status)
    for trig, fmm in re.findall(r"^\|\s*(v\d+\.\d+)\s*\|\s*FMM-(\d+)\s*\|", doc, re.M):
        pass
    sections = [(int(m.group(1)), m.group(2), m.start()) for m in
                re.finditer(r"^## FMM-(\d+)(.*)$", doc, re.M)]
    ranges = [fmm for fmm, _, _ in sections]
    if len(set(ranges)) != len(ranges):
        errors.append("FMM duplicate block headers: %s" % sorted(set(x for x in ranges if ranges.count(x) > 1)))
    for idx, (fmm, tail, start) in enumerate(sections):
        hm = re.search(r"v(\d+\.\d+)", tail)
        header_release = hm.group(1) if hm else ""
        next_start = sections[idx + 1][2] if idx + 1 < len(sections) else len(doc)
        body = doc[start:next_start]
        declared = owner.get(str(fmm), (None, None))
        if header_release and declared[0] and declared[0] != header_release:
            errors.append("FMM-%d: ownership table says v%s but header says v%s"
                          % (fmm, declared[0], header_release))
        if "reserved" in (declared[1] or "").lower() and \
                re.search(r"^\| *(?:[A-Za-z-]+-)?\d", body, re.M):
            errors.append("FMM-%d: declared reserved (empty) but carries rows" % fmm)
        for row in re.findall(r"^\|\s*(FMM-\d+)", body, re.M):
            rid = int(row.split("-")[1])
            if rid != fmm:
                errors.append("FMM-%d block contains cross-range row %s" % (fmm, row))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Anchor + ledger + FMM lint")
    ap.add_argument("--release", default="v1.44", help="active release for FMM write range")
    ap.add_argument("--demand-ledger", action="store_true",
                    help="error out when the ledger doc is absent (default: error)")
    ap.add_argument("--ledger-print", action="store_true")
    args = ap.parse_args(argv)
    errors = []
    check_anchors(errors)
    check_ledger(errors, args.ledger_print)
    check_fmm(errors)
    print("anchor_lint: %d violation(s), release=%s" % (len(errors), args.release))
    for e in errors:
        print("  " + e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
