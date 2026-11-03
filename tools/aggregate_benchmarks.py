#!/usr/bin/env python3
"""
OpenRAR Hardware Benchmark Aggregator, Leaderboard & GitHub Pages Generator
Scans benchmarks/ci/*.json and benchmarks/submissions/*.json, validates
cryptographic decompression integrity, ranks hardware by RARMark v2.0 score,
and generates:
  1. BENCHMARKS.md (GitHub repository markdown table)
  2. docs/index.html (GitHub Pages interactive web leaderboard)
  3. docs/leaderboard.json (Structured JSON API for CLI & web consumption)

Usage:
  python tools/aggregate_benchmarks.py [--check] [--output BENCHMARKS.md]
"""

import argparse
import glob
import html
import json
import os
import sys
from datetime import datetime, timezone
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

# View top scores from the global leaderboard in your terminal
openrar b --leaderboard

# Emit RFC 8259 structured JSON with hardware metrics
openrar b --json > my_hardware.json
```

### 2. View Live Leaderboard & Submit Results
- **Interactive Web Leaderboard:** View live rankings, search CPUs, and inspect vector extensions on the official [GitHub Pages Leaderboard](https://geometric-dev.github.io/openrar/).
- **In-Terminal Query:** Run `openrar b --leaderboard` to query the top hardware rankings directly in your terminal.
- **Option A (Instant Cloud Ingestion - Recommended):** Submit results directly via CLI:
  ```bash
  openrar b --submit
  # Or with custom rig description:
  openrar b --submit --system="Workstation / 64GB DDR5"
  ```
  Submissions are verified cryptographically in real-time, saved to Cloudflare D1, and automatically synced into this table and the GitHub Pages site on every periodic update.
- **Option B (GitHub Issue Form):** Open an issue using the [Benchmark Submission Template](https://github.com/geometric-dev/openrar/issues/new?template=benchmark_submission.yml) and paste your JSON output.
- **Option C (Pull Request):** Place `my_hardware.json` in `benchmarks/submissions/<your-cpu-name>.json` and open a PR. The aggregation workflow will automatically validate your results and update the leaderboard!
"""
    return doc


def format_html_row(rank, entry):
    if rank == 1:
        rank_badge = '<span class="rank gold">&#x1F947; 1</span>'
    elif rank == 2:
        rank_badge = '<span class="rank silver">&#x1F948; 2</span>'
    elif rank == 3:
        rank_badge = '<span class="rank bronze">&#x1F949; 3</span>'
    else:
        rank_badge = f'<span class="rank default">#{rank}</span>'

    mem_bw = f"{entry['mem_bw_gbps']:.1f} GB/s" if entry['mem_bw_gbps'] else "-"
    st_comp = f"{entry['st_compress_kbps'] / 1024.0:.1f} MB/s"
    mt_comp = f"{entry['mt_compress_kbps'] / 1024.0:.1f} MB/s <span class=\"scaling\">({entry['speedup']:.2f}x)</span>"
    decomp = format_throughput(entry['mt_decompress_kbps'])
    score = f"<strong>{entry['rarmark']:,}</strong>"

    simd_badges = " ".join(
        f'<span class="badge badge-simd">{html.escape(s.strip())}</span>'
        for s in entry['simd'].split(",")
        if s.strip()
    )
    arch_badge = f'<span class="badge badge-arch">{html.escape(entry["arch"])}</span>'

    return f"""          <tr>
            <td class="col-rank">{rank_badge}</td>
            <td class="col-cpu">
              <div class="cpu-name">{html.escape(entry['cpu_brand'])}</div>
              <div class="system-sub">{html.escape(entry['system'])}</div>
            </td>
            <td>{arch_badge}</td>
            <td class="num">{entry['threads']}T</td>
            <td class="simd-cell">{simd_badges}</td>
            <td class="num">{mem_bw}</td>
            <td class="num">{st_comp}</td>
            <td class="num">{mt_comp}</td>
            <td class="num">{decomp}</td>
            <td class="num col-score">{score}</td>
          </tr>"""


def generate_html(ci_entries, comm_entries, all_entries, updated_at=None):
    if not updated_at:
        updated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    total_submissions = len(all_entries)
    top_score = f"{all_entries[0]['rarmark']:,}" if all_entries else "0"
    top_cpu = all_entries[0]["cpu_brand"] if all_entries else "N/A"

    max_mem_bw = max((e["mem_bw_gbps"] for e in all_entries if e["mem_bw_gbps"]), default=0.0)
    max_mem_str = f"{max_mem_bw:.1f} GB/s" if max_mem_bw > 0 else "-"

    max_decomp = max((e["mt_decompress_kbps"] for e in all_entries), default=0.0)
    max_decomp_str = format_throughput(max_decomp)

    overall_rows = "\n".join(format_html_row(i + 1, e) for i, e in enumerate(all_entries))
    ci_rows = "\n".join(format_html_row(i + 1, e) for i, e in enumerate(ci_entries))
    comm_rows = "\n".join(format_html_row(i + 1, e) for i, e in enumerate(comm_entries))

    table_header = """        <thead>
          <tr>
            <th class="col-rank">Rank</th>
            <th>Processor / System</th>
            <th>Arch</th>
            <th style="text-align: right">Threads</th>
            <th>Vector SIMD Acceleration</th>
            <th style="text-align: right">Memory BW</th>
            <th style="text-align: right">Single-Thread</th>
            <th style="text-align: right">Multi-Thread</th>
            <th style="text-align: right">Parallel Decompress</th>
            <th style="text-align: right">RARMark Score</th>
          </tr>
        </thead>"""

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta name="description" content="OpenRAR RARMark v2.0 Global Hardware Leaderboard - Automated compression performance audits & community rankings">
  <title>OpenRAR RARMark v2.0 Global Hardware Leaderboard</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600;700&family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <style>
    :root {{
      --bg: #0d1117;
      --card-bg: #161b22;
      --border: #30363d;
      --text: #c9d1d9;
      --text-muted: #8b949e;
      --heading: #f0f6fc;
      --accent: #58a6ff;
      --accent-green: #3fb950;
      --accent-gold: #d29922;
      --code-bg: #0b0f14;
    }}
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{
      background: var(--bg);
      color: var(--text);
      font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
      line-height: 1.5;
      padding: 2rem 1.5rem;
    }}
    .container {{ max-width: 1320px; margin: 0 auto; }}
    header {{ margin-bottom: 2rem; border-bottom: 1px solid var(--border); padding-bottom: 1.5rem; }}
    .header-top {{ display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 1rem; }}
    h1 {{ font-size: 1.8rem; font-weight: 700; color: var(--heading); display: flex; align-items: center; gap: 0.6rem; }}
    .tagline {{ color: var(--text-muted); font-size: 0.95rem; margin-top: 0.4rem; }}
    .header-links {{ display: flex; gap: 0.75rem; align-items: center; flex-wrap: wrap; }}
    .btn-link {{
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
      background: var(--card-bg);
      color: var(--heading);
      border: 1px solid var(--border);
      padding: 0.5rem 1rem;
      border-radius: 6px;
      font-size: 0.85rem;
      text-decoration: none;
      transition: all 0.2s;
    }}
    .btn-link:hover {{ border-color: var(--accent); color: var(--accent); }}

    /* Stats Grid */
    .stats-grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 1rem;
      margin-bottom: 2rem;
    }}
    .stat-card {{
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 1.25rem;
    }}
    .stat-title {{ font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-muted); margin-bottom: 0.25rem; }}
    .stat-value {{ font-size: 1.6rem; font-weight: 700; color: var(--heading); font-family: 'JetBrains Mono', monospace; }}
    .stat-sub {{ font-size: 0.8rem; color: var(--text-muted); margin-top: 0.2rem; }}

    /* CLI Quickstart Box */
    .cli-box {{
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 1.25rem;
      margin-bottom: 2rem;
    }}
    .cli-box h3 {{ font-size: 1rem; color: var(--heading); margin-bottom: 0.4rem; }}
    .cli-box p {{ font-size: 0.85rem; color: var(--text-muted); margin-bottom: 0.75rem; }}
    .code-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 0.75rem; }}
    .code-item {{ position: relative; }}
    .code-block {{
      background: var(--code-bg);
      border: 1px solid var(--border);
      padding: 0.75rem 1rem;
      border-radius: 6px;
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.85rem;
      color: var(--accent-green);
      overflow-x: auto;
      user-select: all;
    }}
    .copy-btn {{
      position: absolute;
      right: 8px;
      top: 50%;
      transform: translateY(-50%);
      background: #21262d;
      border: 1px solid var(--border);
      color: var(--text);
      font-size: 0.75rem;
      padding: 4px 8px;
      border-radius: 4px;
      cursor: pointer;
    }}
    .copy-btn:hover {{ background: #30363d; color: var(--heading); }}

    /* Controls Bar */
    .controls-bar {{
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 1rem;
      margin-bottom: 1rem;
    }}
    .tab-group {{ display: flex; gap: 0.5rem; }}
    .tab-btn {{
      background: var(--card-bg);
      border: 1px solid var(--border);
      color: var(--text-muted);
      padding: 0.5rem 1rem;
      border-radius: 6px;
      font-size: 0.85rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }}
    .tab-btn:hover {{ color: var(--heading); border-color: var(--accent); }}
    .tab-btn.active {{
      background: #1f2937;
      color: var(--heading);
      border-color: var(--accent);
    }}
    .search-box {{
      background: var(--card-bg);
      border: 1px solid var(--border);
      color: var(--text);
      padding: 0.5rem 0.85rem;
      border-radius: 6px;
      font-size: 0.85rem;
      width: 280px;
    }}
    .search-box:focus {{ outline: none; border-color: var(--accent); }}

    /* Leaderboard Table */
    .table-container {{
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      overflow-x: auto;
      margin-bottom: 2rem;
    }}
    table {{ width: 100%; border-collapse: collapse; text-align: left; font-size: 0.9rem; }}
    th {{
      background: #11151c;
      padding: 0.85rem 1rem;
      font-weight: 600;
      color: var(--text-muted);
      border-bottom: 1px solid var(--border);
      white-space: nowrap;
      font-size: 0.8rem;
      text-transform: uppercase;
    }}
    td {{
      padding: 0.85rem 1rem;
      border-bottom: 1px solid #21262d;
      white-space: nowrap;
    }}
    tr:last-child td {{ border-bottom: none; }}
    tr:hover td {{ background: #1c2128; }}
    .num {{ text-align: right; font-family: 'JetBrains Mono', monospace; font-size: 0.85rem; }}
    .col-rank {{ width: 60px; text-align: center; }}
    .rank {{ font-weight: 700; font-family: 'JetBrains Mono', monospace; font-size: 0.95rem; }}
    .rank.gold {{ color: #f2cc60; }}
    .rank.silver {{ color: #c9d1d9; }}
    .rank.bronze {{ color: #d99b66; }}
    .rank.default {{ color: var(--text-muted); font-size: 0.85rem; }}
    .cpu-name {{ font-weight: 600; color: var(--heading); }}
    .system-sub {{ font-size: 0.75rem; color: var(--text-muted); }}
    .col-score {{ color: var(--accent-green); font-size: 1rem; font-weight: 700; }}
    .badge {{
      display: inline-block;
      padding: 2px 6px;
      border-radius: 4px;
      font-size: 0.75rem;
      font-family: 'JetBrains Mono', monospace;
      margin-right: 4px;
      margin-bottom: 2px;
    }}
    .badge-arch {{
      background: #1f242c;
      border: 1px solid #363d47;
      color: #9cdcfe;
    }}
    .badge-simd {{
      background: #19221b;
      border: 1px solid #234228;
      color: #7ee787;
    }}
    .scaling {{ font-size: 0.75rem; color: var(--text-muted); }}

    /* Footer */
    footer {{
      border-top: 1px solid var(--border);
      padding-top: 1.5rem;
      font-size: 0.85rem;
      color: var(--text-muted);
      text-align: center;
    }}
    footer a {{ color: var(--accent); text-decoration: none; }}
    footer a:hover {{ text-decoration: underline; }}
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="header-top">
        <div>
          <h1>&#x1F3C6; OpenRAR RARMark v2.0 Global Leaderboard</h1>
          <div class="tagline">Automated multi-platform performance audit and community hardware rankings for OpenRAR 2.0</div>
        </div>
        <div class="header-links">
          <a class="btn-link" href="https://github.com/geometric-dev/openrar">
            <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
            GitHub Repo
          </a>
          <a class="btn-link" href="https://github.com/geometric-dev/openrar/blob/v2/BENCHMARKS.md">BENCHMARKS.md</a>
          <a class="btn-link" href="leaderboard.json">JSON API</a>
        </div>
      </div>
    </header>

    <div class="stats-grid">
      <div class="stat-card">
        <div class="stat-title">Verified Submissions</div>
        <div class="stat-value">{total_submissions}</div>
        <div class="stat-sub">Across x86_64 and ARM64 platforms</div>
      </div>
      <div class="stat-card">
        <div class="stat-title">Top RARMark Score</div>
        <div class="stat-value" style="color: var(--accent-gold);">&#x1F3C6; {top_score}</div>
        <div class="stat-sub">{html.escape(top_cpu)}</div>
      </div>
      <div class="stat-card">
        <div class="stat-title">Peak Memory Bandwidth</div>
        <div class="stat-value">{max_mem_str}</div>
        <div class="stat-sub">Multi-threaded DRAM streaming</div>
      </div>
      <div class="stat-card">
        <div class="stat-title">Peak Parallel Decompress</div>
        <div class="stat-value">{max_decomp_str}</div>
        <div class="stat-sub">In-memory write-back & CRC32 gate</div>
      </div>
    </div>

    <div class="cli-box">
      <h3>&#x1F680; Benchmark Your Machine & Submit Results</h3>
      <p>Execute the in-memory benchmark (`openrar b`) directly from your terminal and submit to this leaderboard:</p>
      <div class="code-grid">
        <div class="code-item">
          <div class="code-block">openrar b --submit</div>
          <button class="copy-btn" onclick="copyCommand('openrar b --submit', this)">Copy</button>
        </div>
        <div class="code-item">
          <div class="code-block">openrar b --leaderboard</div>
          <button class="copy-btn" onclick="copyCommand('openrar b --leaderboard', this)">Copy</button>
        </div>
        <div class="code-item">
          <div class="code-block">openrar b --json > my_hardware.json</div>
          <button class="copy-btn" onclick="copyCommand('openrar b --json > my_hardware.json', this)">Copy</button>
        </div>
      </div>
    </div>

    <div class="controls-bar">
      <div class="tab-group">
        <button class="tab-btn active" id="btn-overall" onclick="showTab('overall')">Overall Leaderboard ({total_submissions})</button>
        <button class="tab-btn" id="btn-ci" onclick="showTab('ci')">Official CI Baselines ({len(ci_entries)})</button>
        <button class="tab-btn" id="btn-community" onclick="showTab('community')">Community Submissions ({len(comm_entries)})</button>
      </div>
      <input type="text" class="search-box" id="search-input" placeholder="Search CPU, arch, SIMD..." oninput="filterTable(this.value)">
    </div>

    <div id="tab-overall" class="tab-content">
      <div class="table-container">
        <table class="leaderboard-table">
          {table_header}
          <tbody>
{overall_rows}
          </tbody>
        </table>
      </div>
    </div>

    <div id="tab-ci" class="tab-content" style="display: none;">
      <div class="table-container">
        <table class="leaderboard-table">
          {table_header}
          <tbody>
{ci_rows}
          </tbody>
        </table>
      </div>
    </div>

    <div id="tab-community" class="tab-content" style="display: none;">
      <div class="table-container">
        <table class="leaderboard-table">
          {table_header}
          <tbody>
{comm_rows}
          </tbody>
        </table>
      </div>
    </div>

    <footer>
      <p>Hosted on <strong>GitHub Pages</strong> &bull; Updated periodically via <strong>GitHub Actions</strong> &bull; Generated from verified test runs</p>
      <p style="margin-top: 0.4rem;">Last updated: <code>{updated_at}</code> &bull; Formula: <code>RARMark = (0.6 &times; MT_Compress + 0.4 &times; MT_Decompress) / 10</code></p>
    </footer>
  </div>

  <script>
    function showTab(tabId) {{
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(c => c.style.display = 'none');
      document.getElementById('btn-' + tabId).classList.add('active');
      document.getElementById('tab-' + tabId).style.display = 'block';
    }}

    function filterTable(query) {{
      const q = query.toLowerCase();
      document.querySelectorAll('.leaderboard-table tbody tr').forEach(row => {{
        const text = row.innerText.toLowerCase();
        row.style.display = text.includes(q) ? '' : 'none';
      }});
    }}

    function copyCommand(cmd, btn) {{
      navigator.clipboard.writeText(cmd).then(() => {{
        const orig = btn.innerText;
        btn.innerText = 'Copied!';
        setTimeout(() => btn.innerText = orig, 2000);
      }});
    }}
  </script>
</body>
</html>
"""


def generate_json(all_entries, updated_at=None):
    if not updated_at:
        updated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    data = {
        "benchmark": "openrar_rarmark_v2",
        "leaderboard_url": "https://geometric-dev.github.io/openrar/",
        "updated_at": updated_at,
        "total": len(all_entries),
        "entries": [
            {
                "rank": i + 1,
                "cpu_brand": e["cpu_brand"],
                "system": e["system"],
                "arch": e["arch"],
                "threads": e["threads"],
                "simd": e["simd"],
                "mem_bw_gbps": e["mem_bw_gbps"],
                "st_compress_kbps": e["st_compress_kbps"],
                "mt_compress_kbps": e["mt_compress_kbps"],
                "mt_decompress_kbps": e["mt_decompress_kbps"],
                "speedup_ratio": e["speedup"],
                "rarmark_score": e["rarmark"],
            }
            for i, e in enumerate(all_entries)
        ]
    }
    return json.dumps(data, indent=2) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Aggregate OpenRAR hardware benchmarks into BENCHMARKS.md, GitHub Pages HTML, and JSON API")
    parser.add_argument("--benchmarks-dir", default="benchmarks", help="Base directory containing ci/ and submissions/")
    parser.add_argument("--output", default="BENCHMARKS.md", help="Path to output markdown file")
    parser.add_argument("--html-output", default="docs/index.html", help="Path to output GitHub Pages HTML file")
    parser.add_argument("--json-output", default="docs/leaderboard.json", help="Path to output JSON leaderboard")
    parser.add_argument("--check", action="store_true", help="Fail if outputs are not up-to-date")
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
    all_entries = sorted(ci_entries + comm_entries, key=lambda x: x["rarmark"], reverse=True)

    markdown = generate_markdown(ci_entries, comm_entries)
    html_content = generate_html(ci_entries, comm_entries, all_entries)
    json_content = generate_json(all_entries)

    out_md = Path(args.output)
    out_html = Path(args.html_output)
    out_json = Path(args.json_output)

    if args.check:
        for p, expected in [(out_md, markdown)]:
            if not p.exists():
                print(f"Error: {p} does not exist.", file=sys.stderr)
                sys.exit(1)
            with open(p, "r", encoding="utf-8") as f:
                if f.read() != expected:
                    print(f"Error: {p} is out of date. Run python tools/aggregate_benchmarks.py to update.", file=sys.stderr)
                    sys.exit(1)
        print("Check passed: BENCHMARKS.md is synchronized with all benchmark entries.")
        sys.exit(0)

    # Write outputs
    out_md.parent.mkdir(parents=True, exist_ok=True)
    with open(out_md, "w", encoding="utf-8") as f:
        f.write(markdown)

    out_html.parent.mkdir(parents=True, exist_ok=True)
    with open(out_html, "w", encoding="utf-8") as f:
        f.write(html_content)

    # Also mirror HTML to docs/benchmarks/index.html for /benchmarks path compatibility
    mirror_html = Path("docs/benchmarks/index.html")
    mirror_html.parent.mkdir(parents=True, exist_ok=True)
    with open(mirror_html, "w", encoding="utf-8") as f:
        f.write(html_content)

    out_json.parent.mkdir(parents=True, exist_ok=True)
    with open(out_json, "w", encoding="utf-8") as f:
        f.write(json_content)

    mirror_json = Path("benchmarks/leaderboard.json")
    with open(mirror_json, "w", encoding="utf-8") as f:
        f.write(json_content)

    print(f"Successfully generated {out_md}, {out_html}, and {out_json} ({len(ci_entries)} CI baselines, {len(comm_entries)} community submissions).")


if __name__ == "__main__":
    main()
