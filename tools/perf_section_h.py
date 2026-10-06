#!/usr/bin/env python3
# v1.38.0 M4: section-H-only re-record (the extraction rows of
# perf_vs_winrar.py) — same commands, run counts and output schema as the
# harness's section H, without re-running the compression sections (the
# archives they produce are unchanged: the encoder is untouched this arc,
# and the existing on-disk archives are the pack-side byte-stability
# baseline).
import importlib.util
import json
import shutil
import statistics
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "perf_vs_winrar", Path(__file__).resolve().parent / "perf_vs_winrar.py")
pw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pw)

QUICK = "--quick" in sys.argv

if not pw.OPENRAR:
    print("openrar.exe not found — build Release first.")
    raise SystemExit(1)
if not (pw.DATA / "canonical").exists():
    print("canonical corpus missing — run perf_vs_winrar.py once to generate it.")
    raise SystemExit(1)

pw.PERF.mkdir(parents=True, exist_ok=True)
pw.disclose_host()
print(f"host: {pw.HOST['cpu']} | {pw.HOST['cores']} logical cores | {pw.HOST['ram_gb']} GB")
print(f"tools: {pw.HOST['openrar']} | {pw.HOST['winrar']} | {pw.HOST['unrar']}")

rows = []
CORPORA_FPS = pw.corpus_fingerprints()
results_path = pw.PERF / "results.json"
if results_path.exists():
    try:
        old = json.load(open(results_path))
        rows = [r for r in old.get("rows", []) if not r["label"].startswith("extract_")]
        prev_fps = old.get("corpora")
        if prev_fps and prev_fps != CORPORA_FPS:
            print("WARNING: corpus fingerprints drifted since the stored rows; "
                  "pack-side rows are retained but cross-era comparisons are void.")
    except (json.JSONDecodeError, OSError):
        rows = []

ex_rows = []
arc_o = pw.DATA / "out" / "canonical_o_m3_st.rar"
arc_w = pw.DATA / "out" / "canonical_w_m3_st.rar"
ex_matrix = [
    ("o", arc_o, "extract_m3_o_st", [pw.OPENRAR, "x", "-y", "-q", "-mt1", str(arc_o)]),
    ("o", arc_o, "extract_m3_o_mt4", [pw.OPENRAR, "x", "-y", "-q", "-mt4", str(arc_o)]),
    ("o", arc_w, "extract_m3_w_st", [pw.OPENRAR, "x", "-y", "-q", "-mt1", str(arc_w)]),
    ("o", arc_w, "extract_m3_w_mt4", [pw.OPENRAR, "x", "-y", "-q", "-mt4", str(arc_w)]),
    ("unrar", arc_w, "extract_m3_unrar_st", [pw.UNRAR, "x", "-y", "-mt1", str(arc_w)]),
    ("unrar", arc_w, "extract_m3_unrar_mt4", [pw.UNRAR, "x", "-y", "-mt4", str(arc_w)]),
    ("winrar", arc_w, "extract_m3_winrar_mt4", [pw.RAR, "x", "-y", "-inul", "-mt4", str(arc_w)]),
]
for eng, arc, label, base in ex_matrix:
    runs = []
    n = 2 if QUICK else 4
    for i in range(n):
        ex = pw.DATA / "out" / f"ex_{label}_{i}"
        shutil.rmtree(ex, ignore_errors=True)
        ex.mkdir()
        cmd = base + [str(ex) + "\\"] if eng != "o" else base + [str(ex)]
        dt = pw.run(cmd)
        if i > 0 or QUICK:
            runs.append(dt)
        shutil.rmtree(ex, ignore_errors=True)
        print(f"    {label} run{i}: {dt:.2f}s")
    ex_rows.append({"engine": eng, "label": label, "runs": runs,
                    "median": statistics.median(runs), "min": min(runs),
                    "size": 0, "corpus": "canonical"})
    rows = [r for r in rows if r["label"] != label] + ex_rows
    json.dump({"host": pw.HOST, "quick": QUICK, "corpora": CORPORA_FPS, "rows": rows},
              open(results_path, "w"), indent=1)

print(f"section H re-recorded: {len(ex_rows)} rows -> {results_path}")
