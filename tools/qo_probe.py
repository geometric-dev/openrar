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
import shutil
import subprocess
import sys
import tempfile

MIB = 1024 * 1024


def make_corpus(root, n):
    """Create n small files that are individually cheap but numerous."""
    d = os.path.join(root, "src")
    os.makedirs(d, exist_ok=True)
    for i in range(n):
        with open(os.path.join(d, "f%05d.txt" % i), "w", encoding="ascii") as f:
            f.write(("line %d\n" % i) * 20)
    return d


def pack(openrar, corpus, out, threads="1"):
    cmd = [openrar, "a", "-y", "-ep", "-m0", "-mt" + threads, "-idq", out,
           os.path.join(corpus, "*.txt")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0


def time_list(openrar, archive, reps):
    """Return the minimum wall time in ms over `reps` runs of `l`."""
    best = None
    for _ in range(reps):
        import time
        t0 = time.perf_counter()
        r = subprocess.run([openrar, "l", archive], capture_output=True, text=True)
        dt = (time.perf_counter() - t0) * 1000.0
        if r.returncode != 0:
            return None
        if best is None or dt < best:
            best = dt
    return best


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
    hdr = "%-9s %12s %12s %9s %9s" % ("entries", "with-QO", "no-QO", "delta", "size-overhead")
    print(hdr)
    print("-" * len(hdr))

    verdict_rows = []
    for n in sizes:
        tmp = tempfile.mkdtemp(prefix="qo_probe_")
        try:
            corpus = make_corpus(tmp, n)
            with_qo = os.path.join(tmp, "with.rar")
            no_qo = os.path.join(tmp, "no.rar")
            if not pack(openrar, corpus, with_qo):
                # -qo+ is the default; retry explicitly
                if not pack(openrar, corpus, with_qo):
                    print("%-9d pack failed" % n)
                    continue
            cmd = [openrar, "a", "-y", "-ep", "-m0", "-mt1", "-qo-", "-idq",
                   os.path.join(tmp, "no.rar"), os.path.join(corpus, "*.txt")]
            subprocess.run(cmd, capture_output=True, text=True)

            t_qo = time_list(openrar, with_qo, args.reps)
            t_no = time_list(openrar, no_qo, args.reps)
            if t_qo is None or t_no is None:
                print("%-9d list failed" % n)
                continue

            s_qo = os.path.getsize(with_qo)
            s_no = os.path.getsize(no_qo)
            delta = 100.0 * (t_qo - t_no) / t_no
            overhead = 100.0 * (s_qo - s_no) / s_no
            print("%-9d %10.1fms %10.1fms %+8.1f%% %+8.1f%%" %
                  (n, t_qo, t_no, delta, overhead))
            verdict_rows.append((n, delta, overhead, s_qo - s_no))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    if not verdict_rows:
        print("no measurements")
        return 1

    # The decision: does the gap grow with entry count?
    first = verdict_rows[0]
    last = verdict_rows[-1]
    print("Decision input:")
    print("  entries %d -> %d : delta %+.1f%% -> %+.1f%%" %
          (first[0], last[0], first[1], last[1]))
    if last[1] < 0 and last[1] < first[1] - 1.0:
        print("  The win GROWS with entry count - a real (if small) scaling benefit.")
    elif last[1] < 0:
        print("  The win is FLAT with entry count - a fixed-cost artifact, not a scan saving.")
    else:
        print("  NO win at the largest size: QO costs archive bytes and buys no listing speed.")
    print("  Archive cost at %d entries: %+d bytes (%+.1f%%)" %
          (last[0], last[3], last[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())