#!/usr/bin/env python3
"""
OpenRAR v2.1.0 Track E - Milestone M0R Census Harness (Schema v2)
Fingerprinted measurement harness providing dual baseline verification:
  - Baseline A: Greedy (same commit) via OPENRAR_NO_FORWARD_ARRIVAL=1
  - Baseline B: v2.0.23 release binary (external reference)
Measures compression size and throughput across -m1, -m3, -m5
on canonical corpora (zeros8, random8, synthetic_text8, mixed16, big24).
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
    if not p_zeros.exists() or p_zeros.stat().st_size != 8 * 1024 * 1024:
        print("Generating zeros8.bin (8 MiB)...")
        with open(p_zeros, "wb") as f:
            f.write(b"\x00" * (8 * 1024 * 1024))
    corpora["zeros8.bin"] = p_zeros

    # B. random8.bin (8 MiB seeded PRNG)
    p_rand = WORK_DIR / "random8.bin"
    if not p_rand.exists() or p_rand.stat().st_size != 8 * 1024 * 1024:
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
    if not p_text.exists() or p_text.stat().st_size < 8 * 1024 * 1024:
        print("Generating synthetic_text8.txt (8 MiB)...")
        rng = random.Random(20261103)
        with open(p_text, "w", encoding="utf-8", newline="\n") as f:
            while f.tell() < 8 * 1024 * 1024:
                line_len = rng.randint(8, 25)
                f.write(" ".join(rng.choice(WORDS) for _ in range(line_len)) + "\n")
    corpora["synthetic_text8.txt"] = p_text

    # D. mixed16.bin (16 MiB: 8 MiB text + 8 MiB random)
    p_mixed = WORK_DIR / "mixed16.bin"
    if not p_mixed.exists() or p_mixed.stat().st_size != 16 * 1024 * 1024:
        print("Generating mixed16.bin (16 MiB)...")
        with open(p_mixed, "wb") as f_out:
            with open(p_text, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
            with open(p_rand, "rb") as f_in:
                f_out.write(f_in.read(8 * 1024 * 1024))
    corpora["mixed16.bin"] = p_mixed

    # E. big24.bin (24 MiB: 8 MiB text + 8 MiB code + 8 MiB binary)
    p_big = WORK_DIR / "big24.bin"
    if not p_big.exists() or p_big.stat().st_size != 24 * 1024 * 1024:
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

    all_identical = True
    cells = []

    print(f"\n{'Corpus':<20} | {'m':<2} | {'Forward (B)':<12} | {'Greedy (same) (B)':<17} | {'v2.0.23 (B)':<12} | {'Δ vs Greedy':<12} | {'Speed':<7} | {'Verdict'}")
    print("-" * 105)

    for corpus_name, corpus_path in corpora.items():
        for m in [1, 3, 5]:
            # Forward arrival, FORCED on: the shipped default differs per method, so an unset
            # environment would measure greedy at m1-m3 and make the column meaningless.
            r_fa = measure_repetitions(bin_path, corpus_path, m,
                                       {"OPENRAR_ENABLE_FORWARD_ARRIVAL": "1",
                                        "OPENRAR_NO_FORWARD_ARRIVAL": ""}, reps=reps)
            # Greedy same-commit (forced env)
            r_greedy = measure_repetitions(bin_path, corpus_path, m, {"OPENRAR_NO_FORWARD_ARRIVAL": "1"}, reps=reps)
            # v2.0.23 release binary (if provided)
            r_v2023 = measure_repetitions(v2023_bin, corpus_path, m, reps=reps) if v2023_bin else None

            if not r_fa or not r_greedy:
                print(f"[FATAL] Benchmark execution failed for {corpus_name} -m{m}")
                sys.exit(1)

            if r_fa["median_size"] != r_greedy["median_size"]:
                all_identical = False

            delta_bytes = r_fa["median_size"] - r_greedy["median_size"]
            pct_delta = (delta_bytes / r_greedy["median_size"]) * 100 if r_greedy["median_size"] > 0 else 0
            speed_ratio = r_fa["median_mb_s"] / r_greedy["median_mb_s"] if r_greedy["median_mb_s"] > 0 else 0

            # Gating verdict
            verdict = "🟢 Pass"
            if corpus_name == "zeros8.bin" and m == 5 and r_fa["median_size"] > 587:
                verdict = "🔴 B-1: zeros5 > 587B"
            elif m != 1 and speed_ratio < 0.90:
                verdict = f"🔴 G3: {speed_ratio:.2f}x < 0.90x"
            elif delta_bytes > 0:
                verdict = f"🔴 Size +{delta_bytes}B"

            v2023_str = f"{r_v2023['median_size']:,}" if r_v2023 else "N/A"
            delta_str = f"{delta_bytes:+,} ({pct_delta:+.2f}%)"

            print(f"{corpus_name:<20} | {m:<2} | {r_fa['median_size']:<12,} | {r_greedy['median_size']:<17,} | {v2023_str:<12} | {delta_str:<12} | {speed_ratio:<6.2f}x | {verdict}")

            cells.append({
                "corpus": corpus_name,
                "method": m,
                "mt": 1,
                "md": "4m",
                "repetitions": reps,
                "forward": r_fa,
                "greedy_same_commit": r_greedy,
                "v2_0_23_release": r_v2023,
                "delta_vs_greedy_bytes": delta_bytes,
                "delta_vs_greedy_pct": pct_delta,
                "speed_ratio_vs_greedy": speed_ratio,
                "verdict": verdict
            })

    # Fail-closed check (NT-13): if all cells are identical, baseline switch failed
    if all_identical:
        print("\n[FATAL NT-13] Forward-arrival and Greedy same-commit columns are byte-identical across ALL cells!")
        print("OPENRAR_NO_FORWARD_ARRIVAL environment variable was not respected by the engine. Aborting fail-closed.")
        sys.exit(1)

    envelope["runs"] = cells
    envelope["forward_arrival"] = {"forced_env": "OPENRAR_ENABLE_FORWARD_ARRIVAL=1", "enabled": True}
    envelope["greedy_same_commit"] = {"forced_env": "OPENRAR_NO_FORWARD_ARRIVAL=1", "enabled": False}
    envelope["v2_0_23_release"] = {"binary": str(v2023_bin) if v2023_bin else "pending", "status": "measured" if v2023_bin else "pending"}

    report_json = ROOT / "docs" / "milestones" / "v2.1.0-m0-census-report.json"
    with open(report_json, "w", encoding="utf-8") as f:
        json.dump(envelope, f, indent=2)

    print(f"\n[OK] Census results successfully saved to {report_json}")

if __name__ == "__main__":
    main()
