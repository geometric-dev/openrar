#!/usr/bin/env python3
"""Build and verify the archive-level C9 members.

The source members are byte-pinned by `generate_c9.py` + `manifest.json`.
These three archive forms are NOT byte-pinned: archive headers carry source
mtimes, so their bytes are not reproducible across checkouts. They are built
at test time from the pinned sources and verified structurally and by
round-trip instead:

  solid_c9.rar   - SOLID flag set, two members, both extract to source
  enc_c9.rar     - AES-256 data encryption; right password round-trips,
                   wrong password FAILS (a pass-through cipher would pass a
                   digest-only check, so the negative case is the test)
  vol_c9.rar     - multi-volume set, >1 part, concatenated extraction equals
                   source

Usage:
    python tests/fixtures/c9/build_c9_archives.py --generate \
        --src build/c9-corpus --exe build/openrar64/Release/openrar.exe \
        --filter-probe build/Release/filter_probe.exe --out build/c9-archives

`--generate` materialises the members into `--src` first and checks them
against the COMMITTED manifest before building anything. Without it, `--src`
must already hold a corpus with that manifest. Either way a missing corpus is
an error: this script never skips, and the CMake/CTest entry (c9_build) runs
it so an unbuilt corpus cannot read as a pass.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

import generate_c9

HERE = os.path.dirname(os.path.abspath(__file__))
PASSWORD = "c9-decoder-corpus"
MIB = 1 << 20


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(exe, args, expect_ok=True, cwd=None):
    p = subprocess.run([exe] + args, capture_output=True, text=True, cwd=cwd)
    if expect_ok and p.returncode != 0:
        raise RuntimeError("openrar %s failed (%d)\n%s\n%s"
                           % (" ".join(args), p.returncode, p.stdout, p.stderr))
    return p


def extract(exe, archive, outdir, password=None):
    """Extract with `cwd=outdir` and destination `.`.

    `-o` is WinRAR's overwrite switch (`-o+`/`-o-`), NOT an output-directory
    flag; passing `-o<dir>` silently extracts into the process cwd. Running
    with cwd=outdir makes a misparse land somewhere harmless instead of in
    the repository root.
    """
    os.makedirs(outdir, exist_ok=True)
    args = ["x", "-y"]
    if password:
        args.append("-p" + password)
    args.extend([os.path.abspath(archive), "."])
    return run(exe, args, expect_ok=False, cwd=outdir)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("  ok  " + msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="dir holding generate_c9.py output")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--filter-probe", required=True,
                    help="filter_probe binary; asserts the C7 members actually "
                         "trip the E8/DELTA detectors")
    ap.add_argument("--out", required=True)
    ap.add_argument("--generate", action="store_true",
                    help="materialise the corpus into --src and check it against "
                         "the committed manifest before building archives")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    if args.generate:
        os.makedirs(src, exist_ok=True)
        print("  ..  generating C9 members into %s" % src)
        generate_c9.build(src)
        # Verify against the COMMITTED manifest, not the one build() just
        # wrote: checking a manifest against itself would bless any drift.
        committed = os.path.join(HERE, "manifest.json")
        if not os.path.isfile(committed):
            raise SystemExit("C9 committed manifest missing: %s" % committed)
        shutil.copyfile(committed, os.path.join(src, "manifest.json"))
        if generate_c9.verify(src) != 0:
            raise SystemExit("C9 corpus does not match the committed manifest")
    elif not os.path.isfile(os.path.join(src, "manifest.json")):
        raise SystemExit("C9 corpus absent: %s has no manifest.json "
                         "(run with --generate, or point --src at a corpus)" % src)
    args.src = src

    exe = os.path.abspath(args.exe)
    check(os.path.isfile(exe), "openrar binary present: %s" % exe)
    os.makedirs(args.out, exist_ok=True)
    # A second run must behave like the first: `a` on an existing solid archive
    # fails, and stale volume parts would satisfy the >1-part check without
    # this run producing any. Both are this script's own outputs.
    for stale in os.listdir(args.out):
        if stale.startswith(("solid_c9", "enc_c9", "vol_c9", "filter_c9")):
            os.remove(os.path.join(args.out, stale))
    text = os.path.join(args.src, "text_64m.bin")
    mini = os.path.join(args.src, "minified_1m.js")
    for p in (text, mini):
        check(os.path.exists(p), "source present: %s" % os.path.basename(p))

    # C7 gate: a member that the detector does not fire on is not C7 coverage.
    # This runs before any archive is built, so a non-triggering member fails
    # loudly instead of being round-tripped and counted.
    probe = os.path.abspath(args.filter_probe)
    check(os.path.isfile(probe), "filter probe present: %s" % probe)
    for member, expect in (("e8_8m.bin", "e8"), ("audio_6ch_8m.wav", "delta")):
        path = os.path.join(args.src, member)
        check(os.path.exists(path), "source present: %s" % member)
        q = run(probe, ["--expect=" + expect, path], expect_ok=False)
        for line in (q.stdout or "").strip().splitlines():
            print("  ..  " + line)
        check(q.returncode == 0,
              "%s triggers the %s detector (probe rc=%d)"
              % (member, expect.upper(), q.returncode))

    with tempfile.TemporaryDirectory() as tmp:
        # --- solid -------------------------------------------------------
        solid = os.path.join(args.out, "solid_c9.rar")
        run(exe, ["a", "-ep", "-s", "-m3", "-mt1", "-y", solid, text, mini])
        ex = os.path.join(tmp, "solid")
        p = extract(exe, solid, ex)
        check(p.returncode == 0, "solid_c9.rar extracts")
        check(sha256(os.path.join(ex, "text_64m.bin")) == sha256(text),
              "solid: text_64m.bin round-trips")
        check(sha256(os.path.join(ex, "minified_1m.js")) == sha256(mini),
              "solid: minified_1m.js round-trips")

        # --- encrypted ---------------------------------------------------
        enc = os.path.join(args.out, "enc_c9.rar")
        run(exe, ["a", "-ep", "-m3", "-mt1", "-p" + PASSWORD, "-y", enc, mini])
        ex = os.path.join(tmp, "enc")
        p = extract(exe, enc, ex, PASSWORD)
        check(p.returncode == 0, "enc_c9.rar extracts with the right password")
        check(sha256(os.path.join(ex, "minified_1m.js")) == sha256(mini),
              "encrypted: minified_1m.js round-trips")
        ex_bad = os.path.join(tmp, "enc_bad")
        p_bad = extract(exe, enc, ex_bad, "wrong-password")
        leaked = os.path.exists(os.path.join(ex_bad, "minified_1m.js"))
        check(p_bad.returncode != 0 or not leaked,
              "encrypted: wrong password fails closed (rc=%d, leaked=%s)"
              % (p_bad.returncode, leaked))

        # --- span-crossing member ---------------------------------------
        # The C9 member is the real input to this claim: pack it and require
        # the PACKED size to clear the driver's engagement floor. The constant
        # lives in parallel_decode.cpp (SPAN_PACKED_FLOOR = 4 MiB, gated at
        # 4x); two_phase_tests pins that floor, this pins that the member
        # actually reaches it with the shipped binary.
        span = os.path.join(args.src, "span_cross_24m.bin")
        check(os.path.exists(span), "source present: span_cross_24m.bin")
        span_pack = os.path.join(tmp, "span.rar")
        run(exe, ["a", "-ep", "-m3", "-md64m", "-mt1", "-y", span_pack, span])
        span_packed = os.path.getsize(span_pack)
        floor = 4 * 4 * MIB  # 4 x SPAN_PACKED_FLOOR
        check(span_packed >= floor,
              "span_cross_24m.bin packs to %d MiB >= %d MiB span floor"
              % (span_packed // MIB, floor // MIB))

        # --- multi-volume ------------------------------------------------
        vol = os.path.join(args.out, "vol_c9.rar")
        # Part count is a function of PACKED size, not source size. The text
        # member compresses well, so a hardcoded -v8m produced a single part
        # and proved nothing. Probe the packed size, then split it in two.
        pack_probe = os.path.join(tmp, "probe.rar")
        run(exe, ["a", "-ep", "-m3", "-mt1", "-y", pack_probe, text])
        packed = os.path.getsize(pack_probe)
        part_mib = max(1, ((packed // 2) + MIB - 1) // MIB)
        print("  ..  packed %d bytes -> -v%dm" % (packed, part_mib))
        run(exe, ["a", "-ep", "-m3", "-mt1", "-v%dm" % part_mib, "-y", vol, text])
        parts = sorted(f for f in os.listdir(args.out) if f.startswith("vol_c"))
        check(len(parts) >= 2, "vol_c9: %d volume parts (derived from packed size)"
              % len(parts))
        first = os.path.join(args.out, parts[0])
        ex = os.path.join(tmp, "vol")
        p = extract(exe, first, ex)
        check(p.returncode == 0, "vol_c9: extraction from part 1 succeeds")
        extracted = os.path.join(ex, "text_64m.bin")
        check(os.path.exists(extracted) and sha256(extracted) == sha256(text),
              "vol_c9: concatenated volumes round-trip to source")

        # --- filtered MT (C7 emission) -----------------------------------
        # Detection firing in filter_probe's member mode says the input is
        # filterable. This asserts the ARCHIVE carries the region: the probe
        # decodes with the worker-path driver and reads the filter records the
        # decoder actually parsed (type, channels, offsets, coverage). Packed
        # with -mt4, because EXP-FILTER-SAFE-MT is about the MT encoder.
        filt = os.path.join(args.out, "filter_c9.rar")
        run(exe, ["a", "-ep", "-m3", "-md64m", "-mt4", "-y", filt,
                  os.path.join(args.src, "e8_8m.bin"),
                  os.path.join(args.src, "audio_6ch_8m.wav"), text])
        for member, expect, channels in (("e8_8m.bin", "e8", "1"),
                                         ("audio_6ch_8m.wav", "delta", "12"),
                                         ("text_64m.bin", "none", "0")):
            q = run(probe, ["--archive=" + filt, "--entry=" + member,
                            "--expect=" + expect, "--channels=" + channels],
                    expect_ok=False)
            for line in (q.stdout or "").strip().splitlines():
                print("  ..  " + line)
            check(q.returncode == 0,
                  "filter_c9: %s emits %s (channels %s, full coverage)"
                  % (member, expect.upper(), channels))
        # The gate must bite: a member with no filter region cannot satisfy an
        # emission assertion, so the same probe run with the opposite
        # expectation has to fail.
        q = run(probe, ["--archive=" + filt, "--entry=text_64m.bin",
                        "--expect=delta", "--channels=12"], expect_ok=False)
        check(q.returncode != 0,
              "filter_c9: the emission gate is not vacuous (no-filter entry "
              "fails an E8/DELTA expectation)")

    print("C9 archive forms verified.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print("FAIL: %s" % e)
        sys.exit(1)
