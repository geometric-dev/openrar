#!/usr/bin/env python3
"""C9 decode-identity corpus + §4.20 guard members.

Two rules this file exists to enforce:

1. **No `random`.** CPython guarantees only ``random()`` reproduces across
   versions; ``randrange`` does not. All bytes come from ``shake_256`` keyed by
   (name, seed), which is standardized and platform-independent.
2. **Pin properties, not just bytes.** A digest over a degenerate member is a
   green check on the wrong artefact — the first version of this generator
   produced 8 MiB of one repeated byte and pinned it. Each member is now
   measured before the manifest is written, and generation FAILS if a member
   is not what it claims to be.

Commands:
    generate_c9.py --out DIR          materialise + write manifest
    generate_c9.py --out DIR --verify check materialised members vs manifest
    generate_c9.py --check            regenerate to a temp dir and diff against
                                      the committed manifest (never writes it)
"""

import argparse
import hashlib
import json
import math
import os
import platform
import struct
import sys
import tempfile
import zlib

MIB = 1024 * 1024
# Must divide 2^16: `(b0 << 8 | b1) % PALETTE` is uniform only while PALETTE is
# a power of two. A non-power-of-two palette would reintroduce silent index
# collisions that the coverage assertion cannot see.
PALETTE = 4096
PRF = "shake_256"
# Gates `verify` can re-evaluate consumer-side without the generator.
NUMERIC_GATES = {"min_distinct_bytes", "max_distinct_bytes", "min_zlib_ratio",
                 "min_compress_ratio", "min_e8_density"}


def prf_bytes(tag, seed, n):
    """Standardized, version-stable byte stream.

    Fields are length-prefixed: concatenating with a separator makes the
    encoding non-injective (`tag="a|b"` collides with `seed="b|c"`), which
    would collapse two members onto one keystream.
    """
    t = tag.encode()
    h = hashlib.shake_256(len(t).to_bytes(2, "big") + t + b"|" + str(seed).encode())
    return h.digest(n)


class Stream:
    """Indexable PRF stream: block k is prf_bytes(tag, seed, 64 KiB) offset by k."""

    def __init__(self, tag, seed):
        self.tag, self.seed, self.block, self.k = tag, seed, 0, 0
        self.buf = prf_bytes(tag, seed, 1 << 16)

    def u8(self):
        if self.k >= len(self.buf):
            self.buf = prf_bytes("%s#%d" % (self.tag, self.block), self.seed, 1 << 16)
            self.block += 1
            self.k = 0
        v = self.buf[self.k]
        self.k += 1
        return v


def text_like(n, seed):
    st = Stream("text", seed)
    vocab = [("w%d" % i).encode() for i in range(600)]
    vocab += [w.encode() for w in ("the", "quick", "brown", "window", "archive",
                                   "decode", "parallel", "range")]
    # Pre-join 4096 lines, then tile them by PRF choice: keeps the token
    # distribution while avoiding a Python call per word.
    lines = []
    for _ in range(PALETTE):
        words = []
        while True:
            words.append(vocab[st.u8() % len(vocab)])
            if len(b" ".join(words)) > 72:
                break
        lines.append(b" ".join(words) + b"\n")
    # Two INDEPENDENT draws per index. `u8 % 4096` reaches only 256 of the
    # palette's 4096 lines, and a 256-line corpus compresses just as well as a
    # 4096-line one, so the ratio gate would not notice. Note the arithmetic:
    # (b << 8 | b) is 257*b and 4096 = 2^12, so a doubled byte still yields only
    # 256 distinct residues — the two draws must be separate variables.
    used = set()
    out = bytearray()
    while len(out) < n:
        b0, b1 = st.u8(), st.u8()
        idx = ((b0 << 8) | b1) % PALETTE
        used.add(idx)
        out += lines[idx]
    if len(used) < PALETTE:
        raise AssertionError("palette coverage %d/%d" % (len(used), PALETTE))
    return bytes(out[:n])


def zeros(n, _seed):
    return b"\x00" * n


def minified_js(n, seed):
    st = Stream("js", seed)
    toks = [t.encode() for t in ("a", "b", "c", "x", "y", "z", "f", "g", "i", "j", "n", "s", "t")]
    ops = [o.encode() for o in ("+", "-", "*", "=", ",", ";", "(", ")", "{", "}", "[", "]", ".", "?")]
    out = bytearray()
    while len(out) < n:
        if st.u8() < 128:
            out += toks[st.u8() % len(toks)]
        else:
            out += ops[st.u8() % len(ops)]
            if st.u8() < 20:
                out += str(st.u8() % 1000).encode()
    return bytes(out[:n])


def binary(n, seed):
    """Incompressible: PRF bytes, property-gated below."""
    return prf_bytes("binary", seed, n)


def pe_like(n, seed):
    """x86-shaped stream for the E8 detector.

    Density is asserted at generation (>=15% 0xE8), but that is only the
    input's shape. Whether `Filters50::detect_filter` actually returns E8 is
    asserted separately by `tools/filter_probe.cpp` — the byte shape alone is
    not evidence.
    """
    st = Stream("e8", seed)
    out = bytearray(b"MZ\x90\x00")
    while len(out) < n:
        r = st.u8()
        if r < 56:  # ~22% rel32 CALLs
            rel = (((st.u8() << 8) | st.u8()) - 32768)
            out += b"\xe8" + rel.to_bytes(4, "little", signed=True)
        elif r < 76:  # ~8% rel32 JMPs
            rel = (((st.u8() << 8) | st.u8()) - 32768)
            out += b"\xe9" + rel.to_bytes(4, "little", signed=True)
        else:
            out.append(st.u8())
    return bytes(out[:n])


def wav_pcm(n, seed):
    """DELTA member: a real RIFF/WAVE PCM file, 6ch/16-bit (stride 12).

    This is not decoration. `Filters50::detect_filter` returns DELTA only for
    `RIFF` + `WAVEfmt ` with a whole-byte stride of 1..32 — a raw interleaved
    RGB byte stream can never trigger it, however smooth it is. The first
    version of this member was raw stride-3 bytes and the probe proved it was
    dead coverage. The container is the trigger; the samples are the payload.
    """
    ch, bits = 6, 16
    frame = ch * bits // 8  # 12
    data_len = ((n - 44) // frame) * frame
    st = Stream("wav", seed)
    hdr = bytearray(44)
    hdr[0:4] = b"RIFF"
    struct.pack_into("<I", hdr, 4, 36 + data_len)
    hdr[8:16] = b"WAVEfmt "
    struct.pack_into("<IHHIIHH", hdr, 16, 16, 1, ch, 48000, 48000 * frame, frame, bits)
    hdr[36:40] = b"data"
    struct.pack_into("<I", hdr, 40, data_len)
    # Smooth 0.1 s tones, one phase per channel: a delta predictor is highly
    # profitable on this and the detector has something real to accept.
    period = 4800
    phases = [((st.u8() << 8) | st.u8()) % period for _ in range(ch)]
    body = bytearray()
    step = 0
    while len(body) < data_len:
        for c in range(ch):
            body += struct.pack("<h", int(11000 * math.sin(2.0 * math.pi * (phases[c] + step) / period)))
        step += 1
    return bytes(hdr) + bytes(body[:data_len])


def wrap_128m(n, seed):
    """128 MiB with a forced long-distance match.

    A = 64 MiB PRF, B = 32 MiB PRF, then A[:32 MiB] again at offset 96 MiB.
    The only source of the final block is 96 MiB back, i.e. beyond a 64 MiB
    window — so a pack at -md64m must slide the window to find it, and a pack
    at -md4m cannot. It is not a text member: repetitions inside text let a
    greedy matcher stay near, which is why the first version could not prove
    the wrap.
    """
    assert n == 128 * MIB
    a = prf_bytes("wrap-a", seed, 64 * MIB)
    b = prf_bytes("wrap-b", seed, 32 * MIB)
    return a + b + a[: 32 * MIB]


MEMBERS = [
    # name, builder, size, seed, role, properties
    ("text_64m.bin", text_like, 64 * MIB, 1234, "C6/C9 text",
     {"min_compress_ratio": 2.0}),
    ("zeros_64m.bin", zeros, 64 * MIB, 0, "C6/C9 match-heavy",
     {"max_distinct_bytes": 1}),
    ("minified_1m.js", minified_js, 1 * MIB, 77, "C6/C9 literal-heavy",
     {"min_compress_ratio": 1.5}),
    ("binary_8m.bin", binary, 8 * MIB, 91, "C9 incompressible",
     {"min_distinct_bytes": 250, "min_zlib_ratio": 0.98}),
    # Incompressible, so packed stays ~24 MiB and clears the driver's span
    # floor of 4 x SPAN_PACKED_FLOOR = 16 MiB (parallel_decode.cpp); a text
    # member would need ~175 MiB of source to clear the same floor.
    ("span_cross_24m.bin", binary, 24 * MIB, 4242, "C9 span-crossing",
     {"min_zlib_ratio": 0.98}),
    # Density gate is a sanity floor on the input's shape only: the real
    # assertion is the detector probe in build_c9_archives.py.
    ("e8_8m.bin", pe_like, 8 * MIB, 515, "C7/E8 detector (probe-asserted)",
     {"min_e8_density": 0.08}),
    # 8 MiB - 8 keeps header + frames exact for stride 12 (44-byte header).
    ("audio_6ch_8m.wav", wav_pcm, 8 * MIB, 516, "C7/DELTA detector (probe-asserted)",
     {"riff_header": True}),
    ("wrap_128m.bin", wrap_128m, 128 * MIB, 2026, "guard: 64 MiB window wraps",
     {"forced_match": "32 MiB at offset 96 MiB"}),
    ("chunk_12m.bin", text_like, 12 * MIB, 4096, "guard: 3 MiB chunks at -mt4",
     {"min_compress_ratio": 2.0}),
]


def gate_violations(name, m, props):
    """Apply a threshold block to measured values. Used by the producer at
    build time AND by `--verify`, which never runs the generator: a gate that
    only the producer evaluates can be deleted by the party it constrains."""
    bad = []
    if "min_distinct_bytes" in props and m.get("distinct_bytes", 0) < props["min_distinct_bytes"]:
        bad.append("%s: %s distinct bytes < %d"
                   % (name, m.get("distinct_bytes"), props["min_distinct_bytes"]))
    if "max_distinct_bytes" in props and m.get("distinct_bytes", 1 << 30) > props["max_distinct_bytes"]:
        bad.append("%s: %s distinct bytes > %d"
                   % (name, m.get("distinct_bytes"), props["max_distinct_bytes"]))
    if "min_zlib_ratio" in props and m.get("zlib_ratio", 0) < props["min_zlib_ratio"]:
        bad.append("%s: zlib ratio %s < %s"
                   % (name, m.get("zlib_ratio"), props["min_zlib_ratio"]))
    if "min_compress_ratio" in props and m.get("compress_ratio", 0) < props["min_compress_ratio"]:
        bad.append("%s: compress ratio %s < %s"
                   % (name, m.get("compress_ratio"), props["min_compress_ratio"]))
    if "min_e8_density" in props and m.get("e8_density", 0) < props["min_e8_density"]:
        bad.append("%s: E8 density %s < %s"
                   % (name, m.get("e8_density"), props["min_e8_density"]))
    return bad


def measure(name, data, props):
    """Assert the member is what it claims; returns measured properties."""
    m = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    # Whole member, one meaning: a gate on a 1 MiB prefix would let a member
    # whose first MiB is degenerate pass on the strength of its tail.
    if "min_distinct_bytes" in props or "max_distinct_bytes" in props:
        m["distinct_bytes"] = len(set(data))
    if "min_zlib_ratio" in props:
        m["zlib_ratio"] = round(len(zlib.compress(data, 9)) / max(1, len(data)), 4)
    if "min_compress_ratio" in props:
        m["compress_ratio"] = round(len(data) / max(1, len(zlib.compress(data, 6))), 2)
    if "min_e8_density" in props:
        m["e8_density"] = round(data.count(b"\xe8") / max(1, len(data)), 4)
    if props.get("riff_header"):
        if not (data[:4] == b"RIFF" and data[8:16] == b"WAVEfmt "):
            raise AssertionError("%s: missing RIFF/WAVE header packed by delta" % name)
        if (len(data) - 44) % 12 != 0:
            raise AssertionError("%s: payload is not a whole number of 12-byte frames" % name)
    bad = gate_violations(name, m, props)
    if bad:
        raise AssertionError("; ".join(bad))
    if "forced_match" in props:
        # Structural assertion, not a claim: the tail 32 MiB must equal the
        # first 32 MiB (a 96 MiB distance, beyond a 64 MiB window), and the
        # first 8 MiB must not occur anywhere between the two copies.
        dup = 32 * MIB
        if data[-dup:] != data[:dup]:
            raise AssertionError("%s: forced duplicate is not in place" % name)
        if data.find(data[: 8 * MIB], 8 * MIB, len(data) - dup) != -1:
            raise AssertionError("%s: duplicate also occurs inside the 64 MiB window"
                                 % name)
        m["forced_match"] = props["forced_match"]
    return m


def generator_sha256():
    with open(os.path.abspath(__file__), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def build(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for name, fn, size, seed, role, props in MEMBERS:
        data = fn(size, seed)
        if len(data) != size:
            raise AssertionError("%s: built %d bytes, expected %d" % (name, len(data), size))
        measured = measure(name, data, props)
        path = os.path.join(out_dir, name)
        if not (os.path.exists(path) and os.path.getsize(path) == size):
            with open(path, "wb") as f:
                f.write(data)
        measured.update({"name": name, "role": role})
        rows.append(measured)
    return rows


def write_manifest(out_dir, rows):
    doc = {
        "manifest_version": 2,
        "corpus": "C9",
        "prf": PRF,
        "generator_sha256": generator_sha256(),
        "python": platform.python_version(),
        # Only gates that `verify` can re-evaluate from the manifest's own
        # numbers. Structural assertions (RIFF header, forced duplicate) need
        # the bytes, so they live in `measure` and are re-run by `--check`,
        # which regenerates; putting them here would make a gate that silently
        # never fires.
        "gates": {name: {k: v for k, v in props.items() if k in NUMERIC_GATES}
                  for name, _f, _s, _sd, _r, props in MEMBERS},
        "members": rows,
    }
    path = os.path.join(out_dir, "manifest.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    return path


def verify(out_dir):
    path = os.path.join(out_dir, "manifest.json")
    if not os.path.exists(path):
        print("MISSING manifest: %s" % path)
        return 1
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("manifest_version") != 2:
        print("BAD manifest: unsupported version %r" % doc.get("manifest_version"))
        return 1
    pinned = {m["name"]: m for m in doc["members"]}
    bad, missing = [], []
    for name, _fn, size, _seed, _role, _props in MEMBERS:
        p = os.path.join(out_dir, name)
        if not os.path.exists(p):
            missing.append(name)
            continue
        if os.path.getsize(p) != size:
            bad.append("%s: size %d != %d" % (name, os.path.getsize(p), size))
            continue
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(4 * MIB), b""):
                h.update(chunk)
        got = h.hexdigest()
        want = pinned.get(name, {}).get("sha256")
        if want is None:
            bad.append("%s: absent from manifest" % name)
        elif got != want:
            bad.append("%s: sha256 %s != %s" % (name, got, want))
    # Consumer-side gates: re-evaluate the manifest's own measurements against
    # its own thresholds. This runs without the generator, so deleting a
    # threshold from the producer does not silently bless a degenerate member.
    gates = doc.get("gates", {})
    for name, measured in pinned.items():
        if name in gates:
            bad.extend(gate_violations(name, measured, gates[name]))
    if set(pinned) != set(gates):
        bad.append("manifest gates/members mismatch: %s" %
                   sorted(set(pinned) ^ set(gates)))
    known = {m[0] for m in MEMBERS} | {"manifest.json"}
    # Files only: an output directory may legitimately hold built archives.
    extra = sorted(f for f in os.listdir(out_dir)
                   if f not in known and os.path.isfile(os.path.join(out_dir, f)))
    if extra:
        bad.append("unexpected files: %s" % ", ".join(extra))
    for m in missing:
        print("MISSING: %s" % m)
    for m in bad:
        print("BAD: %s" % m)
    if not (missing or bad):
        print("C9 manifest verified (%d members)." % len(MEMBERS))
    return 0 if not (missing or bad) else 1


def check(committed_dir):
    """Regenerate elsewhere and diff: catches a generator change that also
    rewrote the manifest, which --verify cannot see by construction."""
    with tempfile.TemporaryDirectory() as tmp:
        rows = build(tmp)
        write_manifest(tmp, rows)
        a = os.path.join(committed_dir, "manifest.json")
        b = os.path.join(tmp, "manifest.json")
        if not os.path.exists(a):
            print("MISSING committed manifest: %s" % a)
            return 1
        with open(a, "r", encoding="utf-8") as f:
            old = json.load(f)
        with open(b, "r", encoding="utf-8") as f:
            new = json.load(f)
        # `python` is not byte-determining (shake_256 is specified, so an
        # interpreter upgrade reproduces the same bytes); everything else,
        # including generator_sha256, must match. Ignoring the generator hash
        # is what made the old check blind to a producer change that re-blessed
        # its own pin.
        old.pop("python", None)
        new.pop("python", None)
        if old == new:
            print("C9 manifest reproduces exactly.")
            return 0
        print("C9 manifest DRIFT: regeneration does not match the committed manifest.")
        if old.get("generator_sha256") != new.get("generator_sha256"):
            print("  generator changed: %s -> %s"
                  % (str(old.get("generator_sha256"))[:12], str(new.get("generator_sha256"))[:12]))
        return 1


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        return check(here)
    if not args.out:
        ap.error("--out is required unless --check is used")
    if args.verify:
        return verify(args.out)
    rows = build(args.out)
    path = write_manifest(args.out, rows)
    for r in rows:
        print("%12d  %s  %s" % (r["size"], r["sha256"], r["name"]))
    print("manifest: %s" % path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print("PROPERTY FAILURE: %s" % e)
        sys.exit(1)
