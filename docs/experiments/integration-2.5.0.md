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

### 3.1 Gate deviations recorded on this branch

Two landed rows were promoted with gates that did not match the implementation.
Both are amended in the roadmap and recorded here rather than left to drift.

**`EXP-MT-DECISION-CONSOLIDATE` (§4.25).** The row's gate asked for
"byte-identical `-mt4` output for every guard cell". That contradicts the fix:
the defect is precisely that the 64 KiB-sampling sites silently dropped `-mc` on
a container header in `[64 KiB, 1 MiB)`, so on such a member the post-fix bytes
*must* differ (the filter token is now emitted). Amended gate: `-mt1`
byte-identical everywhere; `-mt4` identical wherever the decision was already
chunked/sequential, which covers all 33 cells; `-mt4` may change only on the
affected `[64 KiB, 1 MiB)` members, by design. Residual accepted and recorded: a
PE/Mach-O header at `>= 1 MiB` can still differ between the streaming sites
(1 MiB prefix) and the in-memory site (whole buffer). The original gate also
named `Compressor50::should_chunk_member`; the shipped symbol is the free
function `compress::decide_chunked_pack`.

**`SEC-INTEROP-UNDEF-FILTER` (§4.21).** See §3.2.

### 3.2 §4.21: what the test actually pins

The row asked for a negative test that *constructs* a type-5 filter token and
asserts the region is emitted raw. That token lives inside a Huffman-coded `LD`
token stream; reaching it from a unit test needs a bitstream writer, which the
branch did not build. The delivered tests pin the two adjacent properties that
are reachable, and the roadmap gate is amended to say so:

- the encoder cannot emit a reserved type at all — `FilterType` is pinned to
  `0..3` by `static_assert` and a runtime `<= 7` check, and `put_bits(type, 3)`
  is the only writer (`tests/unit/sec_interop_undef_filter_tests.cpp`);
- legal filters are applied, not passed through raw, with byte-exact round trips
  at `-m1/-m3/-m5`;
- the decoder's reserved-type branch (`decompressor50.cpp:880-881`) emits the
  raw region; this remains a source-level, document-pinned property, explicitly
  **not** claimed as exercised by the test;
- the divergence is recorded in `docs/spec/04-filters.md` §4..7 and the C9
  corpus scan asserts no emitted filter token has `type > 3`.

The roadmap gate now states that the bitstream-level negative test is
outstanding (tracked as future work) instead of claiming it landed.

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
| PROBE-QO-COVERAGE | **measured, on `integration`** | QO costs ~21% archive size for ~2.8% listing speed; see `docs/experiments/exp-probe-qo-coverage.md` |

## 5. Release gate (unchanged from `docs/milestones/roadmap.md` §8)

A 2.5.0 tag requires all four:

1. **Hard correctness & interop** — 100% ctest (60 entries: 58 suites + the
   two C9 fixtures), 30→33 golden
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

- [x] `EXP-MT-DECISION-CONSOLIDATE` landed (`2106487`) — byte-identical at `-mt1`,
      fixes a silent `-mc` loss on containers with headers past 64 KiB
- [x] `PROBE-QO-COVERAGE` measured (`f8bae82`) — QO costs 21% size for 2.8% speed
- [x] 60/60 ctest entries green on the integration branch (58 suites + the two
      C9 fixtures); `mt_decision_tests` added on `2106487`
- [x] release guard `[SUCCESS]` on the integration build
- [ ] `SEC-PASSWORD-CHANNEL` landed, or explicitly deferred with a recorded
      reason (if deferred, 2.5.0 cannot claim a new CLI switch and the MINOR
      justification needs revisiting)
- [ ] Every promoted row's outcome written into `docs/milestones/roadmap.md` §7
      and `CHANGELOG.md`
- [ ] No uncommitted work on the branch
- [ ] Manual approval before any push (per the standing instruction)

### 6.1 Current state

`integration` @ `70f9f45`, 5 commits past `master` (the two landed wins plus
the SEC-INTEROP merge that closed §4.21):

| Commit | Content |
|:---|:---|
| `51fcbf1` | C9 corpus + filter probe (rev 6, from master) |
| `2abf2d4` | this plan |
| `2106487` | EXP-MT-DECISION-CONSOLIDATE |
| `f8bae82` | PROBE-QO-COVERAGE measurement |
| `310ed53` | record the two landed wins and the remaining checklist |
| `1ff989f` | SEC-INTEROP-UNDEF-FILTER |
| `70f9f45` | merge: SEC-INTEROP-UNDEF-FILTER |

Verified on this branch: 60/60 ctest entries, guard `[SUCCESS]`, archives
byte-identical to `master` at `-mt1`. (The commonly quoted "58/58" was a suite
count that omitted the two C9 fixture entries `c9_tools_build` / `c9_build`.)

Gate/claim mismatches found and closed on 2026-11-18: the inverted causal
comments in the MT decision, the §4.25 byte-identity gate that contradicted the
fix it gated, three fail-open test branches, the dead `mt_should_use_parallel`
predicate, a Python-3 configure `FATAL_ERROR` that made the C9 gate a build
prerequisite, the QO probe's missing taken/not-taken bit and its flat-vs-growing
contradiction, and the §4.21 claim that overreached what the tests pin. See §3.1
and §3.2.