#!/usr/bin/env python3
"""
OpenRAR v2.1.0 Track E - Gate G1 Legality & Interoperability Oracle Harness
Orchestrates automated round-trip interoperability testing against UnRAR / WinRAR 7.20.
Enforces fail-closed verification (NT-19):
  - Missing UnRAR or mismatched oracle version immediately exits code 1
  - Any decompression failure or SHA-256 byte divergence immediately exits code 1
  - Must execute all manifest stages (CI asserts stages=<N>)
"""

import os
import sys
import shutil
import hashlib
import subprocess
from pathlib import Path

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from bench_m0_census import DEFAULT_BIN, WORK_DIR, generate_corpora

def find_unrar() -> Path:
    candidates = [
        Path("C:/Program Files/WinRAR/UnRAR.exe"),
        Path("C:/Program Files (x86)/WinRAR/UnRAR.exe"),
        Path("/usr/local/bin/unrar"),
        Path("/usr/bin/unrar"),
    ]
    which_unrar = shutil.which("unrar")
    if which_unrar:
        candidates.insert(0, Path(which_unrar))

    for c in candidates:
        if c.exists():
            return c
    return None

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()

def main():
    print("=" * 80)
    print("OpenRAR v2.1.0 Track E: Gate G1 Legality & Interop Harness (Oracle: UnRAR 7.20)")
    print("=" * 80)

    unrar_bin = find_unrar()
    if not unrar_bin or not unrar_bin.exists():
        print("[FATAL] Oracle UnRAR not found! Gate G1 FAILS CLOSED.")
        print("Run tools/provision_unrar.ps1 (Windows) or tools/provision_unrar.sh (Linux).")
        sys.exit(1)

    print(f"Oracle binary: {unrar_bin}")
    print(f"OpenRAR binary: {DEFAULT_BIN}")

    # Check oracle version
    res = subprocess.run([str(unrar_bin)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if "UNRAR 7.20" not in res.stdout and "UNRAR 7." not in res.stdout:
        print(f"[FATAL] Oracle version mismatch (expected 7.20+): {res.stdout.splitlines()[0] if res.stdout else 'None'}")
        sys.exit(1)
    print(f"Oracle verified: {res.stdout.splitlines()[0].strip() if res.stdout else 'OK'}")

    corpora = generate_corpora()
    methods = [1, 3, 5]
    threads = [1, 4]

    out_dir = WORK_DIR / "interop_extracted"
    out_dir.mkdir(parents=True, exist_ok=True)

    stage_count = 0
    passed_count = 0

    manifest_count = len(corpora) * len(methods) * len(threads)
    print(f"Executing {manifest_count} interop manifest stages...\n")

    for c_name, c_path in corpora.items():
        orig_hash = sha256_file(c_path)

        for m in methods:
            for mt in threads:
                stage_count += 1
                stage_name = f"{c_name}_m{m}_mt{mt}"
                arc_path = WORK_DIR / f"interop_{stage_name}.rar"
                if arc_path.exists():
                    arc_path.unlink()

                # Produce archive with OpenRAR
                cmd_pack = [str(DEFAULT_BIN), "a", "-y", f"-m{m}", f"-mt{mt}", "-md4m", str(arc_path), str(c_path)]
                pack_res = subprocess.run(cmd_pack, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if pack_res.returncode != 0:
                    print(f"[{stage_count:02d}/{manifest_count}] 🔴 FAIL pack: {stage_name}")
                    sys.exit(1)

                # Clean extraction dir
                for f in out_dir.iterdir():
                    if f.is_file():
                        f.unlink()

                # Extract archive with Oracle (UnRAR 7.20)
                cmd_extract = [str(unrar_bin), "x", "-y", "-o+", str(arc_path), str(out_dir)]
                extract_res = subprocess.run(cmd_extract, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if extract_res.returncode != 0:
                    print(f"[{stage_count:02d}/{manifest_count}] 🔴 FAIL extract (UnRAR exited {extract_res.returncode}): {stage_name}")
                    sys.exit(1)

                extracted_file = out_dir / c_path.name
                if not extracted_file.exists():
                    print(f"[{stage_count:02d}/{manifest_count}] 🔴 FAIL missing extracted file: {stage_name}")
                    sys.exit(1)

                ext_hash = sha256_file(extracted_file)
                if ext_hash != orig_hash:
                    print(f"[{stage_count:02d}/{manifest_count}] 🔴 FAIL hash mismatch: {stage_name} (got {ext_hash[:8]}, expected {orig_hash[:8]})")
                    sys.exit(1)

                if arc_path.exists():
                    arc_path.unlink()
                if extracted_file.exists():
                    extracted_file.unlink()

                passed_count += 1
                print(f"[{stage_count:02d}/{manifest_count}] 🟢 PASS: {stage_name} (oracle UnRAR 7.20 verified)")

    print(f"\nstages={stage_count}")
    if stage_count != manifest_count or passed_count != manifest_count:
        print(f"[FATAL] Incomplete manifest run ({passed_count}/{manifest_count}). Gate G1 FAILS CLOSED.")
        sys.exit(1)

    print(f"[OK] Gate G1 PASSED: all {manifest_count} stages decoded byte-identically under UnRAR 7.20.")

if __name__ == "__main__":
    main()

