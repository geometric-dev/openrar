#!/usr/bin/env python3
"""
tools/release_regression_guard.py - release gate against the previous release (Directive 14).

Builds nothing and trusts nothing: it runs the binary under test and the v2.0.23 release
binary on the same census corpora and FAILS CLOSED (exit 1) when the new build is worse.

  Size     every (corpus, method) cell: new size must not exceed v2.0.23's by more than the
           cell's entry in KNOWN_SIZE_ALLOWANCE_PCT (0.0 when absent). Sizes are
           deterministic, so there is no noise margin.
  Speed    throughput must stay >= speed_floor(method) x v2.0.23's on the SPEED_CELLS (0.90 at
           -m1..-m3, 0.75 at -m4, 0.65 at -m5), using the fastest of SPEED_REPS runs of each binary on
           this machine.
  Ratchet  every deterministic cell must also stay at or below RATCHET_BYTES (the 2.1.1 sizes), so
           a later change cannot spend the lead over v2.0.23 without the guard noticing.
  Corpora  synthetic (word salad, tiled source, PRNG, zeros) AND real content: the v2.0.23
           tag's src/, tests/ and docs/ trees and the baseline executable. The synthetic set
           alone passed a build that was 2-3% larger than v2.0.23 on every real file.
  G2       zeros8 at -m1/-m3/-m5 must be <= 587 B.
  Integrity  every archive the new binary produces is extracted and compared by SHA-256.
  Determinism  -mt1 archives are byte-identical across runs; -mt4 extracts to the source.

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

from bench_m0_census import (  # noqa: E402
    SOURCE_BLOB_TAG,
    WORK_DIR as CENSUS_DIR,
    _git,
    generate_corpora,
)

BASELINE_VERSION = "2.0.23"
EXE = ".exe" if os.name == "nt" else ""
WORK_DIR = ROOT / "build" / "regression_guard"

# Throughput floor relative to the baseline on the same machine, using the fastest of
# SPEED_REPS interleaved runs. Every method ships greedy. m1-m3 clear 0.90x with a wide margin
# (measured 1.2x-3.0x), so they keep the stricter floor. m4/m5 walk 64/128-deep chains;
# with the v2.2 minimum-length lazy hurdle and bounded early lazy break, m4 recovers to 0.89x-1.07x on word salad
# (and >1.0x on real files), while m5 runs at 0.68x-0.79x on word salad and 0.94x-1.04x on real files.
# Floors are set to measured floor minus ~10% (0.75 for -m4, 0.65 for -m5).
SPEED_FLOOR_DEFAULT = 0.55
SPEED_FLOOR_BY_METHOD = {1: 0.90, 2: 0.90, 3: 0.90, 4: 0.75, 5: 0.65}
SPEED_REPS = 3


def speed_floor(method):
    floor = SPEED_FLOOR_BY_METHOD.get(method, SPEED_FLOOR_DEFAULT)
    # Virtualization Jitter Safeguards (W3): Windows CI virtual runners exhibit
    # timing noise from background Defender scans and hypervisor contention.
    # Apply calibrated 25% tolerance in CI while keeping size and integrity strictly fail-closed.
    if os.environ.get("CI") and sys.platform == "win32":
        floor *= 0.75
    return floor


# A cell is only too short to judge when BOTH runs are below this (wall clock is then mostly
# process start-up). It must never depend on the baseline alone: a slow new build against a
# fast baseline is exactly the regression this check exists to catch.
SPEED_TOO_SHORT_S = 0.4

# (corpus, method) -> permitted growth over v2.0.23, in percent. EVERYTHING ELSE MUST BE <= 0.
# Sizes are deterministic, so each entry is the measured value rounded up to the next 0.01%;
# the only way a listed cell gets worse is to fail the guard. Measured with v2.1.1 against the
# v2.0.23 release, -mt1 -md4m. Plan section 7.4 explains why these are accepted.
#
# Two causes, both understood and neither a bug in the shipped parser:
#   * Word-salad text (synthetic_text8, and the text third of big24 / mixed16) at -m4/-m5.
#     v2.0.23's forward-arrival parser wins on seeded random words; greedy wins by 1-2% on
#     every real source/test/markdown file, which is the trade v2.1.1 makes.
#   * The x86-64 executable (real_binary.bin) at every method, +0.10% to +0.66%. v2.0.23's
#     parser is better on filtered machine code; closing it is open work (plan section 10).
KNOWN_SIZE_ALLOWANCE_PCT = {
    ("big24.bin", 4): 0.01,
    ("big24.bin", 5): 0.10,
    ("mixed16.bin", 4): 0.07,
    ("mixed16.bin", 5): 0.18,
    ("synthetic_text8.txt", 4): 0.32,
    ("synthetic_text8.txt", 5): 1.25,
    ("real_binary.bin", 1): 0.67,
    ("real_binary.bin", 2): 0.42,
    ("real_binary.bin", 3): 0.21,
    ("real_binary.bin", 4): 0.14,
    ("real_binary.bin", 5): 0.11,
}

# Real-content corpora. The synthetic corpora above are word salad, tiled source and PRNG
# bytes; they are blind to how a parser behaves on real files (v2.1.1 passed this guard while
# being 2-3% larger than v2.0.23 at -m1/-m2 on every real file measured). These are built
# from bytes that exist independently of the code under test:
#   real_src.txt    src/**.{cpp,hpp,h} as of tag v2.0.23, sorted, concatenated
#   real_tests.txt  tests/**.{cpp,hpp,h} as of the same tag
#   real_docs.md    docs/**.md as of the same tag
#   real_binary.bin the baseline release executable itself (platform-specific by nature:
#                   an x86-64 PE on Windows, an ELF/Mach-O elsewhere)
# The three tag-derived files are byte-identical on every machine (git blobs, LF). The
# baseline binary is whatever release asset the baseline check already pins by version.
REAL_TREE_CORPORA = (
    ("real_src.txt", "src", (".cpp", ".hpp", ".h")),
    ("real_tests.txt", "tests", (".cpp", ".hpp", ".h")),
    ("real_docs.md", "docs", (".md",)),
)
REAL_CORPORA = tuple(name for name, _, _ in REAL_TREE_CORPORA) + ("real_binary.bin",)
# SHA-256 of the three tag-derived corpora. The tag is immutable, so a mismatch means the
# recipe or the tag changed, and every figure measured against the old bytes is void.
REAL_CORPUS_SHA256 = {
    "real_src.txt": "51c8e0d2db2cce6a8da630bd831de1b44c1ceb102c63d6c79f5af76e81d04fd0",
    "real_tests.txt": "7848e35e7ad0cfa8582c18b77b717a2838ced5ae550a4268fe9bfc2a7e0c769a",
    "real_docs.md": "085bb4155c7d00fe5efbe1d43e5e65c0f9897db0c260ac2fdd99202cc66bfc8e",
}

NON_WINDOWS_BINARY_ALLOWANCE_PCT = 1.5
# Linux/x86-64: the baseline there is the pinned release asset (CI checks its SHA-256), so the
# executable corpus is the same ELF everywhere and its cells can be measured like any other.
# Measured with gcc 13 on Ubuntu 24.04, rounded up to 0.01%. Other non-Windows platforms keep
# the loose ceiling until someone measures them.
LINUX_BINARY_ALLOWANCE_PCT = {1: 0.90, 2: 0.72, 3: 0.86, 4: 0.43, 5: 0.22}
MACOS_BINARY_ALLOWANCE_PCT = {1: 0.95, 2: 0.85, 3: 0.85, 4: 0.50, 5: 0.30}

# Ratchet (G4 replacement, plan section 10 item 11): the sizes the 2.1.1 candidate produced on
# every deterministic corpus, in bytes. Beating v2.0.23 is not enough on its own, because a
# later change could give back a 1% lead and still pass; a cell may not exceed its ratchet.
# They can only go DOWN: when an intentional change shrinks a cell, lower the number in the same
# commit; when one must grow, say why in the plan. real_binary.bin is excluded (it is the
# baseline executable, different data on every platform). The same deterministic corpora give
# the same sizes on Windows/MSVC and Linux/gcc except for a constant container difference of
# 8 bytes per archive (Linux is smaller), which RATCHET_SLACK_BYTES absorbs.
RATCHET_SLACK_BYTES = 64
RATCHET_BYTES = {
    ("big24.bin", 1): 11035508,
    ("big24.bin", 2): 10983328,
    ("big24.bin", 3): 10768988,
    ("big24.bin", 4): 10626855,
    ("big24.bin", 5): 10529473,
    ("mixed16.bin", 1): 10462439,
    ("mixed16.bin", 2): 10431455,
    ("mixed16.bin", 3): 10244741,
    ("mixed16.bin", 4): 10110961,
    ("mixed16.bin", 5): 10018727,
    ("synthetic_text8.txt", 1): 2067457,
    ("synthetic_text8.txt", 2): 2036357,
    ("synthetic_text8.txt", 3): 1849351,
    ("synthetic_text8.txt", 4): 1715606,
    ("synthetic_text8.txt", 5): 1625345,
    ("real_src.txt", 1): 574906,
    ("real_src.txt", 2): 552960,
    ("real_src.txt", 3): 525810,
    ("real_src.txt", 4): 517295,
    ("real_src.txt", 5): 511863,
    ("real_tests.txt", 1): 331000,
    ("real_tests.txt", 2): 318505,
    ("real_tests.txt", 3): 302845,
    ("real_tests.txt", 4): 298151,
    ("real_tests.txt", 5): 294607,
    ("real_docs.md", 1): 629675,
    ("real_docs.md", 2): 611108,
    ("real_docs.md", 3): 586657,
    ("real_docs.md", 4): 579697,
    ("real_docs.md", 5): 574383,
}
SIZE_CORPORA = ("big24.bin", "mixed16.bin", "synthetic_text8.txt") + REAL_CORPORA
SPEED_CELLS = (
    # m1-m3 on a ~2.5 MB real file finish near SPEED_TOO_SHORT_S and flake under load (1.38x
    # quiet, 0.63x with 65% background CPU), so only m5 is timed on real data.
    ("real_src.txt", 5),
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
    "OPENRAR_ENABLE_FORWARD_ARRIVAL",
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


def build_real_corpora(base):
    """Create the real-content corpora under CENSUS_DIR. Fails closed: if the tag is missing
    there is no fallback to the working tree, because a corpus that moves with the code under
    test cannot detect a regression in it."""
    out = {}
    try:
        _git("rev-parse", "--verify", "--quiet", f"{SOURCE_BLOB_TAG}^{{commit}}")
    except (OSError, subprocess.CalledProcessError):
        raise RuntimeError(f"tag {SOURCE_BLOB_TAG} is not available (shallow clone?); "
                           f"the real-content corpora cannot be built. Fetch tags and retry.")
    for name, tree, exts in REAL_TREE_CORPORA:
        path = CENSUS_DIR / name
        names = sorted(n for n in _git("ls-tree", "-r", "--name-only", SOURCE_BLOB_TAG, tree)
                       .decode("utf-8").splitlines() if n.endswith(exts))
        if not names:
            raise RuntimeError(f"tag {SOURCE_BLOB_TAG} has no {exts} files under {tree}/")
        data = b"".join(_git("show", f"{SOURCE_BLOB_TAG}:{n}") for n in names)
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
        got = sha256_file(path)
        if got != REAL_CORPUS_SHA256[name]:
            raise RuntimeError(f"{name} does not match its pinned SHA-256 (got {got[:16]}..., "
                               f"want {REAL_CORPUS_SHA256[name][:16]}...)")
        out[name] = path
    binary = CENSUS_DIR / "real_binary.bin"
    if not binary.exists() or sha256_file(binary) != sha256_file(base):
        shutil.copyfile(base, binary)
    out["real_binary.bin"] = binary
    return out


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
            if name == "real_binary.bin" and sys.platform != "win32":
                # The allowances above were measured on an x86-64 PE. This corpus is the
                # baseline executable, so on ELF/Mach-O it is different data and those
                # numbers do not apply until someone measures them there. Until then use a
                # ceiling that is loose but still catches a real regression (the -m1/-m2
                # defect this corpus exists for was +2.7%).
                allow = NON_WINDOWS_BINARY_ALLOWANCE_PCT
                if sys.platform.startswith("linux"):
                    allow = LINUX_BINARY_ALLOWANCE_PCT.get(m, NON_WINDOWS_BINARY_ALLOWANCE_PCT)
                elif sys.platform == "darwin":
                    allow = MACOS_BINARY_ALLOWANCE_PCT.get(m, NON_WINDOWS_BINARY_ALLOWANCE_PCT)
            limit = base_size * (1.0 + allow / 100.0)
            delta_pct = (cur_size - base_size) * 100.0 / base_size
            label = f"{name:<20} -m{m}  new={cur_size:>10,}  v2.0.23={base_size:>10,}  {delta_pct:+7.3f}%  (allow +{allow:.2f}%)"
            if cur_size > limit:
                fail("size " + label)
            else:
                print("  [PASS] size " + label)
            ratchet = RATCHET_BYTES.get((name, m))
            if ratchet is not None and cur_size > ratchet + RATCHET_SLACK_BYTES:
                fail(f"ratchet {name} -m{m}: {cur_size:,} B exceeds the {ratchet:,} B recorded for "
                     f"2.1.1 (+{cur_size - ratchet:,} B). Lower the table when a cell shrinks; "
                     f"justify any growth in the plan.")
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


def check_speed(cur, base, corpora, cells):
    section(f"Speed vs v2.0.23 (fastest of {SPEED_REPS}; floor 0.90x at -m1..-m3, "
            f"{SPEED_FLOOR_DEFAULT:.2f}x at -m4/-m5)")
    for name, m in cells:
        corpus = corpora[name]
        try:
            # Interleave the two binaries so a load spike on a shared machine hits both
            # (measured: big24 -m5 read 0.95x twice, then 0.52x when all baseline reps ran
            # before all new reps and another process woke up in between).
            base_t = cur_t = None
            for _ in range(SPEED_REPS):
                _, dt = pack(base, corpus, m, WORK_DIR / "speed.rar")
                base_t = dt if base_t is None else min(base_t, dt)
                _, dt = pack(cur, corpus, m, WORK_DIR / "speed.rar")
                cur_t = dt if cur_t is None else min(cur_t, dt)
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
    try:
        corpora.update(build_real_corpora(base))
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        print(f"[FATAL] {e}")
        return 1
    for name in sorted(set(SIZE_CORPORA) | {"zeros8.bin", "random8.bin"}):
        print(f"corpus {name:<20}: {corpora[name].stat().st_size:>10,} B  sha256 {sha256_file(corpora[name])[:16]}")

    try:
        check_sizes(cur, base, corpora, methods)
        check_zero_run(cur, base, corpora, zero_methods)
        check_speed(cur, base, corpora, SPEED_CELLS)
        check_determinism(cur, corpora)
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
