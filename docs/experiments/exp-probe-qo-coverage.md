# EXP-PROBE-QO-COVERAGE (§4.30) — MEASURED, decision recorded

**Branch:** `integration` (probe only, no production change)
**Roadmap row:** `docs/experimentation-roadmap.md` §4.30
**Status:** **MEASURED** — outcome (ii): the fast path engages but saves < 20%.
Recorded as "QO is not a latency lever at this scale" per the row's exit criteria.

---

## 1. What was measured

The reader's QO fast path (`src/archive/archive_reader.cpp:995-1014`) is only
reachable when **four** conditions hold at once: a locator in the main header,
`locator_qo_offset > 0`, not a volume set, and headers not encrypted. The probe
measures whether it pays when it *is* reachable.

Archive pairs built from the same corpus, differing only in `-qo+` vs `-qo-`.
`l` wall time, **minimum of 9 runs** per configuration (process startup is an
additive fixed cost that dilutes the scan; the minimum suppresses scheduler
noise, the same reason `openrar_bench` uses a median of many).

`tools/qo_probe.py`, corpus generated deterministically, `-m0 -mt1` so the
packed payload is identical and only the QO cache differs.

## 2. Result

| entries | with `-qo+` | without | Δ time | Δ archive size |
|--:|--:|--:|--:|--:|
| 1,000 | 21.9 ms | 21.6 ms | **+1.5%** | +23.0% |
| 3,000 | 31.6 ms | 32.5 ms | −2.6% | +21.7% |
| 10,000 | 65.3 ms | 67.2 ms | **−2.8%** | **+21.3%** (+562,093 B) |

The row's exit criterion (i) was ≥20% at the top of the sweep. **Not met.**

## 3. What this means

**QO is a net loss as shipped.** It costs ~21% archive size (+562 KB at 10k
entries) and buys ~2.8% listing time. At 1,000 entries it is *slower* than not
having it.

The win is **flat**, not growing: +1.5% → −2.6% → −2.8%. The last step is
noise-sized. A cache that scales with entry count would show a widening gap; this
does not, which says the scan is not header-parse-bound in the way QO assumes —
it is dominated by per-file work that QO does not remove.

## 4. Why the row's framing was wrong

§4.30 assumed QO was a latency lever worth *widening* (encrypted-header QO,
volume sets). The measurement says the opposite: the fast path already fires for
the common case (plain, non-volume, unencrypted) and **does not pay there**. So
widening eligibility would extend a cost, not a benefit.

## 5. Recommended follow-ups (not in this row)

- **Record `-qo-` as the better default for OpenRAR**, or at minimum stop
  emitting QO by default. That is a CLI-visible behaviour change and belongs in a
  MINOR — see `docs/experiments/integration-2.5.0.md`.
- **Before flipping it**, confirm on a much larger corpus (≥100k entries) that
  the 2.8% does not become material. The flat trend suggests it will not, but
  the cost of checking is one more sweep.
- **Do not** widen eligibility (encrypted headers, volume sets). The row's
  proposed follow-up is withdrawn by this measurement.

## 6. Reproducing

```
python tools/qo_probe.py --openrar build/openrar64/Release/openrar.exe --reps 9
python tools/qo_probe.py --openrar <exe> --reps 15 --sizes 1000,10000,100000
```

No production code change. The probe writes only to a temp dir and deletes it.