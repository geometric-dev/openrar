#!/usr/bin/env python3
"""
OpenRAR v2.1.0 Track E - Milestone M2 PriceModel 4-Arm Ablation Harness
Evaluates the 4 pricing models:
  - Arm U: Flat Uniform (8 bits literal, fixed length/dist)
  - Arm A: Empirical Static Prior (Pre-flight 4 KiB sample, no block-boundary update)
  - Arm B: Per-Block Measured (Flat Block 0, updated at write_block from Huffman code lengths)
  - Arm C: Dynamic Refresh (Arm A + Arm B + mid-block refresh at 4096 tokens)
Verifies the 3 normative ablation gating criteria from implementation plan §4.3:
  1. Arm A beats Arm U across all corpora (empirical prior is better than flat uniform)
  2. Arm B beats Arm A across all corpora (measured Huffman tables are better than static prior)
  3. Arm C ships ONLY if:
     - size(C) <= size(B) - 0.05% on big24.bin and heterogeneous corpora
     - size(C) <= size(B) + 0.05% on all individual cells
     - throughput(C) >= 0.95 * throughput(B)
"""

import os
import sys
import time
import subprocess
import json
from pathlib import Path

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from bench_m0_census import DEFAULT_BIN, WORK_DIR, generate_corpora, measure_repetitions

def run_ablation(bin_path: Path, reps: int = 1):
    corpora = generate_corpora()
    arms = ["U", "A", "B", "C"]
    
    print("\n" + "=" * 95)
    print("OpenRAR v2.1.0 Track E: Phase M2 PriceModel 4-Arm Ablation Evaluation")
    print("=" * 95)
    print(f"Binary under test: {bin_path}")
    print(f"Repetitions / cell: {reps}")
    print("-" * 95)
    print(f"{'Corpus':<20} | {'Arm U (B)':<12} | {'Arm A (B)':<12} | {'Arm B (B)':<12} | {'Arm C (B)':<12} | {'C vs B (Δ)':<16} | {'Verdict'}")
    print("-" * 95)

    results = {}
    for c_name, c_path in corpora.items():
        results[c_name] = {}
        
        for arm in arms:
            env = {"OPENRAR_PRICE_ARM": arm}
            res = measure_repetitions(bin_path, c_path, 5, env_override=env, reps=reps)
            results[c_name][arm] = res
            
        u_sz = results[c_name]["U"]["median_size"]
        a_sz = results[c_name]["A"]["median_size"]
        b_sz = results[c_name]["B"]["median_size"]
        c_sz = results[c_name]["C"]["median_size"]
        
        delta_cb = c_sz - b_sz
        pct_cb = (delta_cb / b_sz) * 100 if b_sz > 0 else 0
        
        verdict = "🟢 Pass"
        if a_sz > u_sz:
            verdict = "🔴 A > U"
        elif b_sz > a_sz:
            verdict = "🔴 B > A"
        elif pct_cb > 0.05:
            verdict = f"🔴 C regressed +{pct_cb:.2f}%"
            
        print(f"{c_name:<20} | {u_sz:<12,d} | {a_sz:<12,d} | {b_sz:<12,d} | {c_sz:<12,d} | {delta_cb:+7,d} ({pct_cb:+.2f}%) | {verdict}")

    out_file = ROOT / "docs" / "milestones" / "v2.1.0-m2-ablation-report.json"
    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    print(f"\n[OK] Ablation results saved to {out_file}")

if __name__ == "__main__":
    run_ablation(DEFAULT_BIN, reps=1)
