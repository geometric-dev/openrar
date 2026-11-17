# Integration branch — 2.5.0 LTS candidate

**Branch:** `integration`
**Cut from:** `master` @ `51fcbf1`
**Target:** `v2.5.0` (MINOR — see `docs/versioning.md:94`)
**Date cut:** 2026-11-17

---

## 1. Why a MINOR, and why now

`docs/versioning.md:93-94` draws the line this branch exists to respect:

| Bump | Triggers |
|:---|:---|
| **MAJOR** | C ABI breaks, source-breaking C++ API changes, **removal or renaming of CLI switches**, dropping a platform, output regression breaking WinRAR interop |
| **MINOR** | **new CLI switches**, new exports, new WASM/JS surface, new archive features, notable performance work |

`SEC-PASSWORD-CHANNEL` adds `--password-file` and makes bare `-p` / `-hp` read
from a terminal. That is a **new CLI switch**, so it is a MINOR change and must
ship in 2.5.0. It cannot slip to a 2.5.x patch without either breaking the
compatibility promise or shipping a CLI addition in a patch release.

The 2.4.x line is therefore the last line that can be *purely* PATCH. Anything
user-visible that is not already in 2.4.2 has to be in this branch.

## 2. What is already here

| Commit | What | Source branch |
|:---|:---|:---|
| `51fcbf1` | C9 decode-identity corpus + filter probe (rev 6) | master |
| `c0e8a79` | (baseline) wide short-copy primitive | master |

Master is the v2.4.2 release state (`CHANGELOG.md` §2.4.2: short-match decode
up to 19% faster, cadence hoist).

## 3. Promotion candidates, in the order they should land

Ordered by (1) whether the CLI surface depends on them, (2) cost, (3) whether
they unblock something else.

| # | Row | Class | Why here | Risk |
|:---:|:---|:---|:---|:---|
| 1 | **SEC-PASSWORD-CHANNEL** (§4.14) | W2-P | **The only new-CLI-switch item. Blocks the MINOR.** | Medium: TTY handling on POSIX + Windows, non-TTY must fail closed |
| 2 | **EXP-MT-DECISION-CONSOLIDATE** (§4.25) | W2-P | Landed on `exp/mt-decision-consolidate`; fixes a real silent `-mc` loss on containers with late headers | Low: byte-neutral at `-mt1`, verified |
| 3 | **PROBE-QO-COVERAGE** (§4.30) | MEAS | Landed? No — needs writing. Cheap, may find a defect | None (log-only) |
| 4 | **EXP-PACK-BLOCK-PROFILE** (§4.33) | MEAS | Code landed on `exp/pack-block-profile`; row BLOCKED at 45% residual | None (compile-time gated) |
| 5 | **EXP-FILTER-SAFE-MT** (§4.3) | W2-P | Prerequisite for image/audio; needs §4.25 first | Medium: chunk/region alignment, delta plane-carry |
| 6 | **EXP-AUDIO-DETECT** (§4.5) | W3 | Measured win (18.8–48.8% on surround WAV) | Medium: RIFF/RF64/AIFF chunk walk |
| 7 | **EXP-PARALLEL-DECODE-EXTRA-DIST** (§4.26) | W2-D | RAR7 members currently decode single-threaded | High: needs an oracle-produced C8 corpus |
| 8 | **EXP-IMAGE-STRIDE** (§4.4) | W3 | Behind the LZ-retention gate | High: needs that gate |

## 4. Closed / blocked — do not promote

| Row | Status | Reason |
|:---|:---|:---|
| EXP-MACHINE-DISPATCH | shipped v2.4.0 | — |
| SEC-TEMP-MODE | shipped v2.4.1 | — |
| EXP-APPLIER-RECORD-FASTPATH | shipped 2.4.2 | — |
| EXP-BLOCK-FLUSH-TUNE | **closed, refuted** | 0.125% envelope vs 0.5% gate |
| EXP-HUFF-TABLE-REUSE | **blocked by spec gate** | `docs/spec/03-compression-m1-m5.md:79-88`, clause (a) unsatisfiable in-tree |
| EXP-MT-SEED-AMORTIZE | **closed, refuted** | seed path off the critical path |
| EXP-A64-BRANCH / EXP-RISCV-JAL / EXP-PAETH-IMAGE | **closed** | no type slot in RAR 7.23 (Q-1) |
| EXP-PACK-BLOCK-PROFILE | **row BLOCKED** | 45% unattributed residual; code is useful but the numbers cannot rank a 2% bucket |

## 5. Release gate (unchanged from `docs/milestones/roadmap.md` §8)

A 2.5.0 tag requires all four:

1. **Hard correctness & interop** — 100% ctest (57 suites now), 30→33 golden
   cells bit-exact across `-m0`..`-m5`, clean under UnRAR 7.20 and WinRAR 7.20.
2. **Non-regression guard** — `tools/release_regression_guard.py --baseline
   bin/v2.0.23/openrar.exe` → `[SUCCESS]`.
3. **Empirical receipts** — every perf claim backed by same-session A/B on the
   reference host, across MSVC/GCC/Clang.
4. **Memory & lifecycle** — I-15/I-16 via `ScopedAllocTracker`; ASan/UBSan clean.

Plus the version bump: `tools/bump_version.py 2.5.0` across all 8 manifests,
`CHANGELOG.md` `[2.5.0]` with highlights from 2.4.2 + this branch.

## 6. Definition of "enough for a release"

This branch has enough when **all** of these hold, not merely some:

- [ ] `SEC-PASSWORD-CHANNEL` landed, or explicitly deferred with a recorded
      reason (if deferred, 2.5.0 cannot claim a new CLI switch and the MINOR
      justification needs revisiting)
- [ ] `EXP-MT-DECISION-CONSOLIDATE` landed
- [ ] 57+ ctest suites green on the integration branch
- [ ] release guard `[SUCCESS]` on the integration build
- [ ] Every promoted row has its outcome written into
      `docs/milestones/roadmap.md` §7 and `CHANGELOG.md`
- [ ] No uncommitted work on the branch
- [ ] Manual approval before any push (per the standing instruction)