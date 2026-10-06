#!/usr/bin/env python3
"""OpenRAR vs WinRAR benchmark harness (v1.30 documentation refresh).

Protocol (documented in PERFORMANCE.md):
  - deterministic corpora generated once (seeded), kept out of git;
  - every config: 1 untimed warm-up run + 3 timed runs, MEDIAN reported
    (min and spread shown too); fresh output files each run, pre-deleted
    OUTSIDE the timed window;
  - every config's archive is extracted once per engine and the content
    SHA256 is compared against the source tree (cross-extraction parity);
  - host disclosure: CPU, logical cores, RAM, OS, tool versions.

Covers: methods m0/m1/m3/m5 (ST), MT (m3 -mt4), solid (-s), multivolume
(-v32m), OpenRAR-only CDC dedup (-cdc), a 512 MB zeros ratio run, a 1 GB
canonical run, and extraction (openrar x / UnRAR x / WinRAR rar x).

Usage:  python tools/perf_vs_winrar.py [--quick]
        --quick: 1 warm-up + 1 timed run (smoke/CI use; numbers not quoted).

Outputs: tools/perf/results.json, tools/perf/PERFORMANCE.md (summary tables;
the hand-curated PERFORMANCE.md lives at the repo root and is updated from
this output).
"""

import hashlib
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PERF = REPO / "tools" / "perf"
DATA = PERF / "data"
QUICK = "--quick" in sys.argv

OPENRAR = None
for c in (REPO / "build" / "openrar64" / "Release" / "openrar.exe",
          REPO / "build" / "Release" / "openrar.exe",
          REPO / "build" / "openrar.exe"):
    if c.exists():
        OPENRAR = str(c)
        break
RAR = r"C:\Program Files\WinRAR\rar.exe"
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"

MB = 1024 * 1024


def run(cmd, cwd=None):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, capture_output=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        print(f"  FAILED({' '.join(cmd)}) rc={r.returncode}")
        print(r.stdout.decode(errors="replace")[-600:])
        print(r.stderr.decode(errors="replace")[-600:])
        raise SystemExit(1)
    return dt


# ── corpora ──────────────────────────────────────────────────────────────────
import random

WORDS = ("the quick open rar archive stream window huffman literal distance slot "
         "compress block table symbol length code bit reader flush buffer sector "
         "recovery parity shard cipher salt iterate vector match chain hash delta "
         "filter chunk window flag header payload volume digest".split())


def gen_text(path, target):
    rng = random.Random(20260912)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        while f.tell() < target:
            n = rng.randint(6, 24)
            f.write(" ".join(rng.choice(WORDS) for _ in range(n)) + "\n")


def gen_code(path, target, src_blob):
    with open(path, "w", encoding="utf-8", errors="replace", newline="\n") as f:
        while f.tell() < target:
            f.write(src_blob)


def gen_random(path, target, seed):
    rng = random.Random(seed)
    with open(path, "wb") as f:
        remaining = target
        while remaining > 0:
            chunk = rng.randbytes(min(1 * MB, remaining))
            f.write(chunk)
            remaining -= len(chunk)


def gen_zeros(path, target):
    with open(path, "wb") as f:
        chunk = bytes(1 * MB)
        remaining = target
        while remaining > 0:
            n = min(len(chunk), remaining)
            f.write(chunk[:n])
            remaining -= n


def gen_source_blob():
    blob = []
    for root, _, files in os.walk(REPO / "src"):
        for name in files:
            if name.endswith((".cpp", ".hpp", ".h")):
                try:
                    blob.append((REPO / root / name).read_text(encoding="utf-8", errors="replace"))
                except OSError:
                    pass
    return "\n".join(blob) if blob else "int main() { return 0; }\n"


def gen_corpora():
    DATA.mkdir(parents=True, exist_ok=True)
    # canonical: three 1/3 files, ~128 MB total (single-file + multi-file runs)
    canon = DATA / "canonical"
    if not canon.exists():
        canon.mkdir(parents=True)
        gen_text(canon / "text.txt", 43 * MB)
        gen_code(canon / "code.cpp", 43 * MB, gen_source_blob())
        gen_random(canon / "random.bin", 42 * MB, 20260913)
        print("  generated canonical/ (128 MB)")
    # mixed: 36 files, sizes 2-6 MB, incl. duplicate pairs (for CDC/-oi dedup)
    mixed = DATA / "mixed"
    if not mixed.exists():
        mixed.mkdir(parents=True)
        rng = random.Random(20260914)
        src_blob = gen_source_blob()
        for i in range(36):
            kind = i % 6
            size = (2 + (i % 5)) * MB
            if kind == 0:
                gen_text(mixed / f"doc_{i:02}.txt", size)
            elif kind == 1:
                gen_code(mixed / f"src_{i:02}.cpp", size, src_blob)
            elif kind == 2:
                gen_random(mixed / f"bin_{i:02}.dat", size, 20260915 + i)
            elif kind == 3:
                gen_zeros(mixed / f"pad_{i:02}.raw", size)
            elif kind == 4:
                gen_text(mixed / f"report_{i:02}.log", size)
            else:
                gen_random(mixed / f"img_{i:02}.bin", size, 20260930 + i)
        # duplicate pair for dedup switches (-cdc implies -oi)
        shutil.copyfile(mixed / "doc_00.txt", mixed / "doc_00_copy.txt")
        print("  generated mixed/ (36 files)")
    # zeros512: single 512 MB zero file (ratio highlight)
    zeros = DATA / "zeros512"
    if not zeros.exists():
        zeros.mkdir(parents=True)
        gen_zeros(zeros / "zeros.bin", 512 * MB)
        print("  generated zeros512/ (512 MB)")
    # big: 1 GB canonical thirds (headline refresh)
    big = DATA / "big"
    if not big.exists():
        big.mkdir(parents=True)
        gen_text(big / "text.txt", 340 * MB)
        gen_code(big / "code.cpp", 340 * MB, gen_source_blob())
        gen_random(big / "random.bin", 344 * MB, 20260916)
        print("  generated big/ (1 GB)")


def dir_of(corpus):
    return DATA / corpus


def tree_hash(base: Path, files=None) -> str:
    h = hashlib.sha256()
    names = files if files is not None else sorted(p.name for p in base.rglob("*") if p.is_file())
    for name in sorted(names):
        h.update(name.encode("utf-8", "replace"))
        h.update(hashlib.sha256((base / name).read_bytes()).digest())
    return h.hexdigest()[:16]


def corpus_fingerprints() -> dict:
    # The corpora are regenerated per host and `code.cpp` embeds the repo's
    # src tree, so sizes/times from different runs are only comparable within
    # the same corpus era. Fingerprint every corpus into results.json so
    # cross-run diffs can detect drift instead of misreading it as a
    # regression (v1.31 M1 lesson: the solid/volumes "size jump" was corpus
    # drift, not encoder behavior).
    fps = {}
    for name in ("canonical", "mixed", "zeros512", "big"):
        base = DATA / name
        if base.exists():
            fps[name] = tree_hash(base)
    return fps


def extract_and_hash(arc, engine, workdir):
    ex = workdir / ("x_" + Path(arc).stem + "_" + engine)
    if ex.exists():
        shutil.rmtree(ex, ignore_errors=True)
    ex.mkdir()
    if engine == "openrar":
        run([OPENRAR, "x", "-y", "-q", str(arc), str(ex)])
    elif engine == "unrar":
        run([UNRAR, "x", "-y", str(arc), str(ex) + os.sep])
    else:
        run([RAR, "x", "-y", "-inul", str(arc), str(ex) + os.sep])
    return tree_hash(ex), ex


def archive_files(out_path: Path):
    """The archive file (or all volume parts) for an output base name."""
    return sorted(out_path.parent.glob(out_path.stem + "*.rar"))


def bench(engine, label, argv, files, out_path, verify_with):
    """1 warm-up + 3 timed runs (median reported); fresh outputs each run."""
    runs = []
    for i in range(4 if not QUICK else 2):
        for p in archive_files(out_path):
            p.unlink()
        dt = run(argv, cwd=str(dir_of(files)))
        if i > 0 or QUICK:
            runs.append(dt)
        print(f"    run{i}: {dt:.2f}s")
    made = archive_files(out_path)
    size = sum(p.stat().st_size for p in made)
    # cross-extraction verification: the archive made by THIS engine, extracted
    # by BOTH reference engines, must hash equal to the source tree.
    base = dir_of(files)
    fl = sorted(f for f in os.listdir(base) if os.path.isfile(base / f))
    src_h = tree_hash(base, fl)
    hashes = {}
    first = made[0] if len(made) == 1 else next(
        (p for p in made if ".part" in p.name and ".part01." in p.name or ".part1." in p.name),
        made[0])
    for ve in verify_with:
        h, ex = extract_and_hash(first, ve, out_path.parent)
        hashes[ve] = h
        shutil.rmtree(ex, ignore_errors=True)
        if h != src_h:
            print(f"  VERIFY FAIL: {ve} hash mismatch for {label}")
            raise SystemExit(1)
    return {
        "engine": engine, "label": label, "runs": runs,
        "median": statistics.median(runs), "min": min(runs),
        "spread": (max(runs) - min(runs)) / statistics.median(runs) if len(runs) > 1 else 0.0,
        "size": size, "verified": sorted(hashes.values()),
    }


def dir_size(base: Path) -> int:
    return sum(p.stat().st_size for p in base.rglob("*") if p.is_file())


HOST = {
    "cpu": "",
    "cores": os.cpu_count(),
    "ram_gb": 0.0,
    "os": f"{platform.system()} {platform.release()}",
    "openrar": "",
    "winrar": "",
    "unrar": "",
}


def disclose_host():
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                           r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
        HOST["cpu"] = winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
    except OSError:
        HOST["cpu"] = platform.processor() or "unknown"
    try:
        HOST["ram_gb"] = round(int(subprocess.run(
            ["powershell", "-Command", "(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"],
            capture_output=True, text=True).stdout.strip()) / (1 << 30), 1)
    except OSError:
        pass
    HOST["openrar"] = subprocess.run([OPENRAR, "--version"], capture_output=True,
                                     text=True).stdout.splitlines()[0].strip()
    wr = subprocess.run([RAR], capture_output=True, text=True)
    HOST["winrar"] = next((l for l in (wr.stdout + wr.stderr).splitlines() if l.strip()), "").strip()
    ur = subprocess.run([UNRAR], capture_output=True, text=True)
    HOST["unrar"] = next((l for l in (ur.stdout + ur.stderr).splitlines() if l.strip()), "").strip()


# ── matrix ───────────────────────────────────────────────────────────────────
def main():
    if sys.platform != "win32":
        print("Windows-only harness (WinRAR comparison).")
        return 1
    if not OPENRAR:
        print("openrar.exe not found — build Release first.")
        return 1
    PERF.mkdir(parents=True, exist_ok=True)
    print("generating corpora (deterministic; skipped when present)...")
    gen_corpora()
    disclose_host()
    print(f"host: {HOST['cpu']} | {HOST['cores']} logical cores | {HOST['ram_gb']} GB | "
          f"{HOST['os']}")
    print(f"tools: {HOST['openrar']} | {HOST['winrar']} | {HOST['unrar']}")

    rows = []
    CORPORA_FPS = corpus_fingerprints()
    t_start = time.perf_counter()

    def cfg(engine, corpus, files, label, openrar_args, verify=("openrar", "unrar")):
        """engine in {openrar, winrar}; openrar_args carry the switches (the
        switch names are identical across both tools by design)."""
        out = DATA / "out" / f"{corpus}_{label}.rar"
        out.parent.mkdir(parents=True, exist_ok=True)
        # Compression runs with cwd = the corpus dir and RELATIVE file names:
        # WinRAR keeps everything after the drive for absolute paths, which
        # would poison the cross-extraction tree comparison.
        if engine == "openrar":
            argv = [OPENRAR, "a", "-q"] + openrar_args + [str(out)]
        else:
            argv = [RAR, "a", "-inul", "-y"] + openrar_args + [str(out)]
        argv = argv + list(files)
        row = bench(engine, label, argv, corpus, out, verify)
        row["corpus"] = corpus
        row["input_mb"] = round(dir_size(dir_of(corpus)) / MB, 1)
        rows.append(row)
        json.dump({"host": HOST, "quick": QUICK, "corpora": CORPORA_FPS, "rows": rows},
                  open(PERF / "results.json", "w"), indent=1)
        return row

    CANON = ["text.txt", "code.cpp", "random.bin"]

    # A. method sweep, ST
    for m in (0, 1, 3, 5):
        cfg("openrar", "canonical", CANON, f"o_m{m}_st", [f"-m{m}", "-mt1"])
        cfg("winrar", "canonical", CANON, f"w_m{m}_st", [f"-m{m}", "-mt1"])
    # B. MT (all 4 logical cores)
    cfg("openrar", "canonical", CANON, "o_m3_mt", ["-m3", "-mt4"])
    cfg("winrar", "canonical", CANON, "w_m3_mt", ["-m3", "-mt4"])
    # C. solid
    cfg("openrar", "canonical", CANON, "o_m3_solid", ["-m3", "-s", "-mt1"])
    cfg("winrar", "canonical", CANON, "w_m3_solid", ["-m3", "-s", "-mt1"])
    # D. multivolume 32 MB parts (digit-free label: OpenRAR's volume-name
    # derivation replaces the last digit-run in the stem)
    cfg("openrar", "canonical", CANON, "o_volumes", ["-m3", "-v32m", "-mt1"],
        verify=("openrar", "unrar"))
    cfg("winrar", "canonical", CANON, "w_volumes", ["-m3", "-v32m", "-mt1"],
        verify=("openrar", "unrar"))
    # E. mixed corpus, MT (per-file parallelism) + CDC dedup (openrar-only)
    MIXED = sorted(f for f in os.listdir(dir_of("mixed")))
    cfg("openrar", "mixed", MIXED, "o_mix_m3_mt", ["-m3", "-mt4"])
    cfg("winrar", "mixed", MIXED, "w_mix_m3_mt", ["-m3", "-mt4"])
    cfg("openrar", "mixed", MIXED, "o_mix_cdc", ["-cdc", "-m3", "-mt4"])
    # F. zeros 512 MB, m1 (ratio highlight)
    cfg("openrar", "zeros512", ["zeros.bin"], "o_zeros_m1", ["-m1", "-mt1"])
    cfg("winrar", "zeros512", ["zeros.bin"], "w_zeros_m1", ["-m1", "-mt1"])
    # G. big 1 GB canonical, m3 MT (headline refresh)
    BIG = ["text.txt", "code.cpp", "random.bin"]
    cfg("openrar", "big", BIG, "o_big_m3_mt", ["-m3", "-mt4"])
    cfg("winrar", "big", BIG, "w_big_m3_mt", ["-m3", "-mt4"])
    # H. extraction: the canonical m3 ST archives, sequential AND parallel
    # (v1.37.2: openrar honors -mt on x; unrar/winrar pinned -mt1 and -mt4
    # for the serial/parallel baselines).
    ex_rows = []
    arc_o = DATA / "out" / "canonical_o_m3_st.rar"   # OpenRAR-made stream
    arc_w = DATA / "out" / "canonical_w_m3_st.rar"   # Rar-made stream
    ex_matrix = [
        ("o", arc_o, "extract_m3_o_st", [OPENRAR, "x", "-y", "-q", "-mt1", str(arc_o)]),
        ("o", arc_o, "extract_m3_o_mt4", [OPENRAR, "x", "-y", "-q", "-mt4", str(arc_o)]),
        ("o", arc_w, "extract_m3_w_st", [OPENRAR, "x", "-y", "-q", "-mt1", str(arc_w)]),
        ("o", arc_w, "extract_m3_w_mt4", [OPENRAR, "x", "-y", "-q", "-mt4", str(arc_w)]),
        ("unrar", arc_w, "extract_m3_unrar_st", [UNRAR, "x", "-y", "-mt1", str(arc_w)]),
        ("unrar", arc_w, "extract_m3_unrar_mt4", [UNRAR, "x", "-y", "-mt4", str(arc_w)]),
        ("winrar", arc_w, "extract_m3_winrar_mt4", [RAR, "x", "-y", "-inul", "-mt4", str(arc_w)]),
    ]
    for eng, arc, label, base in ex_matrix:
        runs = []
        n = 2 if QUICK else 4
        for i in range(n):
            ex = DATA / "out" / f"ex_{label}_{i}"
            shutil.rmtree(ex, ignore_errors=True)
            ex.mkdir()
            cmd = base + [str(ex) + os.sep] if eng != "o" else base + [str(ex)]
            dt = run(cmd)
            if i > 0 or QUICK:
                runs.append(dt)
            shutil.rmtree(ex, ignore_errors=True)
            print(f"    {label} run{i}: {dt:.2f}s")
        ex_rows.append({"engine": eng, "label": label, "runs": runs,
                        "median": statistics.median(runs), "min": min(runs),
                        "size": 0, "corpus": "canonical"})
        json.dump({"host": HOST, "quick": QUICK, "corpora": CORPORA_FPS, "rows": rows + ex_rows},
                  open(PERF / "results.json", "w"), indent=1)
    rows = rows + ex_rows

    json.dump({"host": HOST, "quick": QUICK, "corpora": CORPORA_FPS, "rows": rows},
              open(PERF / "results.json", "w"), indent=1)
    print(f"done in {(time.perf_counter() - t_start) / 60:.1f} min; "
          f"{len(rows)} configs -> tools/perf/results.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
