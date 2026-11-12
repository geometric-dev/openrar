#!/usr/bin/env python3
"""Measure the C5 ISA-dispatch size gate.

Builds one isolated archive per C5 member for each supplied binary and
compression level, then verifies every archive extracts to the original bytes.
The gate is "no C5 member may grow relative to the baseline binary".

The comparison is between binaries built from *this* branch and from `main`
(c5ff210), not against the v2.0.23 release artifact.

Usage:

    python tools/measure_c5.py \
        --baseline main=/path/to/main/build/openrar64/Release/openrar.exe \
        --candidate branch=build/openrar64/Release/openrar.exe

Exit status is 0 when no member grows and 1 otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURES = (
    "elf_x86_64.o",
    "elf_aarch64.o",
    "elf_riscv64.o",
    "pe_x86_64.exe",
    "pe_aarch64.exe",
)

# Measured and reported, never gated: the original ARM64 PE stub whose payload
# is x86-shaped. It records what the first eval measured; see the corpus README.
CONTROL_FIXTURES = ("control_pe_aarch64_x86payload.exe",)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_archive(exe: Path, level: int, src: Path, archive: Path) -> int:
    """Create a non-solid archive holding exactly `src`; return its size."""
    if archive.exists():
        archive.unlink()
    proc = subprocess.run(
        [str(exe), "a", f"-m{level}", "-ep", "-y", str(archive), str(src)],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0 or not archive.exists():
        raise RuntimeError(
            f"{exe.name} -m{level} failed on {src.name}: "
            f"rc={proc.returncode}\n{proc.stdout}\n{proc.stderr}"
        )
    return archive.stat().st_size


def verify_roundtrip(exe: Path, archive: Path, expected: str, work: Path, tag: str) -> None:
    out = work / f"x_{tag}"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    proc = subprocess.run(
        [str(exe), "x", "-o+", "-y", str(archive)],
        cwd=out,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"extract failed for {archive.name}: {proc.stderr}")
    produced = [p for p in out.rglob("*") if p.is_file()]
    if len(produced) != 1:
        raise RuntimeError(f"expected 1 extracted file from {archive.name}, got {len(produced)}")
    actual = sha256(produced[0])
    if actual != expected:
        raise RuntimeError(f"roundtrip mismatch for {archive.name}: {actual} != {expected}")


def parse_binary(spec: str) -> tuple[str, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected name=/path/to/openrar")
    name, raw = spec.split("=", 1)
    path = Path(raw)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"no such binary: {path}")
    return name, path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", action="append", type=parse_binary, required=True,
                    help="name=/path/to/openrar (repeatable); first one is the delta baseline")
    ap.add_argument("--candidate", action="append", type=parse_binary, default=[],
                    help="name=/path/to/openrar (repeatable)")
    ap.add_argument("--corpus", default="tests/fixtures/c5")
    ap.add_argument("--levels", default="0,1,3,5")
    ap.add_argument("--work", default=None, help="scratch directory (default: temp)")
    ap.add_argument("--json", default=None, help="write the measurement matrix here")
    args = ap.parse_args()

    binaries = args.baseline + args.candidate
    levels = [int(x) for x in args.levels.split(",") if x.strip()]
    corpus = Path(args.corpus)
    missing = [f for f in FIXTURES if not (corpus / f).is_file()]
    if missing:
        print(f"missing C5 fixtures in {corpus}: {', '.join(missing)}", file=sys.stderr)
        return 2

    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="c5_measure_"))
    work.mkdir(parents=True, exist_ok=True)

    rows = []
    control_rows = []
    failures = []
    for fixture in FIXTURES:
        src = corpus / fixture
        digest = sha256(src)
        for level in levels:
            sizes = {}
            for name, exe in binaries:
                archive = work / f"{fixture}.{name}.m{level}.rar"
                sizes[name] = build_archive(exe, level, src, archive)
                verify_roundtrip(exe, archive, digest, work, f"{fixture}.{name}.m{level}")
            base_name, base_exe = binaries[0]
            for name, exe in binaries[1:]:
                delta = sizes[name] - sizes[base_name]
                if delta > 0:
                    failures.append((fixture, level, name, delta))
            rows.append({"fixture": fixture, "level": level, "sizes": sizes})

    for fixture in CONTROL_FIXTURES:
        src = corpus / fixture
        if not src.is_file():
            continue
        for level in levels:
            sizes = {}
            for name, exe in binaries:
                archive = work / f"{fixture}.{name}.m{level}.rar"
                sizes[name] = build_archive(exe, level, src, archive)
            control_rows.append({"fixture": fixture, "level": level, "sizes": sizes})

    names = [n for n, _ in binaries]
    header = "| member | -m | " + " | ".join(names) + " | delta vs " + names[0] + " |"
    print(header)
    print("|---|---|" + "|".join("---:" for _ in names) + "|---:|")
    for row in rows:
        cells = " | ".join(str(row["sizes"][n]) for n in names)
        deltas = [row["sizes"][n] - row["sizes"][names[0]] for n in names[1:]]
        delta_cell = ", ".join(f"{d:+d}" for d in deltas) if deltas else "-"
        print(f"| `{row['fixture']}` | {row['level']} | {cells} | {delta_cell} |")

    if control_rows:
        print("\nNon-gating control (x86-shaped payload in an ARM64 container):")
        for row in control_rows:
            cells = " | ".join(str(row["sizes"][n]) for n in names)
            deltas = [row["sizes"][n] - row["sizes"][names[0]] for n in names[1:]]
            delta_cell = ", ".join(f"{d:+d}" for d in deltas) if deltas else "-"
            print(f"| `{row['fixture']}` | {row['level']} | {cells} | {delta_cell} |")

    if args.json:
        Path(args.json).write_text(json.dumps({"binaries": names, "rows": rows,
                                               "controls": control_rows}, indent=2) + "\n",
                                  encoding="utf-8")

    if failures:
        print("\nC5 SIZE GATE: FAIL")
        for fixture, level, name, delta in failures:
            print(f"  {fixture} -m{level}: {name} grew by {delta} B")
        return 1
    print("\nC5 SIZE GATE: PASS (no member grew)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
