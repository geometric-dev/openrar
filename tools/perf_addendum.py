# Addendum: isolate the ext-sensitive kernels on the same 50 MB payload.
#   store mode  (-m0): file CRC32 dominates            -> PCLMUL vs slicing
#   encrypted   (-m0 -p): PBKDF2 + AES-CBC on top      -> AES-NI vs scalar
# Best of 2 runs, same methodology as perf_matrix.py.
import os, subprocess, time, sys

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

def run(cmd, env=None):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=OUT, env=env, capture_output=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd)); sys.exit(1)
    return dt

def bench(label, args, disable_ext, runs=2):
    env = dict(os.environ)
    if disable_ext:
        env["OPENRAR_DISABLE_CPU_EXT"] = "1"
    times = []
    for i in range(runs):
        arc = f"a_{label}_{i}.rar"
        if os.path.exists(os.path.join(OUT, arc)):
            os.remove(os.path.join(OUT, arc))
        times.append(run([OPENRAR, "a", arc, "-q"] + args + [PAYLOAD], env=env))
    run([OPENRAR, "t", arc, "-q"], env=env)
    return min(times)

rows = [
    ("store  -m0  ext ON ", bench("s1", ["-m0", "-mt1"], False)),
    ("store  -m0  ext OFF", bench("s0", ["-m0", "-mt1"], True)),
    ("enc    -m0 -p ext ON ", bench("e1", ["-m0", "-mt1", "-ppassword"], False)),
    ("enc    -m0 -p ext OFF", bench("e0", ["-m0", "-mt1", "-ppassword"], True)),
]
print()
for label, t in rows:
    print(f"{label:22} {t:8.2f} s   {50.1/t:6.1f} MB/s")
