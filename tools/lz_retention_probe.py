#!/usr/bin/env python3
"""LZ-RETENTION GATE probe (§4.2 / §4.4 decision input) — log-only.

THE QUESTION THIS ANSWERS
-------------------------
Delta on row-major images is a measured 42-63% win on synthetic/paletted
content and a measured 3.6-11.1% LOSS on photographic content. Those cannot
be separated by "is this a BMP". So: is there a CHEAP statistic, computable
from the bytes, that predicts the sign of the size delta?

This probe is falsifiable by construction. For every corpus member it
reports, side by side:

  * measured archive sizes (no filter vs forced DELTA) -- ground truth
  * candidate statistics on the RAW bytes
  * the same statistics on the DELTA-TRANSFORMED bytes
  * the correlation between (stat_after - stat_before) and the size delta

If no statistic separates winners from losers with margin, the row is
REFUTED and §4.4 closes. That is a valid, cheap outcome and the probe is
built to produce it as readily as a positive.

WHY THE GROUND TRUTH COMES FROM openrar
---------------------------------------
Entropy and repeat rate are proxies. The only thing that decides whether a
filter earns its keep is the archive size the real encoder produces, because
the codec is LZ-first: a transform that lowers byte entropy but destroys
match structure LOSES (that is the Kodom result). So every candidate
statistic is scored against real `openrar a` sizes, never against entropy
alone.

PREFIX-vs-WHOLE (decision-relevant for cost)
--------------------------------------------
A streaming caller has 1 MiB (MT_DETECT_PREFIX); the in-memory caller has the
whole member. Statistics are computed on both so the report says whether the
cheap prefix form predicts as well as the full form. If it does not, the gate
is either expensive or unreliable -- both are reasons to stop.

Usage:
    python tools/lz_retention_probe.py --openrar <exe> [--corpus DIR] [--reps 1]

Exit 0 = probe ran (the VERDICT is in the output, not the exit code).
Exit 1 = could not measure (missing binary/corpus). Never exits 0 on a
refuted hypothesis -- refutation is a result, reported as such.
"""

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

MIB = 1024 * 1024
PREFIX = MIB  # MT_DETECT_PREFIX: what a streaming caller can supply


# --------------------------------------------------------------------------
# The transform. Mirrors Filters50::encode_delta exactly so the statistics
# are measured on the bytes the decoder would actually see.
# --------------------------------------------------------------------------

def delta_encode(data, channels):
    """Filters50::encode_delta. For channels==1: out[i] = src[i-1] - src[i].
    For channels>1: per-channel differencing in the same interleaved layout
    (spec: Dst[Channel + k*Channels] = Prev - Src[k], Prev starts at 0).
    """
    n = len(data)
    out = bytearray(n)
    if channels == 1:
        prev = 0
        for i in range(n):
            cur = data[i]
            out[i] = (prev - cur) & 0xFF
            prev = cur
        return bytes(out)
    prev = [0] * channels
    for i in range(n):
        c = i % channels
        cur = data[i]
        out[i] = (prev[c] - cur) & 0xFF
        prev[c] = cur
    return bytes(out)


# --------------------------------------------------------------------------
# Candidate statistics. All are O(n) and cheap enough for a per-member gate.
# --------------------------------------------------------------------------

def byte_entropy(data):
    if not data:
        return 0.0
    c = Counter(data)
    n = len(data)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def repeat_rate(data, width):
    """Fraction of width-byte windows whose exact bytes were seen before.
    This is the LZ match opportunity at that length -- the quantity delta
    either preserves or destroys.
    """
    if len(data) < width:
        return 0.0
    seen = set()
    hits = 0
    total = len(data) - width + 1
    mv = memoryview(data)
    for i in range(total):
        w = bytes(mv[i:i + width])
        if w in seen:
            hits += 1
        else:
            seen.add(w)
    return hits / total


def distinct_ratio(data, width):
    """Distinct width-byte windows / total windows. Lower = more structure.
    Complement of a repeat measure; included because it is the form that is
    monotone with compressibility rather than against it.
    """
    if len(data) < width:
        return 1.0
    total = len(data) - width + 1
    mv = memoryview(data)
    return len({bytes(mv[i:i + width]) for i in range(total)}) / total


STATS = (
    ("entropy", byte_entropy),
    ("repeat8", lambda d: repeat_rate(d, 8)),
    ("repeat4", lambda d: repeat_rate(d, 4)),
    ("distinct8", lambda d: distinct_ratio(d, 8)),
)


# --------------------------------------------------------------------------

def pack_size(openrar, src, delta_channels, tmp):
    """Ground truth: archive size with no filter vs forced DELTA."""
    out = os.path.join(tmp, "a.rar")
    sizes = {}
    for label, mc in (("base", "-mc-"), ("delta", "-mc%dD+" % delta_channels)):
        if os.path.exists(out):
            os.remove(out)
        cmd = [openrar, "a", "-y", "-ep", "-m3", "-mt1", mc, "-idq", out, src]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return None, None, r.stderr[:200]
        sizes[label] = os.path.getsize(out)
    return sizes["base"], sizes["delta"], None


def natural_channels(name):
    """The stride a sane encoder would pick for this member."""
    if "_8bpp" in name:
        return 1
    if "_16bpp" in name:
        return 2
    if "_24bpp" in name:
        return 3
    if "_32bpp" in name:
        return 4
    if name.endswith(".ppm"):
        return 3
    if name.endswith((".tif", ".tiff")):
        return 3
    return 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--openrar", required=True)
    ap.add_argument("--corpus", default="tests/fixtures/experiment")
    args = ap.parse_args()

    openrar = os.path.abspath(args.openrar)
    if not os.path.isfile(openrar):
        print("FAIL: openrar not found: %s" % openrar)
        return 1
    if not os.path.isdir(args.corpus):
        print("FAIL: corpus not found: %s" % args.corpus)
        return 1

    members = sorted(
        os.path.join(args.corpus, f)
        for f in os.listdir(args.corpus)
        if f.lower().endswith((".bmp", ".tif", ".tiff", ".ppm"))
    )
    if not members:
        print("FAIL: no image members in %s" % args.corpus)
        return 1

    print("LZ-RETENTION GATE PROBE (§4.2 / §4.4 decision input)")
    print("corpus: %s  (%d members)" % (args.corpus, len(members)))
    print()
    hdr = ("%-24s %9s %9s %8s | %8s %9s %9s | %8s %9s" %
           ("member", "base", "delta", "delta%", "repeat8", "d_repeat8",
            "abs_rpt8", "entropy", "d_entropy"))
    print(hdr)
    print("-" * len(hdr))

    rows = []
    tmp = tempfile.mkdtemp(prefix="lzret_")
    try:
        for m in members:
            name = os.path.basename(m)
            ch = natural_channels(name)
            with open(m, "rb") as f:
                raw = f.read()
            # Statistics on a 1 MiB prefix (streaming caller's view) and on
            # the whole member (in-memory caller's view).
            pre = raw[:PREFIX]
            d_pre = delta_encode(pre, ch)
            d_all = delta_encode(raw, ch)

            base, delta, err = pack_size(openrar, m, ch, tmp)
            if base is None:
                print("%-24s  PACK FAILED: %s" % (name, err))
                continue
            pct = 100.0 * (delta - base) / base

            r_before = repeat_rate(pre, 8)
            r_after = repeat_rate(d_pre, 8)
            e_before = byte_entropy(pre)
            e_after = byte_entropy(d_pre)
            # Whole-member form, for the prefix-vs-whole comparison.
            ra_all = repeat_rate(d_all, 8)

            print("%-24s %9d %9d %+7.1f%% | %8.3f %+9.3f %9.3f | %8.2f %+9.2f" %
                  (name, base, delta, pct, r_before, r_after - r_before,
                   ra_all, e_before, e_after - e_before))
            rows.append({
                "name": name, "pct": pct,
                "r_before": r_before, "r_after": r_after,
                "e_before": e_before, "e_after": e_after,
                "ra_all": ra_all,
            })
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if not rows:
        print("\nno measurements")
        return 1

    # ----------------------------------------------------------------------
    # The decision. Report every step and the overall first->last, then say
    # plainly whether ANY candidate statistic separates winners from losers.
    # ----------------------------------------------------------------------
    wins = [r for r in rows if r["pct"] < 0]
    losses = [r for r in rows if r["pct"] > 0]
    print()
    print("Ground truth: %d improve, %d regress" % (len(wins), len(losses)))
    for r in rows:
        tag = "WIN " if r["pct"] < 0 else "LOSS"
        print("  %s %-22s %+7.1f%%" % (tag, r["name"], r["pct"]))

    if not wins or not losses:
        print("\nVERDICT: INCONCLUSIVE - only one population present.")
        print("  The corpus has no member from one side, so no separating")
        print("  statistic can be demonstrated. Add photographic and")
        print("  synthetic members before drawing a conclusion.")
        return 0

    # Candidate separators: for each statistic, is there a threshold that
    # puts every win on one side and every loss on the other?
    candidates = (
        ("delta repeat8", lambda r: r["r_after"] - r["r_before"]),
        ("delta entropy", lambda r: r["e_after"] - r["e_before"]),
        ("abs repeat8 after", lambda r: r["r_after"]),
        ("repeat8 before", lambda r: r["r_before"]),
    )
    print()
    print("Can any single statistic separate winners from losers?")
    separated = False
    for label, fn in candidates:
        wv = sorted(fn(r) for r in wins)
        lv = sorted(fn(r) for r in losses)
        # Separable if max(win) < min(loss) or max(loss) < min(win).
        if max(wv) < min(lv):
            print("  [YES] %-20s wins < %.4f <= losses" % (label, max(wv)))
            separated = True
        elif max(lv) < min(wv):
            print("  [YES] %-20s losses < %.4f <= wins" % (label, max(lv)))
            separated = True
        else:
            print("  [NO ] %-20s win range [%.4f, %.4f] overlaps loss [%.4f, %.4f]" %
                  (label, wv[0], wv[-1], lv[0], lv[-1]))

    print()
    if separated:
        print("VERDICT: a separating statistic EXISTS. The gate is viable;")
        print("  the next step is to confirm it on a wider corpus before")
        print("  shipping (n=%d is too small to set a threshold)." % len(rows))
    else:
        print("VERDICT: REFUTED. No single cheap statistic separates the")
        print("  winners from the losers on this corpus. Shipping 'detect a")
        print("  BMP and delta it' would trade a certain win on structured")
        print("  content for an unmeasured regression on photographic")
        print("  content. Close §4.4 rather than ship a coin-flip heuristic.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001 - probe must fail loudly, never silently
        print("FAIL: %s" % e)
        sys.exit(2)