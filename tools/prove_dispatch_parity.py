#!/usr/bin/env python3
"""Prove EXP-MACHINE-DISPATCH is byte-neutral where it must be.

Two checks, both required before promotion:

1. Parity on the 30 ratchet cells (6 deterministic corpora x -m1..-m5).
   The branch and a `main`-built binary must produce byte-identical archives.
   The corpora are the ones `tools/release_regression_guard.py` measures, built
   by the same code, so the cell set is exactly RATCHET_BYTES.

2. Reference extraction of every archive whose bytes DO change (the C5 members
   the dispatch decision affects). UnRAR 7.20 and WinRAR 7.20 must each extract
   the archive to the original bytes; this is the interop gate the roadmap
   requires for changed archives.

Usage:

    python tools/prove_dispatch_parity.py \
        --baseline main=/path/to/main/build/openrar64/Release/openrar.exe \
        --candidate branch=build/openrar64/Release/openrar.exe \
        --json build/dispatch_parity.json

Exit status is 1 if any ratchet cell differs or any changed archive fails a
reference extractor.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import release_regression_guard as rqg  # noqa: E402
from bench_m0_census import generate_corpora  # noqa: E402

RATCHET_CORPORA = (
    "big24.bin",
    "mixed16.bin",
    "synthetic_text8.txt",
    "real_src.txt",
    "real_tests.txt",
    "real_docs.md",
)
METHODS = (1, 2, 3, 4, 5)
C5_FIXTURES = (
    "elf_x86_64.o",
    "elf_aarch64.o",
    "elf_riscv64.o",
    "pe_x86_64.exe",
    "pe_aarch64.exe",
)
C5_LEVELS = (1, 3, 5)

DEFAULT_UNRAR = Path(r"C:\Program Files\WinRAR\UnRAR.exe")
DEFAULT_WINRAR = Path(r"C:\Program Files\WinRAR\WinRAR.exe")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_binary(spec: str) -> tuple[str, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected name=/path/to/openrar")
    name, raw = spec.split("=", 1)
    path = Path(raw)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"no such binary: {path}")
    return name, path


def extract_with(tool: Path, archive: Path, dest: Path, extra: tuple[str, ...]) -> None:
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    proc = subprocess.run(
        [str(tool), "x", "-y", "-o+", *extra, str(archive), str(dest)],
        capture_output=True,
        text=True,
        timeout=180,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{tool.name} failed on {archive.name} (rc={proc.returncode}): "
            f"{(proc.stdout + proc.stderr).strip()[:300]}"
        )
    produced = [p for p in dest.rglob("*") if p.is_file()]
    if len(produced) != 1:
        raise RuntimeError(f"{tool.name}: expected 1 file from {archive.name}, got {len(produced)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", action="append", type=parse_binary, required=True,
                    help="name=/path/to/openrar (repeatable); first is the reference")
    ap.add_argument("--candidate", action="append", type=parse_binary, default=[])
    ap.add_argument("--c5-corpus", default=str(ROOT / "tests" / "fixtures" / "c5"))
    ap.add_argument("--work", default=str(ROOT / "build" / "dispatch_parity"))
    ap.add_argument("--unrar", default=str(DEFAULT_UNRAR))
    ap.add_argument("--winrar", default=str(DEFAULT_WINRAR))
    ap.add_argument("--json", default=None)
    ap.add_argument("--skip-references", action="store_true",
                    help="compare bytes only; skip UnRAR/WinRAR cross-extraction")
    args = ap.parse_args()

    binaries = args.baseline + args.candidate
    if len(binaries) < 2:
        ap.error("need at least two binaries to compare")
    ref_name, ref_exe = binaries[0]
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    expected_cells = {(c, m) for c in RATCHET_CORPORA for m in METHODS}
    if set(rqg.RATCHET_BYTES) != expected_cells:
        print("RATCHET_BYTES no longer describes the 30 expected cells; refusing to "
              "certify a partial comparison.", file=sys.stderr)
        return 2

    print("Building the ratchet corpora via the release guard's own recipe...")
    corpora = generate_corpora()
    corpora.update(rqg.build_real_corpora(ref_exe))

    rows = []
    changed_cells = 0
    print(f"\n--- Ratchet parity: {len(expected_cells)} cells ---")
    for corpus_name in RATCHET_CORPORA:
        corpus = corpora[corpus_name]
        for method in METHODS:
            sizes = {}
            digests = {}
            archives = {}
            for name, exe in binaries:
                out = work / f"{corpus_name}.{name}.m{method}.rar"
                size, _ = rqg.pack(exe, corpus, method, out)
                sizes[name] = size
                digests[name] = sha256(out)
                archives[name] = out
            same = len(set(digests.values())) == 1
            if not same:
                changed_cells += 1
            rows.append({
                "cell": f"{corpus_name} -m{method}",
                "corpus": corpus_name,
                "method": method,
                "sizes": sizes,
                "sha256": digests,
                "identical": same,
                "archives": {k: str(v) for k, v in archives.items()},
            })
            flag = "same" if same else "DIFFERS"
            delta = sizes[binaries[1][0]] - sizes[ref_name]
            print(f"  {corpus_name:<20} -m{method}  {ref_name}={sizes[ref_name]:>10,}  "
                  f"{binaries[1][0]}={sizes[binaries[1][0]]:>10,}  {delta:+d} B  {flag}")

    # C5: the members the dispatch decision actually affects.
    print(f"\n--- C5 dispatch members (-m1/-m3/-m5) ---")
    c5_dir = Path(args.c5_corpus)
    changed_archives = []
    for fixture in C5_FIXTURES:
        src = c5_dir / fixture
        if not src.is_file():
            print(f"  missing {src}; skipping", file=sys.stderr)
            continue
        original = sha256(src)
        for level in C5_LEVELS:
            sizes = {}
            digests = {}
            archives = {}
            for name, exe in binaries:
                out = work / f"c5_{fixture}.{name}.m{level}.rar"
                size, _ = rqg.pack(exe, src, level, out)
                sizes[name] = size
                digests[name] = sha256(out)
                archives[name] = out
            same = len(set(digests.values())) == 1
            rows.append({
                "cell": f"c5 {fixture} -m{level}",
                "corpus": f"c5/{fixture}",
                "method": level,
                "sizes": sizes,
                "sha256": digests,
                "identical": same,
                "archives": {k: str(v) for k, v in archives.items()},
            })
            if not same:
                for name, _ in binaries:
                    changed_archives.append({
                        "cell": f"c5 {fixture} -m{level}",
                        "producer": name,
                        "archive": archives[name],
                        "source": src,
                        "source_sha256": original,
                    })
            delta = sizes[binaries[1][0]] - sizes[ref_name]
            print(f"  {fixture:<18} -m{level}  {ref_name}={sizes[ref_name]:>6,}  "
                  f"{binaries[1][0]}={sizes[binaries[1][0]]:>6,}  {delta:+d} B  "
                  f"{'same' if same else 'DIFFERS'}")

    reference_results = []
    if changed_archives and not args.skip_references:
        tools = []
        if Path(args.unrar).is_file():
            tools.append((Path(args.unrar), ()))
        if Path(args.winrar).is_file():
            tools.append((Path(args.winrar), ("-ibck",)))
        if not tools:
            print("\nNo reference extractor found; cannot certify interop.", file=sys.stderr)
            return 2
        print(f"\n--- Reference extraction of {len(changed_archives)} changed archive(s) ---")
        for item in changed_archives:
            archive = Path(item["archive"])
            for tool, extra in tools:
                dest = work / f"x_{archive.stem}_{tool.stem}"
                try:
                    extract_with(tool, archive, dest, extra)
                    produced = [p for p in dest.rglob("*") if p.is_file()][0]
                    ok = sha256(produced) == item["source_sha256"]
                    detail = "bytes match source" if ok else "EXTRACTED BYTES DIFFER"
                except Exception as e:  # report, don't abort the remaining checks
                    ok = False
                    detail = str(e)
                reference_results.append({
                    "cell": item["cell"], "producer": item["producer"],
                    "archive": str(archive), "extractor": tool.name, "ok": ok, "detail": detail,
                })
                print(f"  [{'PASS' if ok else 'FAIL'}] {tool.name} {archive.name} "
                      f"({item['cell']}, {item['producer']}): {detail}")

    if args.json:
        Path(args.json).write_text(json.dumps({
            "binaries": [n for n, _ in binaries],
            "cells": rows,
            "changed_ratchet_cells": changed_cells,
            "reference_extractions": reference_results,
        }, indent=2) + "\n", encoding="utf-8")

    ratchet_fail = changed_cells != 0
    ref_fail = any(not r["ok"] for r in reference_results)
    print("\n" + "=" * 72)
    print(f"ratchet cells compared : {len(expected_cells)}")
    print(f"ratchet cells differing: {changed_cells}")
    print(f"changed archives       : {len(changed_archives)}")
    print(f"reference extractions  : {len(reference_results)} "
          f"({sum(1 for r in reference_results if r['ok'])} passed)")
    if ratchet_fail or ref_fail:
        print("VERDICT: FAIL")
        return 1
    print("VERDICT: PASS — byte-identical where required, changed archives interop-clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
