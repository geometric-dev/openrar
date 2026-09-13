# 50 MB compression matrix: OpenRAR vs WinRAR, ST vs MT, CPU ext on/off.
# Two timed runs per row, best (min) wall time reported; archives verified
# afterwards. WinRAR's SIMD dispatch is not controllable, so the ext
# dimension applies to OpenRAR only.
import os, subprocess, time, hashlib, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(ROOT, "perf", "payload")
OUT = os.path.join(ROOT, "perf")
OPENRAR = os.environ.get("OPENRAR_EXE")
if not OPENRAR or not os.path.exists(OPENRAR):
    for c in [
        os.path.join(ROOT, "..", "build", "Release", "openrar.exe"),
        os.path.join(ROOT, "..", "build", "openrar.exe"),
        os.path.join(ROOT, "..", "build", "openrar"),
        os.path.join(ROOT, "..", "build", "openrar64", "Release", "openrar.exe"),
    ]:
        if os.path.exists(c):
            OPENRAR = c
            break
RAR = r"C:\Program Files\WinRAR\rar.exe"
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"

def run(cmd, env=None, cwd=OUT):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd), r.returncode)
        print(r.stdout.decode(errors="replace")[-500:], r.stderr.decode(errors="replace")[-500:])
        sys.exit(1)
    return dt

def bench_openrar(label, mt_args, disable_ext):
    env = dict(os.environ)
    if disable_ext:
        env["OPENRAR_DISABLE_CPU_EXT"] = "1"
    times = []
    for i in range(2):
        arc = f"o_{label}_{i}.rar"
        if os.path.exists(os.path.join(OUT, arc)):
            os.remove(os.path.join(OUT, arc))
        times.append(run([OPENRAR, "a", arc, "-m3", "-q"] + mt_args + [PAYLOAD], env=env))
    arc = f"o_{label}_0.rar"
    run([OPENRAR, "t", arc, "-q"], env=env)
    size = os.path.getsize(os.path.join(OUT, arc))
    return min(times), size, arc

def bench_winrar(label, mt_args):
    times = []
    for i in range(2):
        arc = f"w_{label}_{i}.rar"
        if os.path.exists(os.path.join(OUT, arc)):
            os.remove(os.path.join(OUT, arc))
        times.append(run([RAR, "a", "-m3", "-inul"] + mt_args + [arc, PAYLOAD]))
    run([UNRAR, "t", "-y", f"w_{label}_0.rar"], cwd=OUT)
    size = os.path.getsize(os.path.join(OUT, f"w_{label}_0.rar"))
    return min(times), size

def content_hash(arc, unrar=False):
    ex = os.path.join(OUT, "x_" + os.path.basename(arc))
    if os.path.exists(ex):
        import shutil; shutil.rmtree(ex)
    if unrar:
        subprocess.run([UNRAR, "x", "-y", arc, ex + "/"], cwd=OUT, capture_output=True, check=True)
    else:
        subprocess.run([OPENRAR, "x", arc, ex, "-q"], cwd=OUT, capture_output=True, check=True)
    h = hashlib.sha256()
    for root, _, files in os.walk(ex):
        for n in sorted(files):
            with open(os.path.join(root, n), "rb") as f:
                h.update(f.read())
    import shutil; shutil.rmtree(ex)
    return h.hexdigest()[:16]

rows = []
t, s, arc = bench_openrar("st_ext", ["-mt1"], False); rows.append(("OpenRAR  ST  ext ON ", t, s, arc, False))
t, s, arc = bench_openrar("st_noext", ["-mt1"], True); rows.append(("OpenRAR  ST  ext OFF", t, s, arc, False))
t, s, arc = bench_openrar("mt_ext", ["-mt8"], False); rows.append(("OpenRAR  MT8 ext ON ", t, s, arc, False))
t, s, arc = bench_openrar("mt_noext", ["-mt8"], True); rows.append(("OpenRAR  MT8 ext OFF", t, s, arc, False))
t, s = bench_winrar("st", ["-mt1"]); rows.append(("WinRAR   ST  -mt1   ", t, s, None, False))
t, s = bench_winrar("mt", []); rows.append(("WinRAR   MT  default", t, s, None, False))

base = content_hash("o_st_ext_0.rar")
noext = content_hash("o_st_noext_0.rar")
win = content_hash("w_st_0.rar", unrar=True)

print()
print(f"{'row':22} {'best sec':>9} {'size':>12} {'MB/s':>7}")
for label, t, s, _, _ in rows:
    print(f"{label:22} {t:9.2f} {s:12,} {50.1/t:7.1f}")
print()
print("content sha256 (extracted):")
print("  openrar ext ON :", base)
print("  openrar ext OFF:", noext, "(identical)" if base == noext else "(DIFFERENT!)")
print("  winrar ST      :", win, "(identical)" if base == win else "(DIFFERENT)")
