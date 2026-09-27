#!/usr/bin/env python3
"""Layer-direction checker for OpenRAR (ARCHITECTURE.md section 2).

Verifies that every #include edge between src/ subsystems points downward
only: higher layers may include lower layers, never the reverse. Boundary
surfaces (src/api, src/dll, src/wasm) sit above the stack; the public API
headers (include/openrar/) are includable only from the boundary surfaces and
the CLI. Including a .cpp file is forbidden outright (ARCHITECTURE.md 3.1).

Ratchet model (see docs/verification-legs.md): tools/layer_baseline.json
holds accepted violations as "includer -> included" path pairs (no line
numbers, so edits don't churn the baseline). New violations fail; baseline
updates are manual (`--update-baseline`), never automatic — CI never grows
the baseline.

Usage:
    python tools/layer_check.py                 # full tree (what CI runs)
    python tools/layer_check.py --changed       # only edges touching git-changed files
    python tools/layer_check.py --update-baseline
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BASELINE = REPO / "tools" / "layer_baseline.json"

# Rank 0 is the bottom of the stack (ARCHITECTURE.md section 2 diagram).
# Same-rank includes are allowed (the mid tier compress/format/recovery are
# peers); lower-rank includers are violations.
#
# Two subsystem directories predate this checker but are missing from the
# ARCHITECTURE.md diagram; they sit where their includes put them:
#   - src/sfx: the SFX directive runtime, driven only by src/cli — ranked
#     beside cli at the top. It includes core only.
#   - src/unicode: generated Unicode 15.1.0 tables, pure leaf data over core.
LAYER_RANK = {
    "core": 0,
    "unicode": 0,
    "io": 1,
    "crypto": 2,
    "compress": 3,
    "format": 3,
    "recovery": 3,
    "archive": 4,
    "cli": 5,
    "sfx": 5,
    # src/sandbox (v1.30.0): the broker/worker sandboxed-parse runtime,
    # driven by cli; includes core (+ io via the spawn layer) only.
    "sandbox": 5,
    # Boundary surfaces wrap the top of the stack.
    "api": 6,
    "dll": 6,
    "wasm": 6,
}
# include/openrar/ public API headers: only these layers may include them.
PUBLIC_API_USERS = {"cli", "api", "dll", "wasm"}

SOURCE_EXTS = {".cpp", ".hpp", ".h"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def layer_of(rel_path: str) -> str:
    """Map a repo-relative source path to its layer name."""
    if rel_path.startswith("include/"):
        return "public_api"
    parts = Path(rel_path).parts
    if len(parts) >= 3 and parts[0] == "src":
        return parts[1]
    return "external"


def rank(layer: str) -> int:
    return LAYER_RANK.get(layer, 99)


def collect_sources() -> list:
    files = []
    for base in ("src", "include"):
        files.extend(p for p in (REPO / base).rglob("*")
                     if p.suffix in SOURCE_EXTS and p.is_file())
    return sorted(files)


def resolve_include(target: str, includer: Path) -> Path | None:
    """Resolve a quoted include to a repo file, mirroring CMake include dirs."""
    cand = includer.parent / target
    if cand.is_file():
        return cand.resolve()
    for base in (REPO / "src", REPO / "include"):
        cand = base / target
        if cand.is_file():
            return cand.resolve()
    return None


def extract_edges() -> list:
    """Yield (includer_rel, included_rel, line_no) for every internal edge."""
    edges = []
    for src in collect_sources():
        includer_rel = src.relative_to(REPO).as_posix()
        try:
            text = src.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line.split("//")[0])
            if not m:
                continue
            target = m.group(1)
            if target.endswith(".cpp"):
                edges.append((includer_rel, "<cpp-include:" + target + ">", line_no))
                continue
            resolved = resolve_include(target, src)
            if resolved is None:
                continue  # unknown/generated header — not an internal edge
            included_rel = resolved.relative_to(REPO).as_posix()
            if included_rel == includer_rel:
                continue
            edges.append((includer_rel, included_rel, line_no))
    return edges


def violation(includer_rel: str, included_rel: str) -> str | None:
    """Return a message if this edge breaks the layer rules, else None."""
    src_layer = layer_of(includer_rel)
    dst_layer = layer_of(included_rel)
    if src_layer == "external" or dst_layer == "external":
        return None
    if dst_layer == "public_api":
        if src_layer == "public_api":
            return None  # public headers include each other freely
        if src_layer not in PUBLIC_API_USERS:
            return (f"public API header included from '{src_layer}' "
                    f"(allowed only from: {', '.join(sorted(PUBLIC_API_USERS))})")
        return None
    if rank(src_layer) < rank(dst_layer):
        return f"upward include: {src_layer} -> {dst_layer}"
    return None


def edge_key(includer_rel: str, included_rel: str) -> str:
    return f"{includer_rel} -> {included_rel}"


def load_baseline() -> dict:
    if not BASELINE.is_file():
        return {"version": 1, "violations": []}
    return json.loads(BASELINE.read_text(encoding="utf-8"))


def changed_files() -> set:
    """Files modified vs HEAD (staged + unstaged) plus untracked files."""
    out = set()
    for cmd in (["git", "diff", "--name-only", "HEAD"],
                ["git", "ls-files", "--others", "--exclude-standard"]):
        try:
            res = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
            out.update(line.strip() for line in res.stdout.splitlines() if line.strip())
        except OSError:
            continue
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--changed", action="store_true",
                    help="only fail on edges touching git-changed files")
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite the baseline with current violations (manual)")
    args = ap.parse_args()

    edges = extract_edges()
    scoped = edges
    if args.changed:
        touched = changed_files()
        scoped = [e for e in edges
                  if e[0] in touched or e[1] in touched]

    baseline = load_baseline()
    accepted = set(baseline.get("violations", []))

    new_violations = []   # (key, line_no, reason)
    accepted_hits = []    # edges matched by the baseline
    for includer_rel, included_rel, line_no in scoped:
        reason = violation(includer_rel, included_rel)
        if reason is None:
            continue
        key = edge_key(includer_rel, included_rel)
        if key in accepted:
            accepted_hits.append((key, line_no, reason))
        else:
            new_violations.append((key, line_no, reason))

    if args.update_baseline:
        all_keys = sorted({edge_key(i, d) for i, d, _ in edges
                           if violation(i, d) is not None})
        doc = {"version": 1,
               "notes": baseline.get("notes", []),
               "violations": all_keys}
        BASELINE.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"Baseline updated: {len(all_keys)} accepted violation(s) "
              f"in {BASELINE.relative_to(REPO).as_posix()}")
        print("Review the diff — the baseline only ever shrinks by hand.")
        return 0

    print(f"Layer check: {len(edges)} internal include edges scanned"
          + (" (changed-file scope)" if args.changed else ""))

    if accepted_hits:
        print(f"Accepted by baseline: {len(accepted_hits)}")
        for key, _, reason in accepted_hits:
            print(f"  [baseline] {key}  ({reason})")

    if new_violations:
        print(f"FAILED: {len(new_violations)} new layer violation(s) "
              f"(ARCHITECTURE.md section 2, downward-only):")
        for key, line_no, reason in sorted(new_violations):
            print(f"  {key}:{line_no}  ({reason})")
        print("Fix the include direction, or — only with a documented reason —")
        print(f"run `python tools/layer_check.py --update-baseline` and commit "
              f"the baseline diff.")
        return 1

    print("OK: no new layer violations "
          f"({len(accepted)} pre-existing accepted).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
