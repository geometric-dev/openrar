#!/usr/bin/env python3
"""
OpenRAR v2.1.0 Track E - Milestone M0 Census Harness
Measures compression size and throughput across -m1, -m3, -m5
on canonical corpora (zeros, random, text, mixed/big24).
"""

import os
import sys
import time
import subprocess
import shutil
import random
import json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "openrar64", "Release", "openrar.exe")
if not os.path.exists(BIN):
    BIN = os.path.join(ROOT, "build", "Release", "openrar.exe")
if not os.path.exists(BIN):
    print(f"Error: openrar binary not found at {BIN}")
    sys.exit(1)

WORK_DIR = os.path.join(ROOT, "build", "m0_census")
os.makedirs(WORK_DIR, exist_ok=True)

# 1. Generate Corpora
CORPORA = {}

# A. zeros8.bin (8 MiB of zeros)
p_zeros = os.path.join(WORK_DIR, "zeros8.bin")
if not os.path.exists(p_zeros):
    print("Generating zeros8.bin (8 MiB)...")
    with open(p_zeros, "wb") as f:
        f.write(b"\x00" * (8 * 1024 * 1024))
CORPORA["zeros8.bin"] = p_zeros

# B. random8.bin (8 MiB of high-entropy pseudo-random bytes)
p_rand = os.path.join(WORK_DIR, "random8.bin")
if not os.path.exists(p_rand):
    print("Generating random8.bin (8 MiB)...")
    rng = random.Random(20261103)
    with open(p_rand, "wb") as f:
        remaining = 8 * 1024 * 1024
        while remaining > 0:
            chunk = bytes(rng.getrandbits(8) for _ in range(min(1 << 20, remaining)))
            f.write(chunk)
            remaining -= len(chunk)
CORPORA["random8.bin"] = p_rand

# C. text8.txt (8 MiB of repetitive/structured English text)
p_text = os.path.join(WORK_DIR, "text8.txt")
if not os.path.exists(p_text):
    print("Generating text8.txt (8 MiB)...")
    WORDS = ("the quick brown fox jumps over the lazy dog open rar archive compression "
             "stream window huffman literal distance slot block table symbol length "
             "code bit reader flush buffer sector recovery parity shard cipher salt "
             "iterate vector match chain hash algorithm forward arrival dynamic model").split()
    rng = random.Random(20261103)
    with open(p_text, "w", encoding="utf-8") as f:
        while f.tell() < 8 * 1024 * 1024:
            line_len = rng.randint(8, 25)
            f.write(" ".join(rng.choice(WORDS) for _ in range(line_len)) + "\n")
CORPORA["text8.txt"] = p_text

# D. mixed16.bin (16 MiB: 8 MiB code/text + 8 MiB binary)
p_mixed = os.path.join(WORK_DIR, "mixed16.bin")
if not os.path.exists(p_mixed):
    print("Generating mixed16.bin (16 MiB)...")
    with open(p_mixed, "wb") as f_out:
        with open(p_text, "rb") as f_in:
            f_out.write(f_in.read(8 * 1024 * 1024))
        with open(p_rand, "rb") as f_in:
            f_out.write(f_in.read(8 * 1024 * 1024))
CORPORA["mixed16.bin"] = p_mixed

def measure(input_path, method, env_override=None):
    arc_path = os.path.join(WORK_DIR, "temp.rar")
    if os.path.exists(arc_path):
        os.remove(arc_path)
    
    cmd = [BIN, "a", "-y", f"-m{method}", "-mt1", "-md4m", arc_path, input_path]
    env = os.environ.copy()
    if env_override:
        env.update(env_override)
    
    t0 = time.perf_counter()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    t1 = time.perf_counter()
    
    if res.returncode != 0:
        print(f"Error compressing {input_path}: {res.stderr.decode(errors='replace')}")
        return None
    
    arc_size = os.path.getsize(arc_path)
    dur = t1 - t0
    in_size = os.path.getsize(input_path)
    mb_s = (in_size / (1024 * 1024)) / dur if dur > 0 else 0
    
    # Test archive integrity
    test_res = subprocess.run([BIN, "t", "-y", arc_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if test_res.returncode != 0:
        print(f"FAILED integrity test on {arc_path}")
        return None
    
    if os.path.exists(arc_path):
        os.remove(arc_path)
    
    return {"size": arc_size, "time_s": dur, "mb_s": mb_s, "orig_size": in_size}

print("\n" + "=" * 80)
print("OpenRAR v2.1.0 Track E: Baseline Census (M0)")
print("=" * 80)
print(f"Binary: {BIN}")
print(f"Host Date: {time.strftime('%Y-%m-%d %H:%M:%S')}")
print("-" * 80)

results = {}

for corpus_name, corpus_path in CORPORA.items():
    results[corpus_name] = {}
    print(f"\nEvaluating: {corpus_name} ({os.path.getsize(corpus_path):,} bytes)")
    print(f"{'Method':<8} | {'Mode':<18} | {'Compressed (B)':<16} | {'Ratio':<10} | {'Speed (MB/s)':<12} | {'Time (s)':<10}")
    print("-" * 80)
    for m in [1, 3, 5]:
        # Mode 1: Default (Forward-Arrival Engine Active)
        r_fa = measure(corpus_path, m)
        # Mode 2: Legacy Greedy Baseline (OPENRAR_NO_FORWARD_ARRIVAL=1)
        r_no_fa = measure(corpus_path, m, {"OPENRAR_NO_FORWARD_ARRIVAL": "1"})
        
        ratio_fa = (r_fa["size"] / r_fa["orig_size"]) * 100
        ratio_no_fa = (r_no_fa["size"] / r_no_fa["orig_size"]) * 100
        
        delta_bytes = r_fa["size"] - r_no_fa["size"]
        pct_delta = ((r_fa["size"] - r_no_fa["size"]) / r_no_fa["size"]) * 100
        
        print(f"-m{m:<6} | {'Forward-Arrival':<18} | {r_fa['size']:<16,} | {ratio_fa:6.2f}%   | {r_fa['mb_s']:8.2f}     | {r_fa['time_s']:6.3f}s")
        print(f"-m{m:<6} | {'Greedy (Legacy)':<18} | {r_no_fa['size']:<16,} | {ratio_no_fa:6.2f}%   | {r_no_fa['mb_s']:8.2f}     | {r_no_fa['time_s']:6.3f}s")
        print(f"       -> Delta: {delta_bytes:+,} bytes ({pct_delta:+.2f}%) | Speed ratio: {r_fa['mb_s']/r_no_fa['mb_s']:.2f}x")
        print("-" * 80)
        
        results[corpus_name][f"m{m}"] = {
            "forward_arrival": r_fa,
            "greedy_legacy": r_no_fa,
            "delta_bytes": delta_bytes,
            "pct_delta": pct_delta
        }

report_json = os.path.join(ROOT, "docs", "milestones", "v2.1.0-m0-census-report.json")
with open(report_json, "w", encoding="utf-8") as f:
    json.dump(results, f, indent=2)

print(f"\nCensus results saved to {report_json}")

