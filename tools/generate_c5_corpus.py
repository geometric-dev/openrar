#!/usr/bin/env python3
"""Generate the C5 ISA-dispatch corpus deterministically.

The Windows reference host has no clang (Q-7), so real cross-compiled objects
cannot be produced here. These stubs are structurally valid ELF64 / PE32+
containers with an ISA-shaped payload, sufficient to exercise
Filters50::detect_filter. They are NOT compiler output and must not be used to
argue compression ratio on real ARM64/RISC-V code; generate the real corpus in
CI with:

    clang --target=aarch64-linux-gnu -O2 -c pinned.c -o elf_aarch64.o
    clang --target=riscv64-linux-gnu -O2 -c pinned.c -o elf_riscv64.o
    clang --target=x86_64-linux-gnu  -O2 -c pinned.c -o elf_x86_64.o

Usage: python tools/generate_c5_corpus.py [--out tests/fixtures/c5]
"""

from __future__ import annotations

import argparse
import hashlib
import random
import struct
from pathlib import Path

SEED = 1337
PAYLOAD_BYTES = 10000

MACHINES = {
    "x86_64": (0x3E, "amd64"),
    "aarch64": (0xB7, "arm64"),
    "riscv64": (0xF3, "riscv64"),
}

PE_MACHINES = {
    "x86_64": 0x8664,
    "aarch64": 0xAA64,
}

# Frozen evidence, not generated: the original ARM64 PE stub from the first
# C2/C5 eval, whose payload is x86-shaped (101 0xE8 bytes). It is kept so the
# corpus swap cannot be read as moving the goalposts; see README.md.
FROZEN_FIXTURES = ("control_pe_aarch64_x86payload.exe",)


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _x86_payload(rng: random.Random, size: int) -> bytes:
    """Deterministic x86-64-shaped text: CALL rel32, NOPs, and filler."""
    out = bytearray()
    while len(out) < size:
        if rng.random() < 0.35:
            out.append(0xE8)  # CALL rel32
            out += struct.pack("<I", rng.randrange(0, 0x01000000))
        elif rng.random() < 0.5:
            out.append(0x90)  # NOP
        else:
            out += bytes(rng.randrange(256) for _ in range(4))
    return bytes(out[:size])


def _aarch64_payload(rng: random.Random, size: int) -> bytes:
    """Deterministic AArch64-shaped text: BL imm26 and NOP words."""
    out = bytearray()
    while len(out) < size:
        if rng.random() < 0.5:
            out += struct.pack("<I", 0x94000000 | rng.randrange(1 << 26))  # BL
        else:
            out += struct.pack("<I", 0xD503201F)  # NOP
    return bytes(out[:size])


def _riscv_payload(rng: random.Random, size: int) -> bytes:
    """Deterministic RISC-V-shaped text: JAL and NOP words."""
    out = bytearray()
    while len(out) < size:
        if rng.random() < 0.5:
            # JAL x0, imm: opcode 0x6F, rd=0, funct3=0, imm in bits [31:12].
            out += struct.pack("<I", (rng.randrange(1 << 20) << 12) | 0x0000006F)
        else:
            out += struct.pack("<I", 0x00000013)  # NOP
    return bytes(out[:size])


PAYLOADS = {
    "x86_64": _x86_payload,
    "aarch64": _aarch64_payload,
    "riscv64": _riscv_payload,
}


def elf64(machine: int, payload: bytes) -> bytes:
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8
    hdr = ident + struct.pack(
        "<HHIQQQIHHHHHH",
        1,          # e_type = ET_REL
        machine,    # e_machine
        1,          # e_version
        0, 0, 0,    # e_entry, e_phoff, e_shoff
        0,          # e_flags
        64, 0, 0,   # e_ehsize, e_phentsize, e_phnum
        0, 0, 0,    # e_shentsize, e_shnum, e_shstrndx
    )
    assert len(hdr) == 64
    return hdr + payload


def pe64(machine: int, payload: bytes) -> bytes:
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)
    nt = b"PE\x00\x00"
    coff = struct.pack(
        "<HHIIIHH",
        machine, 1, 0, 0, 0, 0xF0, 0x0022
    )
    opt = bytearray(0xF0)
    struct.pack_into("<H", opt, 0, 0x20B)  # PE32+
    struct.pack_into("<I", opt, 16, 0x1000)  # AddressOfEntryPoint
    struct.pack_into("<I", opt, 20, 0x1000)  # BaseOfCode
    struct.pack_into("<Q", opt, 24, 0x140000000)  # ImageBase
    struct.pack_into("<I", opt, 32, 0x1000)  # SectionAlignment
    struct.pack_into("<I", opt, 36, 0x200)   # FileAlignment
    struct.pack_into("<H", opt, 40, 6)       # MajorOperatingSystemVersion
    struct.pack_into("<H", opt, 48, 6)       # MajorSubsystemVersion
    struct.pack_into("<I", opt, 56, _align_up(512 + len(payload), 0x1000))  # SizeOfImage
    struct.pack_into("<I", opt, 60, 512)     # SizeOfHeaders
    struct.pack_into("<H", opt, 68, 3)       # Subsystem = CUI
    raw_off = 512
    sect = struct.pack(
        "<8sIIIIIIHHI",
        b".text\x00\x00\x00",
        len(payload), 0x1000, len(payload), raw_off, 0, 0, 0, 0, 0x60000020
    )
    head = bytes(dos) + nt + coff + bytes(opt) + sect
    assert len(head) <= raw_off, len(head)
    return head + b"\x00" * (raw_off - len(head)) + payload


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tests/fixtures/c5")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    written: list[Path] = []
    for name, (machine, _) in MACHINES.items():
        rng = random.Random(SEED)
        data = elf64(machine, PAYLOADS[name](rng, PAYLOAD_BYTES))
        p = out / f"elf_{name}.o"
        p.write_bytes(data)
        written.append(p)

    for name, machine in PE_MACHINES.items():
        rng = random.Random(SEED + 1)
        data = pe64(machine, PAYLOADS[name](rng, PAYLOAD_BYTES))
        p = out / f"pe_{name}.exe"
        p.write_bytes(data)
        written.append(p)

    entries = list(written)
    for name in FROZEN_FIXTURES:
        frozen = out / name
        if frozen.is_file():
            entries.append(frozen)
        else:
            print(f"[WARN] frozen control missing: {frozen}")

    lines = []
    for p in entries:
        digest = hashlib.sha256(p.read_bytes()).hexdigest()
        lines.append(f"{digest}  {p.name}\n")
    # newline="" keeps the manifest LF-only on Windows, so sha256sum -c works
    # on the committed copy from any host.
    with (out / "manifest.sha256").open("w", encoding="utf-8", newline="") as fh:
        fh.write("".join(lines))
    print(f"wrote {len(written)} C5 stubs + {len(entries) - len(written)} frozen control(s) "
          f"+ manifest to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
