"""EXP-PROBE-QO-COVERAGE (§4.30) — does the QuickOpen cache earn its cost?

Measures `l` wall time on archives built with and without `-qo+`, across
several entry counts, and reports whether the gap grows with entry count (the
decision-relevant quantity) or is flat (a fixed-cost artifact).

Protocol: each configuration is run N times and the MINIMUM is taken, not the
median. Process startup is a fixed additive cost that dilutes the scan time; the
minimum suppresses scheduler noise and is the standard way to compare
CPU-bound CLI work on a shared host.

The probe also reports the archive-size cost, because QO is not free: it writes
a service header plus a payload caching every file header.

Usage: python tools/qo_probe.py --openrar <exe> [--reps N]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

MIB = 1024 * 1024


def make_corpus(root, n):
    """Create n small files that are individually cheap but numerous."""
    d = os.path.join(root, "src")
    os.makedirs(d, exist_ok=True)
    for i in range(n):
        with open(os.path.join(d, "f%05d.txt" % i), "w", encoding="ascii") as f:
            f.write(("line %d\n" % i) * 20)
    return d


def pack(openrar, corpus, out, qo, threads="1"):
    """Build an archive. `qo` is "-qo+" or "-qo-" and is always explicit so the
    pair differs in exactly one flag."""
    cmd = [openrar, "a", "-y", "-ep", "-m0", "-mt" + threads, qo, "-idq", out,
           os.path.join(corpus, "*.txt")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0


def time_list(openrar, archive, reps):
    """Return (minimum wall time in ms, qo_taken) over `reps` runs of `l`.

    qo_taken is the reader's own taken/not-taken bit, emitted on stderr because
    OPENRAR_DEBUG_QO is set. It is required: if the line is absent the probe
    fails rather than reporting a timing that cannot say whether the fast path
    fired. A bit that changes between identical runs also fails.
    """
    env = dict(os.environ, OPENRAR_DEBUG_QO="1")
    best = None
    taken = None
    for _ in range(reps):
        t0 = time.perf_counter()
        r = subprocess.run([openrar, "l", archive], capture_output=True, text=True,
                           env=env)
        dt = (time.perf_counter() - t0) * 1000.0
        if r.returncode != 0:
            return None
        m = re.search(r"^\[qo\] taken=([01])", r.stderr or "", re.MULTILINE)
        if m is None:
            raise RuntimeError(
                "reader emitted no '[qo] taken=' bit for %s - the probe cannot "
                "tell whether the fast path fired (is OPENRAR_DEBUG_QO wired "
                "into this build?)" % archive)
        bit = m.group(1) == "1"
        if taken is not None and bit != taken:
            raise RuntimeError("QO taken bit changed between runs of %s" % archive)
        taken = bit
        if best is None or dt < best:
            best = dt
    return (best, taken)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--openrar", required=True)
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--sizes", default="1000,3000,10000")
    args = ap.parse_args()

    openrar = os.path.abspath(args.openrar)
    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]

    print("EXP-PROBE-QO-COVERAGE (docs/experimentation-roadmap.md 4.30)")
    print("minimum of %d runs per configuration (process startup is additive)" % args.reps)
    print()
    hdr = "%-7s %10s %10s %8s %9s %9s" % ("entries", "with-QO", "no-QO", "delta",
                                          "qo-taken", "size-overhead")
    print(hdr)
    print("-" * len(hdr))

    verdict_rows = []
    for n in sizes:
        tmp = tempfile.mkdtemp(prefix="qo_probe_")
        try:
            corpus = make_corpus(tmp, n)
            with_qo = os.path.join(tmp, "with.rar")
            no_qo = os.path.join(tmp, "no.rar")
            if not pack(openrar, corpus, with_qo, "-qo+"):
                print("%-7d pack failed (-qo+)" % n)
                continue
            if not pack(openrar, corpus, no_qo, "-qo-"):
                print("%-7d pack failed (-qo-)" % n)
                continue

            res_qo = time_list(openrar, with_qo, args.reps)
            res_no = time_list(openrar, no_qo, args.reps)
            if res_qo is None or res_no is None:
                print("%-7d list failed" % n)
                continue
            (t_qo, taken_qo), (t_no, taken_no) = res_qo, res_no

            # Self-validating: the bit must distinguish the two archives. If the
            # -qo- build also reports "taken", the marker is not measuring QO
            # and every verdict below would be built on a meaningless signal.
            if taken_no:
                raise RuntimeError(
                    "the -qo- archive at %d entries took the QO fast path - the "
                    "taken bit does not distinguish QO from a linear scan" % n)
            if not taken_qo:
                print("  note: -qo+ archive at %d entries did not take the fast "
                      "path (recorded, not inferred)" % n)

            s_qo = os.path.getsize(with_qo)
            s_no = os.path.getsize(no_qo)
            delta = 100.0 * (t_qo - t_no) / t_no
            overhead = 100.0 * (s_qo - s_no) / s_no
            print("%-7d %8.1fms %8.1fms %+7.1f%% %9s %+8.1f%%" %
                  (n, t_qo, t_no, delta, "yes" if taken_qo else "no", overhead))
            verdict_rows.append((n, delta, overhead, s_qo - s_no, taken_qo))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    if not verdict_rows:
        print("no measurements")
        return 1

    first = verdict_rows[0]
    last = verdict_rows[-1]
    print("Decision input:")
    print("  qo fast path taken (with-QO): %s" %
          ", ".join("%d:%s" % (r[0], "yes" if r[4] else "no") for r in verdict_rows))
    for prev, cur in zip(verdict_rows, verdict_rows[1:]):
        print("  entries %d -> %d : delta %+.1f%% -> %+.1f%% (step %+.1f pts)" %
              (prev[0], cur[0], prev[1], cur[1], cur[1] - prev[1]))
    print("  overall entries %d -> %d : delta %+.1f%% -> %+.1f%%" %
          (first[0], last[0], first[1], last[1]))

    # Growth is judged on the LAST step, not first-vs-last. A one-time
    # transition from "slower" to "faster" and a gap that keeps widening both
    # look like a wide first->last range, but only the latter supports "the
    # scan is header-parse-bound". With N points the top-of-sweep slope is the
    # decision-relevant one; the doc reports every step as well.
    if len(verdict_rows) >= 2:
        last_step = last[1] - verdict_rows[-2][1]
    else:
        last_step = 0.0
    if last[1] < 0 and last_step <= -1.0:
        print("  The win GROWS with entry count - it is still widening at the "
              "top of the sweep (%+.1f pts on the last step)." % last_step)
    elif last[1] < 0:
        print("  The win is FLAT at the top of the sweep (%+.1f pts on the last "
              "step): a fixed-cost artifact, not a growing scan saving." % last_step)
    else:
        print("  NO win at the largest size: QO costs archive bytes and buys no "
              "listing speed.")
    print("  Archive cost at %d entries: %+d bytes (%+.1f%%)" %
          (last[0], last[3], last[2]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        # Fail closed: a probe that cannot report the taken bit, or whose bit
        # does not separate the two archives, must not exit 0 with a table.
        print("FAIL: %s" % e)
        sys.exit(2)