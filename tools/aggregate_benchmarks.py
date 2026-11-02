#!/usr/bin/env python3
"""
OpenRAR Hardware Benchmark Aggregator & Leaderboard Generator
Scans benchmarks/ci/*.json and benchmarks/submissions/*.json, validates
cryptographic decompression integrity, ranks hardware by RARMark v2.0 score,
and generates BENCHMARKS.md dynamically.

Usage:
  python tools/aggregate_benchmarks.py [--check] [--output BENCHMARKS.md]
"""

import argparse
import glob
import json
import os
import sys
from pathlib import Path


def load_and_validate(json_path):
    try:
        with open(json_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Warning: Failed to parse {json_path}: {e}", file=sys.stderr)
        return None

    if data.get("benchmark") != "openrar_rarmark_v2":
        print(f"Warning: {json_path} is not an openrar_rarmark_v2 benchmark.", file=sys.stderr)
        return None

    cpu = data.get("cpu", {})
    results = data.get("results", {})

    if not results.get("decompression_verified", False):
        print(f"Warning: {json_path} failed decompression verification (skipped).", file=sys.stderr)
        return None

    return {
        "file": Path(json_path).name,
        "system": data.get("system", Path(json_path).stem),
        "version": data.get("version", "2.0.0"),
        "cpu_brand": cpu.get("brand", "Unknown CPU"),
        "arch": cpu.get("arch", "unknown"),
        "logical_cores": cpu.get("logical_cores", 1),
        "simd": ", ".join(cpu.get("simd_features", [])) or "Scalar Baseline",
        "threads": data.get("config", {}).get("threads", cpu.get("logical_cores", 1)),
        "mem_bw_gbps": results.get("memory_bandwidth_gbps", None),
        "st_compress_kbps": results.get("st_compress_kbps", 0.0),
        "mt_compress_kbps": results.get("mt_compress_kbps", 0.0),
        "mt_decompress_kbps": results.get("mt_decompress_kbps", 0.0),
        "speedup": results.get("speedup_ratio", 1.0),
        "rarmark": results.get("rarmark_score", 0),
        "verified": results.get("decompression_verified", False),
    }


def format_throughput(kbps):
    mbps = kbps / 1024.0
    if mbps >= 1024.0:
        return f"{mbps / 1024.0:.2f} GB/s"
    return f"{mbps:.1f} MB/s"


def format_row(rank, entry):
    mem_bw = f"{entry['mem_bw_gbps']:.1f} GB/s" if entry['mem_bw_gbps'] else "-"
    st_comp = f"{entry['st_compress_kbps'] / 1024.0:.1f} MB/s"
    mt_comp = f"{entry['mt_compress_kbps'] / 1024.0:.1f} MB/s ({entry['speedup']:.2f}x)"
    decomp = format_throughput(entry['mt_decompress_kbps'])
    score = f"**{entry['rarmark']:,}**"

    return (
        f"| {rank} | **{entry['cpu_brand']}**<br>_{entry['system']}_ | "
        f"{entry['arch']} | {entry['threads']}T | `{entry['simd']}` | "
        f"{mem_bw} | {st_comp} | {mt_comp} | {decomp} | {score} |"
    )


def generate_markdown(ci_entries, community_entries):
    all_entries = sorted(ci_entries + community_entries, key=lambda x: x["rarmark"], reverse=True)

    header = (
        "| Rank | Processor / System | Arch | Threads | Vector SIMD | "
        "Memory Bandwidth | Single-Thread | Multi-Thread | Parallel Decompress | RARMark Score |\n"
        "|:---:|---|:---:|:---:|---|:---:|:---:|:---:|:---:|:---:|\n"
    )

    overall_table = header + "\n".join(format_row(i + 1, e) for i, e in enumerate(all_entries))
    ci_table = header + "\n".join(format_row(i + 1, e) for i, e in enumerate(ci_entries))
    community_table = header + "\n".join(format_row(i + 1, e) for i, e in enumerate(community_entries)) if community_entries else "_No community submissions yet._"

    formula = r"$$\text{RARMark Score} = \frac{0.6 \times \text{MT Compression (KB/s)} + 0.4 \times \text{MT Decompression (KB/s)}}{10}$$"
    doc = f"""# OpenRAR RARMark v2.0 Hardware Benchmarks

Automated multi-platform performance audit and community hardware leaderboard for **OpenRAR 2.0**.

The built-in benchmark (`openrar b`) exercises the complete in-memory engine:
1. **Multi-Threaded Memory Bandwidth:** Measures host DRAM/cache streaming copy bandwidth in GB/s across active threads.
2. **Single-Thread Compression:** Benchmarks core LZ/Huffman algorithm throughput without thread overhead.
3. **Multi-Thread Compression:** Measures parallel solid-block chunking and thread-scaling efficiency across all CPU cores.
4. **Parallel Decompression:** Stresses multi-threaded decompression, CRC32 calculation, and memory write-back (saturating up to 1.0+ GB/s).
5. **Bit-Exact Integrity Gate:** Verifies that 100% of decompressed bytes match the original corpus CRC32 (`decompression_verified: true`).

{formula}

---

## Overall RARMark Leaderboard

{overall_table}

---

## Official CI Hardware Baselines

Standardized GitHub Actions runner environments measured during CI builds:

{ci_table}

---

## Community Hardware Submissions

Benchmarks submitted by community members across diverse architectures (Ryzen, Core Ultra, Apple Silicon, ARM64 servers, Raspberry Pi):

{community_table}

---

## How to Run & Submit Your Benchmark

You can benchmark your machine directly using the OpenRAR CLI:

### 1. Run the Benchmark
```bash
# Standard benchmark (all logical cores, 5s per phase)
openrar b

# Quick benchmark with plain log output
openrar b --seconds=1 -plain

# Emit RFC 8259 structured JSON with hardware metrics
openrar b --json > my_hardware.json
```

### 2. Submit Your Results
There are three ways to contribute your benchmark to this leaderboard:

- **Option A (Instant Cloud Ingestion - Recommended):** Pipe directly to our Cloudflare Workers edge ingestion API:
  ```bash
  openrar b --submit
  # Or with custom endpoint / piping:
  openrar b --json | curl -s -X POST -H "Content-Type: application/json" -d @- https://benchmark.openrar.workers.dev/api/submit
  ```
  Submissions are verified cryptographically in real-time, saved to Cloudflare D1, and automatically synced into this table. View live stats at [Cloud Leaderboard](https://benchmark.openrar.workers.dev/) or the [openrar-benchmark-submission](https://github.com/geometric-dev/openrar-benchmark-submission) service.

- **Option B (GitHub Issue Form):** Open an issue using the [Benchmark Submission Template](https://github.com/geometric-dev/openrar/issues/new?template=benchmark_submission.yml) and paste your JSON output.

- **Option C (Pull Request):** Place `my_hardware.json` in `benchmarks/submissions/<your-cpu-name>.json` and open a PR. The aggregation workflow will automatically validate your results and update the leaderboard!
"""
    return doc


def main():
    parser = argparse.ArgumentParser(description="Aggregate OpenRAR hardware benchmarks into BENCHMARKS.md")
    parser.add_argument("--benchmarks-dir", default="benchmarks", help="Base directory containing ci/ and submissions/")
    parser.add_argument("--output", default="BENCHMARKS.md", help="Path to output markdown file")
    parser.add_argument("--check", action="store_true", help="Fail if output is not up-to-date")
    args = parser.parse_args()

    base_dir = Path(args.benchmarks_dir)
    ci_files = glob.glob(str(base_dir / "ci" / "*.json"))
    comm_files = glob.glob(str(base_dir / "submissions" / "*.json"))

    ci_entries = [e for f in ci_files if (e := load_and_validate(f))]
    raw_comm = [e for f in comm_files if (e := load_and_validate(f))]

    # Filter out community submissions that duplicate CI baselines (e.g. CI test submissions)
    def is_ci_duplicate(entry):
        for ci in ci_entries:
            if entry.get("cpu_brand") == ci.get("cpu_brand") and entry.get("system") == ci.get("system"):
                return True
        if entry.get("system", "").startswith("GitHub Actions"):
            return True
        return False

    comm_entries = [e for e in raw_comm if not is_ci_duplicate(e)]

    ci_entries.sort(key=lambda x: x["rarmark"], reverse=True)
    comm_entries.sort(key=lambda x: x["rarmark"], reverse=True)

    markdown = generate_markdown(ci_entries, comm_entries)

    out_path = Path(args.output)

    if args.check:
        if not out_path.exists():
            print(f"Error: {out_path} does not exist.", file=sys.stderr)
            sys.exit(1)
        with open(out_path, "r", encoding="utf-8") as f:
            existing = f.read()
        if existing != markdown:
            print(f"Error: {out_path} is out of date. Run python tools/aggregate_benchmarks.py to update.", file=sys.stderr)
            sys.exit(1)
        print(f"Check passed: {out_path} is synchronized with all benchmark entries.")
        sys.exit(0)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(markdown)

    print(f"Successfully generated {out_path} ({len(ci_entries)} CI baselines, {len(comm_entries)} community submissions).")


if __name__ == "__main__":
    main()
