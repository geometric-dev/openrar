#!/usr/bin/env python3
"""
tools/release_regression_guard.py - release gate against the previous release (Directive 14).

Builds nothing and trusts nothing: it runs the binary under test and the v2.0.23 release
binary on the same census corpora and FAILS CLOSED (exit 1) when the new build is worse.

  Size     every (corpus, method) cell: new size must not exceed v2.0.23's by more than the
           cell's entry in KNOWN_SIZE_ALLOWANCE_PCT (0.0 when absent). Sizes are
           deterministic, so there is no noise margin.
  Speed    throughput must stay >= speed_floor(method) x v2.0.23's on the SPEED_CELLS (0.90 at
           -m1..-m3, 0.65 at -m4/-m5), using the fastest of SPEED_REPS runs of each binary on
           this machine.
  G2       zeros8 at -m1/-m3/-m5 must be <= 587 B.
  Integrity  every archive the new binary produces is extracted and compared by SHA-256.
  Determinism  -mt1 archives are byte-identical across runs; -mt4 extracts to the source.
  Engines  the alternate parser at every method (forward-arrival where the default is
           greedy and vice versa) must still round-trip, so the opt-in path cannot rot.

The baseline binary is MANDATORY. A missing or mis-versioned baseline is a failure, never a
skip: the previous version of this script fell back to a hard-coded size when it was absent.

Usage:
  release_regression_guard.py [--binary PATH] [--baseline PATH] [--quick]
  OPENRAR_BASELINE_BIN may stand in for --baseline.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from bench_m0_census import WORK_DIR as CENSUS_DIR, generate_corpora  # noqa: E402

BASELINE_VERSION = "2.0.23"
EXE = ".exe" if os.name == "nt" else ""
WORK_DIR = ROOT / "build" / "regression_guard"

# Throughput floor relative to the baseline on the same machine. The shipped parsers are
# faster than v2.0.23 at m1-m3 and on big24/mixed16 at m4-m5; the worst measured cell is
# synthetic text at -m4 (0.76x in clean runs, 0.71x under load), where the forward-arrival
# parser does more work per byte. 0.65 leaves room for scheduler noise on a shared runner while
# still catching the class of bug this guard exists for (an unbounded search that ran 18x
# slower than the release).
SPEED_FLOOR_DEFAULT = 0.65
# The greedy methods clear 0.90x with a wide margin (measured 1.4x-3.6x), so they are held to
# the stricter floor. Only the forward-arrival methods get the looser one.
SPEED_FLOOR_BY_METHOD = {1: 0.90, 2: 0.90, 3: 0.90}
SPEED_REPS = 3


def speed_floor(method):
    return SPEED_FLOOR_BY_METHOD.get(method, SPEED_FLOOR_DEFAULT)


# A cell is only too short to judge when BOTH runs are below this (wall clock is then mostly
# process start-up). It must never depend on the baseline alone: a slow new build against a
# fast baseline is exactly the regression this check exists to catch.
SPEED_TOO_SHORT_S = 0.4

# (corpus, method) -> permitted growth over v2.0.23, in percent. EVERYTHING ELSE MUST BE <= 0.
# The one measured, understood loss: -m5 on big24.bin is +0.028% (+2,942 B of 10.5 MB), from
# the tiled repository-source third where the forward-arrival parser trails greedy. It is
# listed so that the only way this cell gets worse is to fail the guard. See plan section 7.2.
KNOWN_SIZE_ALLOWANCE_PCT = {
    ("big24.bin", 5): 0.03,
}

SIZE_CORPORA = ("big24.bin", "mixed16.bin", "synthetic_text8.txt")
SPEED_CELLS = (
    ("random8.bin", 3),
    ("random8.bin", 5),
    ("synthetic_text8.txt", 1),
    ("synthetic_text8.txt", 3),
    ("synthetic_text8.txt", 4),  # slowest cell relative to v2.0.23 (0.76x in clean runs)
    ("synthetic_text8.txt", 5),
    ("big24.bin", 5),
)
ZERO_RUN_LIMIT_BYTES = 587  # G2

# Engine switches that must not leak in from the caller's shell.
SCRUB_ENV = (
    "OPENRAR_NO_FORWARD_ARRIVAL",
    "OPENRAR_ENABLE_FORWARD_ARRIVAL",
    "OPENRAR_EXPERIMENTAL_M1_DAG",
    "OPENRAR_PRICE_ARM",
    "OPENRAR_NO_DYNAMIC_PRICE",
    "OPENRAR_REFCAP",
)

failures = []


def fail(msg):
    failures.append(msg)
    print(f"  [FAIL] {msg}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, env_extra=None, timeout=1200):
    env = dict(os.environ)
    for k in SCRUB_ENV:
        env.pop(k, None)
    if env_extra:
        env.update(env_extra)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=timeout)
    return p, time.perf_counter() - t0


def pack(binary, corpus, method, out, mt=1, env_extra=None):
    """Returns (size, seconds) or raises RuntimeError."""
    if out.exists():
        out.unlink()
    cmd = [str(binary), "a", "-y", f"-m{method}", f"-mt{mt}", "-md4m", str(out), str(corpus)]
    p, dt = run(cmd, env_extra)
    if p.returncode != 0 or not out.exists():
        raise RuntimeError(f"{binary.name} a -m{method} -mt{mt} {corpus.name} exited {p.returncode}: "
                           f"{p.stderr.decode(errors='replace')[:200]}")
    return out.stat().st_size, dt


def extract_matches(binary, archive, corpus, label):
    """Extract with the binary under test and compare to the source by SHA-256."""
    dest = WORK_DIR / "extract"
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    p, _ = run([str(binary), "x", "-y", str(archive), str(dest)])
    got = dest / corpus.name
    if p.returncode != 0 or not got.exists():
        fail(f"{label}: extraction failed (exit {p.returncode})")
        return False
    if sha256_file(got) != sha256_file(corpus):
        fail(f"{label}: extracted bytes differ from the source")
        return False
    return True


def locate_current(arg):
    if arg:
        return Path(arg)
    for rel in (f"build/openrar64/Release/openrar{EXE}", f"build/Release/openrar{EXE}",
                f"build/openrar{EXE}", f"build/bin/openrar{EXE}"):
        if (ROOT / rel).exists():
            return ROOT / rel
    return ROOT / f"build/openrar{EXE}"


def locate_baseline(arg):
    if arg:
        return Path(arg)
    if os.environ.get("OPENRAR_BASELINE_BIN"):
        return Path(os.environ["OPENRAR_BASELINE_BIN"])
    return ROOT / "bin" / f"v{BASELINE_VERSION}" / f"openrar{EXE}"


def version_of(binary):
    p, _ = run([str(binary), "--version"], timeout=60)
    return (p.stdout.decode(errors="replace").strip() or p.stderr.decode(errors="replace").strip())


def section(title):
    print(f"\n--- {title} ---")


def check_sizes(cur, base, corpora, methods):
    section("Size vs v2.0.23 (deterministic; allowance per cell)")
    for name in SIZE_CORPORA:
        corpus = corpora[name]
        for m in methods:
            try:
                base_size, _ = pack(base, corpus, m, WORK_DIR / "base.rar")
                cur_size, _ = pack(cur, corpus, m, WORK_DIR / "cur.rar")
            except RuntimeError as e:
                fail(f"size {name} -m{m}: {e}")
                continue
            allow = KNOWN_SIZE_ALLOWANCE_PCT.get((name, m), 0.0)
            limit = base_size * (1.0 + allow / 100.0)
            delta_pct = (cur_size - base_size) * 100.0 / base_size
            label = f"{name:<20} -m{m}  new={cur_size:>10,}  v2.0.23={base_size:>10,}  {delta_pct:+7.3f}%  (allow +{allow:.2f}%)"
            if cur_size > limit:
                fail("size " + label)
            else:
                print("  [PASS] size " + label)
            extract_matches(cur, WORK_DIR / "cur.rar", corpus, f"integrity {name} -m{m}")


def check_zero_run(cur, base, corpora, methods):
    section("G2 zero-run bound")
    corpus = corpora["zeros8.bin"]
    for m in methods:
        try:
            cur_size, _ = pack(cur, corpus, m, WORK_DIR / "cur.rar")
            base_size, _ = pack(base, corpus, m, WORK_DIR / "base.rar")
        except RuntimeError as e:
            fail(f"zeros8 -m{m}: {e}")
            continue
        label = f"zeros8.bin -m{m}  new={cur_size} B  v2.0.23={base_size} B  limit={ZERO_RUN_LIMIT_BYTES} B"
        if cur_size > ZERO_RUN_LIMIT_BYTES or cur_size > base_size:
            fail("G2 " + label)
        else:
            print("  [PASS] G2 " + label)
        extract_matches(cur, WORK_DIR / "cur.rar", corpus, f"integrity zeros8 -m{m}")

    corpus = corpora["random8.bin"]
    for m in methods:
        try:
            cur_size, _ = pack(cur, corpus, m, WORK_DIR / "cur.rar")
            base_size, _ = pack(base, corpus, m, WORK_DIR / "base.rar")
        except RuntimeError as e:
            fail(f"random8 -m{m}: {e}")
            continue
        label = f"random8.bin -m{m}  new={cur_size:,}  v2.0.23={base_size:,}"
        if cur_size > base_size:
            fail("incompressible " + label)
        else:
            print("  [PASS] incompressible " + label)
        extract_matches(cur, WORK_DIR / "cur.rar", corpus, f"integrity random8 -m{m}")


def fastest(binary, corpus, method, reps):
    best = None
    for _ in range(reps):
        _, dt = pack(binary, corpus, method, WORK_DIR / "speed.rar")
        best = dt if best is None else min(best, dt)
    return best


def check_speed(cur, base, corpora, cells):
    section(f"Speed vs v2.0.23 (fastest of {SPEED_REPS}; floor 0.90x at -m1..-m3, "
            f"{SPEED_FLOOR_DEFAULT:.2f}x at -m4/-m5)")
    for name, m in cells:
        corpus = corpora[name]
        try:
            base_t = fastest(base, corpus, m, SPEED_REPS)
            cur_t = fastest(cur, corpus, m, SPEED_REPS)
        except RuntimeError as e:
            fail(f"speed {name} -m{m}: {e}")
            continue
        mib = corpus.stat().st_size / (1024 * 1024)
        ratio = base_t / cur_t if cur_t > 0 else float("inf")
        label = (f"{name:<20} -m{m}  new={mib / cur_t:6.2f} MB/s  v2.0.23={mib / base_t:6.2f} MB/s  "
                 f"ratio={ratio:5.2f}x")
        if max(base_t, cur_t) < SPEED_TOO_SHORT_S:
            print("  [SKIP] speed " + label + "  (both runs too short to time reliably)")
        elif ratio < speed_floor(m):
            fail("speed " + label)
        else:
            print("  [PASS] speed " + label)


def check_determinism(cur, corpora):
    section("Determinism and thread parity")
    corpus = corpora["mixed16.bin"]
    try:
        pack(cur, corpus, 3, WORK_DIR / "mt1_a.rar", mt=1)
        pack(cur, corpus, 3, WORK_DIR / "mt1_b.rar", mt=1)
        pack(cur, corpus, 3, WORK_DIR / "mt4.rar", mt=4)
    except RuntimeError as e:
        fail(f"determinism: {e}")
        return
    if sha256_file(WORK_DIR / "mt1_a.rar") != sha256_file(WORK_DIR / "mt1_b.rar"):
        fail("-mt1 archives differ between two identical runs")
    else:
        print("  [PASS] -mt1 is byte-identical across runs")
    if extract_matches(cur, WORK_DIR / "mt4.rar", corpus, "-mt4 archive"):
        print("  [PASS] -mt4 archive extracts to the source")


def check_alternate_engines(cur, corpora, methods):
    section("Alternate parser still round-trips (opt-in / kill-switch paths)")
    corpus = corpora["synthetic_text8.txt"]
    for m in methods:
        for env, tag in (({"OPENRAR_ENABLE_FORWARD_ARRIVAL": "1"}, "forward-arrival forced on"),
                         ({"OPENRAR_NO_FORWARD_ARRIVAL": "1"}, "greedy forced on")):
            try:
                size, _ = pack(cur, corpus, m, WORK_DIR / "alt.rar", env_extra=env)
            except RuntimeError as e:
                fail(f"engine {tag} -m{m}: {e}")
                continue
            if extract_matches(cur, WORK_DIR / "alt.rar", corpus, f"engine {tag} -m{m}"):
                print(f"  [PASS] {tag:<26} -m{m}  {size:,} B round-trips")
    zeros = corpora["zeros8.bin"]
    try:
        size, _ = pack(cur, zeros, 5, WORK_DIR / "alt.rar", env_extra={"OPENRAR_ENABLE_FORWARD_ARRIVAL": "1"})
        if size > ZERO_RUN_LIMIT_BYTES:
            fail(f"G2 with forward-arrival forced on: zeros8 -m5 = {size} B")
        else:
            print(f"  [PASS] G2 holds with forward-arrival forced on ({size} B)")
    except RuntimeError as e:
        fail(f"G2 forward-arrival: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", help="openrar under test (default: auto-detect under build/)")
    ap.add_argument("--baseline", help=f"v{BASELINE_VERSION} release openrar (default: bin/v{BASELINE_VERSION}/)")
    ap.add_argument("--quick", action="store_true", help="m1/m3/m5 only (default: m1-m5)")
    ap.add_argument("--baseline-version", default=BASELINE_VERSION,
                    help="version string the baseline must report (default: %(default)s); "
                         "overridden only to prove the guard fires, e.g. with the roles swapped")
    args = ap.parse_args()

    cur = locate_current(args.binary)
    base = locate_baseline(args.baseline)

    print("=" * 78)
    print("OpenRAR release regression guard (fail-closed)")
    print("=" * 78)
    print(f"binary under test : {cur}")
    print(f"baseline          : {base}")

    for label, path in (("binary under test", cur), ("baseline", base)):
        if not path.is_file():
            print(f"[FATAL] {label} not found: {path}")
            if label == "baseline":
                print(f"        Download the v{BASELINE_VERSION} release binary and pass --baseline,"
                      f" or set OPENRAR_BASELINE_BIN. There is no fallback by design.")
            return 1

    base_ver = version_of(base)
    print(f"baseline version  : {base_ver}")
    if args.baseline_version not in base_ver:
        print(f"[FATAL] baseline reports '{base_ver}', expected {args.baseline_version}")
        return 1
    print(f"new version       : {version_of(cur)}")

    methods = (1, 3, 5) if args.quick else (1, 2, 3, 4, 5)
    zero_methods = (1, 3, 5)

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    corpora = generate_corpora()
    for name in set(SIZE_CORPORA) | {"zeros8.bin", "random8.bin"}:
        print(f"corpus {name:<20}: {corpora[name].stat().st_size:>10,} B  sha256 {sha256_file(corpora[name])[:16]}")

    try:
        check_sizes(cur, base, corpora, methods)
        check_zero_run(cur, base, corpora, zero_methods)
        check_speed(cur, base, corpora, SPEED_CELLS)
        check_determinism(cur, corpora)
        check_alternate_engines(cur, corpora, methods)
    except subprocess.TimeoutExpired as e:
        fail(f"timeout: {e}")
    finally:
        shutil.rmtree(WORK_DIR / "extract", ignore_errors=True)

    print("\n" + "=" * 78)
    if failures:
        print(f"[FATAL] Release regression guard FAILED ({len(failures)} check(s)). Release is blocked.")
        for f in failures:
            print(f"   - {f}")
        return 1
    print("[SUCCESS] No size, speed, integrity or determinism regression against "
          f"v{BASELINE_VERSION}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
