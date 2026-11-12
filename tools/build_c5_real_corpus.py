#!/usr/bin/env python3
"""Cross-compile the pinned C5 source into a real ISA corpus with clang.

The committed C5 fixtures are synthetic stubs because the Windows reference
host has no clang (Q-7). This script produces the real objects in CI:

    clang --target=aarch64-linux-gnu -O2 -c pinned.c -o elf_aarch64.o
    clang --target=riscv64-linux-gnu -O2 -c pinned.c -o elf_riscv64.o
    clang --target=x86_64-linux-gnu  -O2 -c pinned.c -o elf_x86_64.o

The source is freestanding (no headers, no libc), so no sysroot is needed.

Usage:

    python3 tools/build_c5_real_corpus.py --out build/c5_real [--clang PATH]

Exit status is non-zero if the pinned source hash does not match, clang is
missing, a target fails to compile, or the produced machine type is wrong.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PINNED_SOURCE = ROOT / "tests" / "fixtures" / "c5" / "pinned.c"

# Changing pinned.c changes the corpus and voids every measurement taken on it.
PINNED_SHA256 = "a8dae4e0c3fa896f97ed710a396be3bd9f32b7cbf2bf683df4b556b53130ff8a"

# name -> (clang triple, expected ELF e_machine)
TARGETS = {
    "x86_64": ("x86_64-linux-gnu", 0x3E),
    "aarch64": ("aarch64-linux-gnu", 0xB7),
    "riscv64": ("riscv64-linux-gnu", 0xF3),
}

CFLAGS = ("-O2", "-c", "-ffreestanding", "-fno-builtin", "-fno-asynchronous-unwind-tables")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def valid_e8_calls(data: bytes) -> int:
    """Count E8 rel32 sites the detector would accept (same rule as filters50)."""
    count = 0
    i = 0
    limit = len(data) - 4
    while i < limit:
        j = data.find(0xE8, i)
        if j < 0 or j >= limit:
            break
        addr = struct.unpack_from("<I", data, j + 1)[0]
        if addr < 0x01000000 or addr >= 0xFF000000:
            count += 1
            i = j + 5
        else:
            i = j + 1
    return count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/c5_real")
    ap.add_argument("--clang", default=None, help="clang executable (default: PATH)")
    args = ap.parse_args()

    if not PINNED_SOURCE.is_file():
        print(f"missing pinned source: {PINNED_SOURCE}", file=sys.stderr)
        return 2
    digest = sha256(PINNED_SOURCE)
    if digest != PINNED_SHA256:
        print(f"pinned source hash mismatch: {digest} != {PINNED_SHA256}\n"
              "The corpus is pinned; restore pinned.c or update the pin and every "
              "measurement that quotes it.", file=sys.stderr)
        return 2

    clang = args.clang or shutil.which("clang")
    if not clang:
        print("clang not found; this script only runs where clang can target the "
              "aarch64/riscv64/x86_64 triples (CI ubuntu leg).", file=sys.stderr)
        return 2

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    written = []
    for name, (triple, expected_machine) in TARGETS.items():
        target = out / f"elf_{name}.o"
        if target.exists():
            target.unlink()
        cmd = [clang, f"--target={triple}", *CFLAGS, "-o", str(target), str(PINNED_SOURCE)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 or not target.is_file():
            print(f"clang failed for {triple} (rc={proc.returncode}):\n"
                  f"{proc.stdout}{proc.stderr}", file=sys.stderr)
            return 1

        data = target.read_bytes()
        if data[:4] != b"\x7fELF" or len(data) < 20:
            print(f"{target.name} is not an ELF object", file=sys.stderr)
            return 1
        machine = struct.unpack_from("<H", data, 0x12)[0]
        if machine != expected_machine:
            print(f"{target.name}: e_machine 0x{machine:04X}, expected "
                  f"0x{expected_machine:04X}", file=sys.stderr)
            return 1
        calls = valid_e8_calls(data)
        print(f"  {target.name:<16} {len(data):>7,} B  e_machine=0x{machine:04X}  "
              f"valid E8 sites={calls}")
        if name == "x86_64" and calls < 32:
            print("  x86_64 object does not reach the E8 density threshold (32); the "
                  "size gate would not exercise the filter.", file=sys.stderr)
            return 1
        written.append(target)

    lines = []
    for path in sorted(written):
        lines.append(f"{sha256(path)}  {path.name}\n")
    with (out / "manifest.sha256").open("w", encoding="utf-8", newline="") as fh:
        fh.write("".join(lines))
    print(f"wrote {len(written)} real C5 objects + manifest to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
