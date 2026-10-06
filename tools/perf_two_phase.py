#!/usr/bin/env python3
# v1.37.0 two-phase decode claim measurements (v1.34 SS3 protocol).
#
# Paired deltas, min-statistic, alternating order, single session, fresh
# binaries. Rows: decode-bound text members (Rar-made 512 KiB-block streams
# AND our-encoder streams) at -mt4/-mt8 vs our sequential, plus the honest
# no-regression rows (4-block member, stored, zeros, -mt1) and the UnRAR
# cross-baseline. The baseline to beat (Gate 0 SS0): Rar 7.20 measures
# 1.29x over our decode-bound extraction at -mt4 (1.50x at 128 MiB dict),
# 0.95x on a 4-block member, 1.07x on overhead-bound zeros.
import os
import subprocess
import sys
import tempfile
import time
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OPENRAR = ROOT / "build" / "openrar64" / "Release" / "openrar.exe"
UNRAR = Path(r"C:\Program Files\WinRAR\UnRAR.exe")
RAR = Path(r"C:\Program Files\WinRAR\Rar.exe")

PAIRS = 15  # interleaved pairs per configuration; min-statistic per side


def make_text(n, seed=0x137):
    import random
    rng = random.Random(seed)
    vocab = [f"w{i}" for i in range(600)] + [
        "the", "quick", "brown", "window", "archive", "decode", "parallel", "range"
    ]
    out = bytearray()
    while len(out) < n:
        line = ""
        while len(line) <= 72:
            line += vocab[rng.randrange(len(vocab))]
            if len(line) > 72:
                break
            line += " "
        out += line.encode() + b"\n"
    return bytes(out[:n])


def make_random(n, seed=7):
    import random
    rng = random.Random(seed)
    return bytes(rng.getrandbits(8) for _ in range(n))


def run(cmd, env=None, cwd=None):
    e = os.environ.copy()
    if env:
        for k, v in env.items():
            if v is None:
                e.pop(k, None)
            else:
                e[k] = v
    t0 = time.perf_counter()
    r = subprocess.run(cmd, env=e, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dt = (time.perf_counter() - t0) * 1000.0
    if r.returncode != 0:
        raise RuntimeError(f"command failed rc={r.returncode}: {cmd}")
    return dt


def measure(seq_env, par_env, archive, workdir, pairs=PAIRS):
    """min-of-pairs, alternating order (v1.34 SS3)."""
    seq_ms, par_ms = [], []
    out = workdir / "out"
    for i in range(pairs):
        order = [(seq_env, seq_ms), (par_env, par_ms)]
        if i % 2 == 1:
            order.reverse()
        for env_dict, sink in order:
            e = {k: v for k, v in env_dict.items() if v is not None}
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            sink.append(run([str(OPENRAR), "x", "-y", str(archive), str(out) + os.sep], env=e))
    s, p = min(seq_ms), min(par_ms)
    return s, p, s / p if p else 0.0


def main():
    td = Path(tempfile.mkdtemp(prefix="pd_measure_"))
    MB = 1024 * 1024
    print(f"=== v1.37.0 two-phase claims (paired deltas, min of {PAIRS}, alternating) ===",
          flush=True)
    print(f"host: {os.cpu_count()} logical CPUs", flush=True)

    text64 = td / "text64.bin"
    if not text64.exists():
        text64.write_bytes(make_text(64 * MB))
    text2 = td / "text2.bin"
    text2.write_bytes(make_text(2 * MB, seed=0x55))
    zeros64 = td / "zeros64.bin"
    if not zeros64.exists():
        zeros64.write_bytes(bytes(64 * MB))
    rand8 = td / "rand8.bin"
    rand8.write_bytes(make_random(8 * MB))

    rar64 = td / "rar64m3.rar"
    run([str(RAR), "a", "-ep", "-m3", "-md2m", str(rar64), str(text64)], cwd=str(td))
    rar64_128 = td / "rar64md128.rar"
    run([str(RAR), "a", "-ep", "-m3", "-md128m", str(rar64_128), str(text64)], cwd=str(td))
    our64 = td / "our64m3.rar"
    run([str(OPENRAR), "a", "-y", "-ep", "-m3", "-md2m", str(our64), str(text64)], cwd=str(td))
    rar2 = td / "rar2m3.rar"
    run([str(RAR), "a", "-ep", "-m3", "-md2m", str(rar2), str(text2)], cwd=str(td))
    rarz = td / "rarz.rar"
    run([str(RAR), "a", "-ep", "-m3", "-md2m", str(rarz), str(zeros64)], cwd=str(td))
    rarr = td / "rarr.rar"
    run([str(RAR), "a", "-ep", "-m0", str(rarr), str(rand8)], cwd=str(td))

    SEQ = {"OPENRAR_NO_PARALLEL_DECODE": "1"}
    PAR4 = {"OPENRAR_PARALLEL_DECODE_THREADS": "4"}
    PAR8 = {"OPENRAR_PARALLEL_DECODE_THREADS": "8"}
    PAR1 = {"OPENRAR_PARALLEL_DECODE_THREADS": "1"}

    def row(label, archive, seq_env, par_env, pairs=PAIRS):
        s, p, ratio = measure(seq_env, par_env, archive, td, pairs)
        print(f"{label:44s} seq={s:8.1f}ms par={p:8.1f}ms ratio={ratio:5.2f}x", flush=True)

    def unrar_row(label, archive):
        out = td / "out"
        ms = []
        for i in range(PAIRS):
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            ms.append(run([str(UNRAR), "x", "-y", "-mt4", str(archive), str(out) + os.sep]))
        print(f"{label:44s} unrar-mt4={min(ms):8.1f}ms", flush=True)

    print("--- primary rows (decode-bound text) ---", flush=True)
    row("rar64m3 -mt4 vs seq (baseline beat: 1.29x)", rar64, SEQ, PAR4)
    unrar_row("  unrar -mt4 on rar64m3", rar64)
    row("rar64m3 -mt8 vs seq", rar64, SEQ, PAR8)
    row("rar64md128m -mt4 vs seq (baseline: 1.50x)", rar64_128, SEQ, PAR4)
    row("our64m3 -mt4 vs seq", our64, SEQ, PAR4)

    print("--- no-regression rows ---", flush=True)
    row("rar2m3 (4-block member) -mt4 vs seq", rar2, SEQ, PAR4)
    row("rarz zeros64 -mt8 vs seq (ref ~1.07x)", rarz, SEQ, PAR8)
    row("rarr stored -mt4 vs seq", rarr, SEQ, PAR4)
    row("rar64m3 -mt1 vs seq (parity)", rar64, SEQ, PAR1, pairs=5)

    print("=== done ===", flush=True)
    shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
