# Deterministic 50 MB canonical payload: text / code / binary thirds.
import random, os, sys

out_dir = sys.argv[1] if len(sys.argv) > 1 else "payload"
os.makedirs(out_dir, exist_ok=True)

WORDS = ("the quick open rar archive stream window huffman literal distance slot "
         "compress block table symbol length code bit reader flush buffer sector "
         "recovery parity shard cipher salt iterate vector match chain hash").split()
rng = random.Random(20260912)

# text: ~17 MB of varied prose-ish lines (compressible, realistic redundancy)
with open(os.path.join(out_dir, "text.txt"), "w") as f:
    while f.tell() < 17 * 1024 * 1024:
        n = rng.randint(6, 24)
        f.write(" ".join(rng.choice(WORDS) for _ in range(n)) + "\n")

# code: ~17 MB from real source files, repeated to exercise long-range matches
src = []
for root, _, files in os.walk("src"):
    for name in files:
        if name.endswith((".cpp", ".hpp", ".h")):
            p = os.path.join(root, name)
            try:
                src.append(open(p, encoding="utf-8", errors="replace").read())
            except OSError:
                pass
blob = "\n".join(src) if src else "int main() { return 0; }\n"
with open(os.path.join(out_dir, "code.cpp"), "w") as f:
    while f.tell() < 17 * 1024 * 1024:
        f.write(blob)

# binary: ~16 MB pseudo-random (incompressible tail)
with open(os.path.join(out_dir, "random.bin"), "wb") as f:
    remaining = 16 * 1024 * 1024
    while remaining > 0:
        chunk = bytes(rng.getrandbits(8) for _ in range(min(1 << 20, remaining)))
        f.write(chunk)
        remaining -= len(chunk)

total = sum(os.path.getsize(os.path.join(out_dir, n)) for n in os.listdir(out_dir))
print(f"payload: {total/1024/1024:.1f} MB in {out_dir}")
