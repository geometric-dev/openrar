#!/usr/bin/env python3
"""
Sync OpenRAR Cloud Benchmarks from Cloudflare Worker to benchmarks/submissions/

Usage:
  python tools/sync_cloud_benchmarks.py [--endpoint URL] [--output-dir DIR]
"""

import argparse
import json
import os
import sys
import urllib.request
from pathlib import Path


def fetch_export(endpoint: str, timeout: int = 15):
    req = urllib.request.Request(
        endpoint,
        headers={"User-Agent": "OpenRAR-Benchmark-Sync/1.0", "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as response:
        if response.status != 200:
            raise RuntimeError(f"HTTP Error {response.status}: {response.read().decode('utf-8')}")
        data = json.loads(response.read().decode("utf-8"))
        return data


def sync_submissions(endpoint: str, output_dir: Path) -> int:
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        submissions = fetch_export(endpoint)
    except Exception as e:
        print(f"Info: Unable to reach {endpoint} ({e}). Skipping cloud benchmark sync.", file=sys.stderr)
        return 0

    if not isinstance(submissions, list):
        print(f"Warning: Expected JSON list from {endpoint}, got {type(submissions)}", file=sys.stderr)
        return 0

    updated_count = 0
    for entry in submissions:
        slug = entry.get("slug")
        if not slug:
            continue

        file_path = output_dir / f"{slug}.json"
        content_str = json.dumps(entry, indent=2) + "\n"

        if file_path.exists():
            try:
                existing_content = file_path.read_text(encoding="utf-8")
                if existing_content == content_str:
                    continue
            except Exception:
                pass

        file_path.write_text(content_str, encoding="utf-8")
        updated_count += 1
        print(f"Synced submission: {file_path.name}")

    return updated_count


def main():
    parser = argparse.ArgumentParser(description="Sync OpenRAR cloud benchmarks from Cloudflare Worker")
    parser.add_argument(
        "--endpoint",
        default=os.getenv(
            "OPENRAR_BENCHMARK_ENDPOINT",
            "https://openrar-benchmark-submission.openrar-benchmark-submission.workers.dev/api/export",
        ),
        help="Export API endpoint URL",
    )
    parser.add_argument(
        "--output-dir",
        default="benchmarks/submissions",
        type=Path,
        help="Target directory for benchmark JSON files",
    )
    args = parser.parse_args()

    count = sync_submissions(args.endpoint, args.output_dir)
    print(f"Cloud sync completed: {count} new or updated benchmark file(s).")


if __name__ == "__main__":
    main()

