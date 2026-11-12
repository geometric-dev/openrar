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
PINNED_SHA256 = "d3bad8e15c404cf28aa259540c244c7e11d2c6911151d0ad20014586dbf38847"

# name -> (clang triple, expected ELF e_machine)
TARGETS = {
    "x86_64": ("x86_64-linux-gnu", 0x3E),
    "aarch64": ("aarch64-linux-gnu", 0xB7),
    "riscv64": ("riscv64-linux-gnu", 0xF3),
}

CFLAGS = ("-O2", "-c", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
          "-fno-asynchronous-unwind-tables")

# Real PE images need a linker as well as a compiler: lld-link is not present
# everywhere, so this is opt-in (--pe) and skipped loudly when unavailable.
PE_TARGETS = {
    "x86_64": ("x86_64-pc-windows-msvc", "x64", 0x8664),
    "aarch64": ("aarch64-pc-windows-msvc", "arm64", 0xAA64),
}


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
    ap.add_argument("--pe", action="store_true",
                    help="also link real PE32+ images (needs lld-link)")
    ap.add_argument("--lld-link", default=None, help="lld-link executable (default: PATH)")
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

    if args.pe:
        lld_link = args.lld_link or shutil.which("lld-link")
        if not lld_link:
            print("lld-link not found; cannot link the real PE objects. Install lld and "
                  "re-run with --pe.", file=sys.stderr)
            return 2
        for name, (triple, machine_flag, expected_machine) in PE_TARGETS.items():
            obj = out / f"pe_{name}.o"
            image = out / f"pe_{name}.exe"
            for stale in (obj, image):
                if stale.exists():
                    stale.unlink()
            compile_cmd = [clang, f"--target={triple}", *CFLAGS, "-o", str(obj),
                           str(PINNED_SOURCE)]
            proc = subprocess.run(compile_cmd, capture_output=True, text=True)
            if proc.returncode != 0 or not obj.is_file():
                print(f"clang failed for {triple} (rc={proc.returncode}):\n"
                      f"{proc.stdout}{proc.stderr}", file=sys.stderr)
                return 1
            link_cmd = [lld_link, f"/machine:{machine_flag}", "/entry:c5_entry",
                        "/subsystem:console", "/nodefaultlib", f"/out:{image}", str(obj)]
            proc = subprocess.run(link_cmd, capture_output=True, text=True)
            obj.unlink(missing_ok=True)
            if proc.returncode != 0 or not image.is_file():
                print(f"lld-link failed for {machine_flag} (rc={proc.returncode}):\n"
                      f"{proc.stdout}{proc.stderr}", file=sys.stderr)
                return 1
            data = image.read_bytes()
            if data[:2] != b"MZ" or len(data) < 128:
                print(f"{image.name} is not a PE image", file=sys.stderr)
                return 1
            pe_off = struct.unpack_from("<I", data, 0x3C)[0]
            if pe_off + 6 > len(data) or data[pe_off:pe_off + 4] != b"PE\x00\x00":
                print(f"{image.name} has no PE header", file=sys.stderr)
                return 1
            machine = struct.unpack_from("<H", data, pe_off + 4)[0]
            if machine != expected_machine:
                print(f"{image.name}: machine 0x{machine:04X}, expected "
                      f"0x{expected_machine:04X}", file=sys.stderr)
                return 1
            calls = valid_e8_calls(data)
            print(f"  {image.name:<16} {len(data):>7,} B  machine=0x{machine:04X}  "
                  f"valid E8 sites={calls}")
            if name == "x86_64" and calls < 32:
                print("  PE x86-64 image does not reach the E8 density threshold (32).",
                      file=sys.stderr)
                return 1
            written.append(image)

    lines = []
    for path in sorted(written):
        lines.append(f"{sha256(path)}  {path.name}\n")
    with (out / "manifest.sha256").open("w", encoding="utf-8", newline="") as fh:
        fh.write("".join(lines))
    print(f"wrote {len(written)} real C5 objects + manifest to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
