#!/usr/bin/env python3
"""v1.40 many-entry extraction harness (not part of the perf-vs-WinRAR
canonical run): the per-entry quick-win scorecard (has_symlink_parent chain
caching, containment cache-hit dup elision, leaf_is_reparse elision) is
invisible on the 3-entry canonical corpus, so this bench packs a corpus of
hundreds of small files in nested directories where per-entry costs dominate.

Measured: openrar extraction of the same corpus in entry and batch
durability granularity (-mt1), plus an UnRAR reference row. The extraction
root is deliberately deep so the lexical parent chain (has_symlink_parent)
has realistic length.

Usage:
    python tools/bench_extract_entries.py [--runs 5] [--label variant]
        [--files 30] [--skip-pack]
"""
import argparse
import hashlib
import json
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PERF = REPO / "tools" / "perf"
DATA = PERF / "data"
MANY = DATA / "many"
OUT = DATA / "out"

OPENRAR = None
for c in (REPO / "build" / "openrar64" / "Release" / "openrar.exe",
          REPO / "build" / "Release" / "openrar.exe",
          REPO / "build" / "openrar.exe"):
    if c.exists():
        OPENRAR = str(c)
        break
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"

N_DIRS = 20
PER_DIR_DEFAULT = 30
FILE_SIZE = 4096


def file_bytes(i: int, size: int) -> bytes:
    seed = hashlib.sha256(f"openrar-many-{i}".encode()).digest()
    return (seed * ((size // len(seed)) + 1))[:size]


def gen_corpus(per_dir: int) -> int:
    """Deterministic nested corpus: d00..d19 x per_dir files + deep/a/b/c."""
    if MANY.exists():
        keep_marker = MANY / ".generated"
        if keep_marker.exists() and keep_marker.read_text() == f"{per_dir}":
            return count_files()
    shutil.rmtree(MANY, ignore_errors=True)
    idx = 0
    for d in range(N_DIRS):
        dd = MANY / f"d{d:02d}"
        dd.mkdir(parents=True, exist_ok=True)
        for i in range(per_dir):
            (dd / f"f{i:03d}.bin").write_bytes(file_bytes(idx, FILE_SIZE))
            idx += 1
    deep = MANY / "deep" / "a" / "b" / "c"
    deep.mkdir(parents=True, exist_ok=True)
    for i in range(10):
        (deep / f"deep{i:02d}.bin").write_bytes(file_bytes(idx, FILE_SIZE))
        idx += 1
    (MANY / ".generated").write_text(str(per_dir))
    return idx


def count_files() -> int:
    return sum(1 for p in MANY.rglob("*") if p.is_file() and p.name != ".generated")


def corpus_size() -> int:
    return sum(p.stat().st_size for p in MANY.rglob("*") if p.is_file() and p.name != ".generated")


def run(cmd, cwd=None) -> float:
    t0 = time.perf_counter()
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"FAIL ({p.returncode}): {' '.join(map(str, cmd))}")
        print((p.stdout or "") + (p.stderr or ""))
        raise SystemExit(1)
    return time.perf_counter() - t0


def sha_tree(root: Path) -> str:
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.name != ".generated":
            h.update(str(p.relative_to(root)).encode())
            h.update(p.read_bytes())
    return h.hexdigest()


def pack(arc: Path, method: int):
    if arc.exists():
        return
    # Directory arguments preserve relative paths (file arguments store
    # basenames only — verified against WinRAR parity).
    names = sorted(p.name for p in MANY.iterdir() if p.is_dir())
    run([OPENRAR, "a", "-q", f"-m{method}", "-mt1", str(arc)] + names, cwd=str(MANY))


def run_ab(args, arc0, arc3, src_hash):
    """Interleaved A/B: within each config, alternate the two binaries per
    iteration so environment drift hits both arms equally."""
    bin_a, bin_b = args.ab
    arms = {"A": bin_a, "B": bin_b}
    configs = [
        ("m0_entry", ""),
        ("m0_batch", "-db"),
        ("m3_entry", ""),
        ("m3_batch", "-db"),
    ]
    results = []
    for name, db in configs:
        runs = {"A": [], "B": []}
        for i in range(args.runs + 1):  # run 0 = warmup, dropped
            order = ["A", "B"] if i % 2 == 0 else ["B", "A"]
            for arm in order:
                ex = OUT / f"ab_{name}_{arm}_r"
                deep_out = ex / "a" / "b" / "c" / "d"
                shutil.rmtree(ex, ignore_errors=True)
                deep_out.mkdir(parents=True)
                cmd = [arms[arm], "x", "-y", "-q", "-mt1"]
                if db:
                    cmd.append(db)
                cmd += [str(arc0 if name.startswith("m0") else arc3), str(deep_out)]
                dt = run(cmd)
                if i > 0:
                    runs[arm].append(dt)
                if sha_tree(deep_out) != src_hash:
                    print(f"FAIL: {name} {arm} run{i} extracted tree mismatch")
                    return 1
                shutil.rmtree(ex, ignore_errors=True)
                print(f"    {name} {arm} run{i}: {dt:.3f}s")
        for arm in ("A", "B"):
            results.append({"label": args.label, "arm": arm, "binary": arms[arm],
                            "config": name, "runs": runs[arm],
                            "median": statistics.median(runs[arm]),
                            "min": min(runs[arm])})

    out_path = PERF / "many_entries_results.json"
    old = []
    if out_path.exists():
        try:
            old = json.load(open(out_path))
        except (json.JSONDecodeError, OSError):
            old = []
    old = [r for r in old if r.get("label") != args.label]
    json.dump(old + results, open(out_path, "w"), indent=1)

    print(f"\n=== interleaved A/B ({args.label}) ===")
    print(f"{'config':10s} {'A median':>9s} {'B median':>9s} {'A min':>9s} "
          f"{'B min':>9s} {'min d%':>8s}")
    for name, _ in configs:
        a = next(r for r in results if r["config"] == name and r["arm"] == "A")
        b = next(r for r in results if r["config"] == name and r["arm"] == "B")
        dmin = (b["min"] - a["min"]) / a["min"] * 100.0
        print(f"{name:10s} {a['median']:9.3f} {b['median']:9.3f} "
              f"{a['min']:9.3f} {b['min']:9.3f} {dmin:+7.1f}%")
    print("A = " + bin_a)
    print("B = " + bin_b)
    print(f"-> {out_path}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--label", default="unlabeled")
    ap.add_argument("--files", type=int, default=PER_DIR_DEFAULT)
    ap.add_argument("--skip-pack", action="store_true")
    ap.add_argument("--ab", nargs=2, metavar=("BIN_A", "BIN_B"),
                    help="interleaved A/B: alternate the two binaries run-by-run "
                         "(cancels environment drift between arms)")
    args = ap.parse_args()

    if not OPENRAR:
        print("openrar.exe not found — build Release first.")
        return 1
    if not sys.platform == "win32":
        print("Windows-only harness.")
        return 1

    n = gen_corpus(args.files)
    src_hash = sha_tree(MANY)
    print(f"corpus: {n} files, {corpus_size() / (1 << 20):.1f} MiB at {MANY}")

    OUT.mkdir(parents=True, exist_ok=True)
    arc0 = OUT / "many_m0.rar"
    arc3 = OUT / "many_m3.rar"
    if not args.skip_pack:
        pack(arc0, 0)
        pack(arc3, 3)

    if args.ab:
        return run_ab(args, arc0, arc3, src_hash)

    version = subprocess.run([OPENRAR, "--version"], capture_output=True,
                             text=True).stdout.splitlines()[0].strip()

    configs = [
        ("m0_entry", [OPENRAR, "x", "-y", "-q", "-mt1", str(arc0)]),
        ("m0_batch", [OPENRAR, "x", "-y", "-q", "-mt1", "-db", str(arc0)]),
        ("m3_entry", [OPENRAR, "x", "-y", "-q", "-mt1", str(arc3)]),
        ("m3_batch", [OPENRAR, "x", "-y", "-q", "-mt1", "-db", str(arc3)]),
        ("m3_unrar", [UNRAR, "x", "-y", "-mt1", str(arc3)]),
    ]

    results = []
    for name, cmd in configs:
        runs = []
        for i in range(args.runs + 1):  # run 0 = warmup, dropped
            ex = OUT / f"many_{name}_r"
            # deep extraction root: amplifies the lexical parent chain
            deep_out = ex / "a" / "b" / "c" / "d"
            shutil.rmtree(ex, ignore_errors=True)
            deep_out.mkdir(parents=True)
            dt = run(cmd + [str(deep_out) + "\\"] if name.endswith("unrar")
                     else cmd + [str(deep_out)])
            if i > 0:
                runs.append(dt)
            got_hash = sha_tree(deep_out)
            if got_hash != src_hash:
                print(f"FAIL: {name} run{i} extracted tree mismatch")
                return 1
            shutil.rmtree(ex, ignore_errors=True)
            print(f"    {name} run{i}: {dt:.3f}s")
        row = {"label": args.label, "config": name,
               "runs": runs, "median": statistics.median(runs),
               "min": min(runs), "version": version}
        results.append(row)

    out_path = PERF / "many_entries_results.json"
    old = []
    if out_path.exists():
        try:
            old = json.load(open(out_path))
        except (json.JSONDecodeError, OSError):
            old = []
    old = [r for r in old if r.get("label") != args.label]
    json.dump(old + results, open(out_path, "w"), indent=1)

    print(f"\n=== many-entry extraction ({args.label}) ===")
    print(f"{'config':12s} {'median':>9s} {'min':>9s}")
    for r in results:
        print(f"{r['config']:12s} {r['median']:9.3f} {r['min']:9.3f}")
    print(f"-> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
