#!/usr/bin/env python3
"""
OpenRAR Census & Benchmark Measurement Harness
Measures compression size and throughput across -m1, -m3, -m5
on canonical corpora (zeros8, random8, synthetic_text8, mixed16, big24)
against the v2.0.23 release binary baseline.
"""

import os
import sys
import time
import subprocess
import shutil
import random
import json
import hashlib
import platform
import datetime
from pathlib import Path

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parent.parent
def _default_bin() -> Path:
    """Locate the CLI under test: $OPENRAR_BIN, else the first build layout that exists
    (MSVC: build/openrar64/Release, build/Release; Ninja/Make on Linux and macOS: build/openrar)."""
    env = os.environ.get("OPENRAR_BIN")
    if env:
        return Path(env)
    for rel in ("build/openrar64/Release/openrar.exe", "build/Release/openrar.exe",
                "build/openrar", "build/openrar.exe"):
        if (ROOT / rel).exists():
            return ROOT / rel
    return ROOT / "build" / "openrar64" / "Release" / "openrar.exe"


DEFAULT_BIN = _default_bin()

WORK_DIR = ROOT / "build" / "m0_census"
WORK_DIR.mkdir(parents=True, exist_ok=True)

WORDS = ("the quick brown fox jumps over the lazy dog open rar archive compression "
         "stream window huffman literal distance slot block table symbol length "
         "code bit reader flush buffer sector recovery parity shard cipher salt "
         "iterate vector match chain hash algorithm forward arrival dynamic model").split()

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


# SHA-256 of every synthetic corpus, pinned. generate_corpora() used to trust any file that
# existed at the right size, so a corpus written by an older recipe was measured as if it were
# the current one: mixed16.bin sat on disk from 3 Nov (hash 781087af...) while its recipe, 8 MiB
# of synthetic_text8.txt + 8 MiB of random8.bin, produces f835d53c... Every mixed16 figure
# taken on that machine described a file nobody else could regenerate. A pinned corpus that does
# not match its hash is rebuilt, and a rebuild that still does not match is an error.
# big24.bin additionally depends on tag SOURCE_BLOB_TAG being present (see get_source_blob).
CORPUS_SHA256 = {
    "zeros8.bin": "2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74",
    "random8.bin": "a7255df839d7f16f83fdeb26f48f313f9fbbc65829311aa46df82701a2ad03b0",
    "synthetic_text8.txt": "029ac157f7029f7d24e558deb4779220199602cb2ff03c5e01b4df30cb23d288",
    "mixed16.bin": "f835d53c2426e60f52f6682f610626486d43e6028cd84656faf9ed8570deda45",
    "big24.bin": "4d4e12c9ec52a8ccfa2e4859bd167d5ca0834eb464c5ecc0343840205ed48bdf",
}


def _pinned_ok(path: Path) -> bool:
    """True when `path` exists and matches its pinned SHA-256."""
    return path.exists() and sha256_file(path) == CORPUS_SHA256[path.name]

# The "repository source blob" inside big24.bin is the src/ tree AS OF THIS TAG, not the
# working tree. Reading the working tree made the corpus change whenever the code under
# test changed (the encoder is part of src/), so a before/after size comparison was
# measuring against a moving target and two machines could never agree on big24.bin.
SOURCE_BLOB_TAG = "v2.0.23"


def _git(*args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=ROOT, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def get_source_blob() -> str:
    exts = (".cpp", ".hpp", ".h")
    blob = []
    try:
        names = sorted(n for n in _git("ls-tree", "-r", "--name-only", SOURCE_BLOB_TAG, "src")
                       .decode("utf-8").splitlines() if n.endswith(exts))
        for name in names:
            blob.append(_git("show", f"{SOURCE_BLOB_TAG}:{name}").decode("utf-8", errors="replace"))
    except (OSError, subprocess.CalledProcessError):
        blob = []
    if blob:
        return "\n".join(blob)

    print(f"WARNING: tag {SOURCE_BLOB_TAG} is not available (shallow clone or no git); "
          f"building big24.bin from the working tree. It will NOT match other machines.",
          file=sys.stderr)
    for root, dirs, files in os.walk(ROOT / "src"):
        dirs.sort()
        for name in sorted(files):
            if name.endswith(exts):
                try:
                    blob.append((Path(root) / name).read_text(encoding="utf-8", errors="replace"))
                except OSError:
                    pass
    return "\n".join(blob) if blob else "int main() { return 0; }\n"

def generate_corpora() -> dict:
    corpora = {}
    
    # A. zeros8.bin (8 MiB)
    p_zeros = WORK_DIR / "zeros8.bin"
    if not _pinned_ok(p_zeros):
        print("Generating zeros8.bin (8 MiB)...")
        with open(p_zeros, "wb") as f:
            f.write(b"\x00" * (8 * 1024 * 1024))
    corpora["zeros8.bin"] = p_zeros

    # B. random8.bin (8 MiB seeded PRNG)
    p_rand = WORK_DIR / "random8.bin"
    if not _pinned_ok(p_rand):
        print("Generating random8.bin (8 MiB)...")
        rng = random.Random(20261103)
        with open(p_rand, "wb") as f:
            remaining = 8 * 1024 * 1024
            while remaining > 0:
                chunk = bytes(rng.getrandbits(8) for _ in range(min(1 << 20, remaining)))
                f.write(chunk)
                remaining -= len(chunk)
    corpora["random8.bin"] = p_rand

    # C. synthetic_text8.txt (8 MiB word salad, renamed from text8.txt per M0R-2)
    p_text = WORK_DIR / "synthetic_text8.txt"
    if not _pinned_ok(p_text):
        print("Generating synthetic_text8.txt (8 MiB)...")
        rng = random.Random(20261103)
        with open(p_text, "w", encoding="utf-8", newline="\n") as f:
            while f.tell() < 8 * 1024 * 1024:
                line_len = rng.randint(8, 25)
                f.write(" ".join(rng.choice(WORDS) for _ in range(line_len)) + "\n")
    corpora["synthetic_text8.txt"] = p_text

    # D. mixed16.bin (16 MiB: 8 MiB text + 8 MiB random)
    p_mixed = WORK_DIR / "mixed16.bin"
    if not _pinned_ok(p_mixed):
        print("Generating mixed16.bin (16 MiB)...")
        with open(p_mixed, "wb") as f_out:
            with open(p_text, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
            with open(p_rand, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
    corpora["mixed16.bin"] = p_mixed

    # E. big24.bin (24 MiB: 8 MiB text + 8 MiB code + 8 MiB binary)
    p_big = WORK_DIR / "big24.bin"
    if not _pinned_ok(p_big):
        print("Generating big24.bin (24 MiB)...")
        src_blob = get_source_blob().encode("utf-8", errors="replace")
        with open(p_big, "wb") as f_out:
            with open(p_text, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
            # Write 8 MiB code blob
            written = 0
            while written < 8 * 1024 * 1024:
                take = min(len(src_blob), (8 * 1024 * 1024) - written)
                f_out.write(src_blob[:take])
                written += take
            with open(p_rand, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
    corpora["big24.bin"] = p_big

    for name, path in corpora.items():
        got = sha256_file(path)
        if got != CORPUS_SHA256[name]:
            raise RuntimeError(
                f"corpus {name} does not match its pinned SHA-256 after regeneration "
                f"(got {got[:16]}..., want {CORPUS_SHA256[name][:16]}...). For big24.bin this "
                f"usually means tag {SOURCE_BLOB_TAG} is missing; fetch tags and retry.")
    return corpora

def get_system_fingerprint(corpora: dict) -> dict:
    git_sha = "unknown"
    dirty = False
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True)
        if r.returncode == 0:
            git_sha = r.stdout.strip()
        r_stat = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True)
        if r_stat.returncode == 0 and r_stat.stdout.strip():
            dirty = True
    except Exception:
        pass

    corpus_meta = []
    for name, path in corpora.items():
        corpus_meta.append({
            "name": name,
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path)
        })

    return {
        "schema_version": 2,
        "git_sha": git_sha,
        "dirty": dirty,
        "build_type": "Release",
        "compiler": "MSVC / Clang",
        "cpu": platform.processor() or platform.machine(),
        "os": f"{platform.system()} {platform.release()} ({platform.version()})",
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "corpora": corpus_meta
    }

def run_single(binary: Path, input_path: Path, method: int, env_override: dict = None) -> dict:
    arc_path = WORK_DIR / f"temp_{method}_{random.randint(1000, 9999)}.rar"
    if arc_path.exists():
        arc_path.unlink()

    cmd = [str(binary), "a", "-y", f"-m{method}", "-mt1", "-md4m", str(arc_path), str(input_path)]
    env = os.environ.copy()
    if env_override:
        env.update(env_override)

    t0 = time.perf_counter()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    dur = time.perf_counter() - t0

    if res.returncode != 0:
        print(f"Error compressing {input_path} at -m{method}: {res.stderr.decode(errors='replace')}")
        return None

    arc_size = arc_path.stat().st_size
    in_size = input_path.stat().st_size
    mb_s = (in_size / (1024 * 1024)) / dur if dur > 0 else 0

    # Integrity verification
    test_res = subprocess.run([str(binary), "t", "-y", str(arc_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if test_res.returncode != 0:
        print(f"FAILED integrity test on {arc_path}")
        if arc_path.exists():
            arc_path.unlink()
        return None

    if arc_path.exists():
        arc_path.unlink()

    return {"size": arc_size, "time_s": dur, "mb_s": mb_s}

def measure_repetitions(binary: Path, input_path: Path, method: int, env_override: dict = None, reps: int = 5) -> dict:
    sizes = []
    speeds = []
    times = []
    for _ in range(reps):
        m = run_single(binary, input_path, method, env_override)
        if not m:
            return None
        sizes.append(m["size"])
        speeds.append(m["mb_s"])
        times.append(m["time_s"])

    sizes.sort()
    speeds.sort()
    times.sort()

    mid = reps // 2
    median_size = sizes[mid]
    median_speed = speeds[mid]
    p10_size = sizes[max(0, int(reps * 0.1))]
    p90_size = sizes[min(reps - 1, int(reps * 0.9))]
    p10_speed = speeds[max(0, int(reps * 0.1))]
    p90_speed = speeds[min(reps - 1, int(reps * 0.9))]

    return {
        "median_size": median_size,
        "p10_size": p10_size,
        "p90_size": p90_size,
        "median_mb_s": median_speed,
        "p10_mb_s": p10_speed,
        "p90_mb_s": p90_speed,
        "orig_size": input_path.stat().st_size
    }

def main():
    import argparse
    parser = argparse.ArgumentParser(description="OpenRAR Milestone M0R Census Harness (Schema v2)")
    parser.add_argument("--bin", default=str(DEFAULT_BIN), help="Path to openrar executable under test")
    parser.add_argument("--v2023-bin", default=None, help="Path to v2.0.23 release binary (if available)")
    parser.add_argument("--quick", action="store_true", help="Quick mode (1 repetition per cell)")
    parser.add_argument("--reps", type=int, default=5, help="Number of repetitions (default: 5)")
    args = parser.parse_args()

    bin_path = Path(args.bin)
    if not bin_path.exists():
        print(f"Error: binary under test not found at {bin_path}")
        sys.exit(1)

    v2023_bin = Path(args.v2023_bin) if args.v2023_bin else None
    if not v2023_bin:
        cand = ROOT / "bin" / "v2.0.23" / "openrar.exe"
        if cand.exists():
            v2023_bin = cand

    reps = 1 if args.quick else args.reps

    print("\n" + "=" * 90)
    print("OpenRAR v2.1.0 Track E: Baseline Census Harness (Phase M0R - Schema v2)")
    print("=" * 90)
    print(f"Binary under test:  {bin_path}")
    print(f"v2.0.23 release bin: {v2023_bin or 'None (External Reference Pending M0R-6)'}")
    print(f"Repetitions / cell:  {reps}")
    print(f"Host Date / Time:   {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("-" * 90)

    corpora = generate_corpora()
    envelope = get_system_fingerprint(corpora)

    cells = []

    print(f"\n{'Corpus':<20} | {'m':<2} | {'Current (B)':<12} | {'v2.0.23 (B)':<12} | {'Δ vs v2.0.23':<15} | {'Speed (MB/s)':<12} | {'Verdict'}")
    print("-" * 95)

    for corpus_name, corpus_path in corpora.items():
        for m in [1, 3, 5]:
            r_cur = measure_repetitions(bin_path, corpus_path, m, reps=reps)
            r_v2023 = measure_repetitions(v2023_bin, corpus_path, m, reps=reps) if v2023_bin else None

            if not r_cur:
                print(f"[FATAL] Benchmark execution failed for {corpus_name} -m{m}")
                sys.exit(1)

            delta_bytes = (r_cur["median_size"] - r_v2023["median_size"]) if r_v2023 else 0
            pct_delta = (delta_bytes / r_v2023["median_size"]) * 100 if (r_v2023 and r_v2023["median_size"] > 0) else 0
            speed_ratio = (r_cur["median_mb_s"] / r_v2023["median_mb_s"]) if (r_v2023 and r_v2023["median_mb_s"] > 0) else 0

            # Gating verdict
            verdict = "🟢 Pass"
            if corpus_name == "zeros8.bin" and m == 5 and r_cur["median_size"] > 587:
                verdict = "🔴 B-1: zeros5 > 587B"
            elif r_v2023 and delta_bytes > 0:
                verdict = f"🔴 Size +{delta_bytes}B"

            v2023_str = f"{r_v2023['median_size']:,}" if r_v2023 else "N/A"
            delta_str = f"{delta_bytes:+,} ({pct_delta:+.2f}%)" if r_v2023 else "N/A"

            print(f"{corpus_name:<20} | {m:<2} | {r_cur['median_size']:<12,} | {v2023_str:<12} | {delta_str:<15} | {r_cur['median_mb_s']:<10.2f}   | {verdict}")

            cells.append({
                "corpus": corpus_name,
                "method": m,
                "mt": 1,
                "md": "4m",
                "repetitions": reps,
                "current": r_cur,
                "v2_0_23_release": r_v2023,
                "delta_vs_v2023_bytes": delta_bytes,
                "delta_vs_v2023_pct": pct_delta,
                "speed_ratio_vs_v2023": speed_ratio,
                "verdict": verdict
            })

    envelope["runs"] = cells
    envelope["current_binary"] = str(bin_path)
    envelope["v2_0_23_release"] = {"binary": str(v2023_bin) if v2023_bin else "pending", "status": "measured" if v2023_bin else "pending"}

    report_json = ROOT / "docs" / "milestones" / "v2.1.0-m0-census-report.json"
    with open(report_json, "w", encoding="utf-8") as f:
        json.dump(envelope, f, indent=2)

    print(f"\n[OK] Census results successfully saved to {report_json}")

if __name__ == "__main__":
    main()
