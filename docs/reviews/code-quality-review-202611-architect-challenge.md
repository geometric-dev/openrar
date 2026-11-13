# Code-Quality Review 2026-11 — Principal Architect Challenge

**Plan reviewed:** [`docs/reviews/code-quality-review-202611.md`](code-quality-review-202611.md) — the
prioritised P2–P4 remediation list and its fix outlines
**Reviewer posture:** independent Principal Systems & Solution Architect — adversarial, pre-implementation
**Method:** re-derived every verification claim in the plan against the shipped tree at HEAD `701abb1`;
traced each proposed fix against the actual call graph, thread model and test surface
**Date:** 2026-11-13
**Verdict as found:** 🟡 **CONDITIONAL APPROVAL (REVISIONS REQUIRED)**

---

## 1. Executive Summary & Core Posture

The review's *findings* are sound and unusually well-evidenced: the arithmetic-site classification
holds up under re-derivation, and the four "looks dangerous, is bounded" analyses (recursion depth,
`SIZE_MAX` guards, `rec_end` clamping, varint overflow) each survive an independent read. The
priority ordering is defensible: nothing in P3/P4 blocks anything in P2.

The *fix outlines*, however, are not all implementable as written. Three of them contain defects
that would ship the appearance of a fix without the substance:

1. The headline remediation for the overflow class — a `narrow<T>()` helper whose only check is
   `assert` — **compiles to a plain `static_cast` in release builds**. For a parser fed hostile
   input on a release binary, that is not a fix; it is a comment.
2. The proposed static-analysis ratchet keys its baseline on `file:line`. That baseline will
   invalidate itself on the next refactor, and the predictable response — regenerate the baseline —
   silently re-accepts whatever was fixed.
3. The stack-frame fix proposes a **mutable scratch member reused across calls** on an object that
   the `-mt` extract/test path drives from multiple pool workers. That converts a stack-budget
   concern into a data race.

Separately, two verification gates the plan leans on do not exist in the form claimed
(byte-identical CLI golden output; MSVC `C6262` "drops to 0"), and the plan schedules no remediation
for the two translation units the review itself could not analyse — one of which is the sandboxed
parser for hostile input.

On the question "can this list be executed as written and produce the guarantees it claims?", the
answer is **no, not yet**. It is close: four directives (§6, D-1…D-4) make it sound.

---

## 2. 🚨 Critical Red Flags & Fatal Flaws

### R-1 — `narrow<T>()` with `assert` does not fix the overflow class (P2-1 step 1)

The plan's helper:

```cpp
template <class To, class From>
constexpr To narrow(From v) noexcept {
    assert(v >= /* min */ && v <= /* max */);
    return static_cast<To>(v);
}
```

In any `NDEBUG` build — i.e. every shipped binary — this is `static_cast`. The review's own stated
assurance goal is *"the project's assurance claim is about overflow and memory safety"*; an
assert-only converter delivers exactly the truncation it was introduced to prevent, while removing
the warning that used to advertise it. Worse, it **launders the signal**: after the sweep, the
`bugprone-narrowing-conversions` check is re-enabled (step 3) but every site is already wrapped in
`narrow<>()`, so the checker has nothing left to see.

The two populations must be separated, because they have different trust levels:

- **Internal, provably-bounded values** (reserve sizes derived from a capped buffer, iterator
  deltas within one container, `method` ids already masked to 0..5): an asserting converter is
  correct here, and is genuinely useful in debug.
- **Values derived from an archive header** (`unp_size`, `pack_size`, `rec_size`, `name_len`,
  `tlen`, `extra_size`): conversion failure is an *input-validation outcome*, not an assertion.
  These need a fallible API and a fail-closed call site:

```cpp
// core/narrow.hpp
template <class To, class From>
constexpr std::optional<To> checked_narrow(From v) noexcept {
    if (!std::in_range<To>(v)) return std::nullopt;   // C++20; C++17: manual range test
    return static_cast<To>(v);
}
```

**The plan must state, per site, which of the two applies.** A blanket `narrow<>()` sweep is not an
acceptable remediation.

### R-2 — A `file:line` baseline ratchet will rot on first use (P2-1 step 4)

`tools/static_analysis_baseline.json` keyed on `(file, line, check)` has 209 entries today. Any edit
above a baselined line — including the P2-3 `cli_main` decomposition, which rewrites 1,095 lines —
shifts or deletes most of them. The gate then fails on "new" findings that are the same old ones,
and the only practical remedy is regenerating the baseline, which is indistinguishable from
re-accepting a real regression.

Line-keyed allowlists are a known anti-pattern for exactly this reason. The good news the plan
itself under-states: **there is no need for a baseline at all.** The true distinct-site count after
deduplicating repeated header diagnostics is ~60 edits, all mechanical. Fix to zero, then set the
gate to `--warnings-as-errors` with nothing to allow. A ratchet you cannot finish is worse than a
deadline you can.

### R-3 — The P2-2 scratch-buffer fix introduces a data race

The plan says: *"replace the frame with a caller-owned scratch (`std::vector<core::byte>& scratch`,
`resize` once, reused across extents)"* and, in the same bullet, *"a per-reader scratch member"*.
The second form is unsafe. `ArchiveReader::extract_entry_sink` / `test_entry_stream` are submitted
per-entry to `core::ThreadPool` on the `-mt` path; a mutable member buffer reused across those
calls is shared mutable state with no lock — a classic race that TSan would flag immediately and
that would corrupt extracted data non-deterministically.

The stub itself is only 64 KiB. The safe form is a **per-call heap allocation hoisted outside the
extent loop**:

```cpp
const auto buf = std::make_unique<core::byte[]>(64 * 1024);   // once per call, not per extent
```

This realises the whole stack saving, keeps the template header-only, adds one allocation per
*entry* (not per 64 KiB read), and is race-free by construction. The plan must delete the
"member scratch" option rather than present it as an equal alternative.

### R-4 — Two verification gates in the plan do not exist in the claimed form

- **"Gate on byte-identical CLI golden output" (P2-3).** There is no stdout-capture harness.
  `tests/unit/cli_tests.cpp` (3,052 lines) spawns the built binary with `std::system` and asserts
  **exit codes and filesystem side effects**, not output bytes. A 1,095-line function rewritten
  under a gate that cannot observe its output has no gate.
- **"`C6262` count for those TUs drops to 0" (P2-2).** Lowering the `/analyze` threshold to 16 KiB
  *raises* the number of reported frames; it cannot be used to demonstrate the fix. Verification
  must be: the named frame disappears from a fresh `/analyze` run at the default threshold.

### R-5 — The plan schedules nothing for the two unanalysed TUs

The review admits `src/sandbox/worker_main.cpp` and `src/wasm/archive_api.cpp` produced empty
analysis logs and were covered "only by the manual pattern scans". `worker_main.cpp` is the
**sandboxed worker — the process that parses hostile input by design**. Declaring it out of scope
and scheduling no remediation leaves the highest-risk unit outside the guarantee the review exists
to provide, and the review's own P2-1 remedy (an extended CI leg) would inherit the same gap
silently, because the extra compile DB is a hand-maintained 2-entry file that has evidently
stopped working.

---

## 3. ⚠️ Weak Assumptions & Fragile Invariants

- **"No P0/P1" is a static-only claim presented as a safety verdict.** The report is careful in §7,
  but §1's verdict line is what people quote. `ThreadPool` and `ByteBudget` are the only
  synchronisation in the tree and neither has a TSan run behind it; `ByteBudget::acquire` can
  deadlock by construction if a caller ever requests more than the total (documented, but a
  documented deadlock is still a deadlock). Either qualify the verdict or add the TSan leg.
- **P2-3 and P2-1 collide.** `cli/main.cpp` carries 10 of the 35 narrowing sites. Doing P2-1 step 2
  first means those 10 edits are thrown away when `cli_main` is decomposed. Sequence the
  decomposition first, or exclude `main.cpp` from the mechanical sweep.
- **The widening sweep touches hot headers.** `compressor50.hpp` and `decompressor50.hpp` are
  included by 7+ TUs each; a `2u * 1024u * 1024u` sweep forces a full rebuild. Harmless, but it must
  be one isolated commit so a bisect does not land on a 60-file header change mixed with anything
  else.
- **`assert(depth < 2)` (P3-3) compiles out in release.** For recursion the *bound* is structural,
  so the assert is documentation — which is fine, provided the plan says so instead of implying
  runtime enforcement. If enforcement is wanted, return an error code at the back-edge.
- **P2-2's evidence is ten days stale.** All `C6262` figures come from
  `*.nativecodeanalysis.xml` dated 2026-11-03; HEAD is 2026-11-13. The frames were spot-checked in
  source and do exist, but a fix scheduled against stale measurements should begin with a fresh
  baseline run.

---

## 4. ⚡ High-Performance & Mechanical Sympathy Gaps

- **P2-2 allocation placement.** "Hoisted outside the extent loop" is the whole point; if the
  `make_unique` lands *inside* the per-extent loop it adds an allocation per volume extent on the
  verification hot path. The plan must say "once per call". It currently says "resize once, reused
  across extents", which is the right idea expressed against the wrong (member) object.
- **P2-3 and inlining.** Decomposing `cli_main` moves the hot per-entry loops (`write_batch_add_ex`,
  the parallel extract submission) across TU boundaries. Without LTO that is a measurable change to
  the per-file path. The plan claims "no behaviour change" but not "no throughput change", and
  schedules no measurement. Add: extract benchmark before/after, same corpus, same thread count.
- **P2-1's zero-tolerance gate will run 69 TUs serially in CI.** `clang_tidy_check.sh` already uses
  `xargs -P $(nproc)`; the extended suite adds the alpha checkers, which are the slow ones
  (`cli_main` 200 s, `dll_api` 155 s under the extended set). Budget the CI leg explicitly rather
  than discovering it as a timeout flake.

---

## 5. 🔍 Omissions, Edge Cases & Recovery Gaps

- **No disposition for the 7 alpha false positives.** The report documents them (§4.6–4.7) but the
  plan has no step that records the disposition in source (`NOLINT` + reason) or drops the alpha
  checkers from the standing suite. They will therefore reappear on every run and be re-triaged by
  every reviewer — the definition of a noise floor.
- **No fresh MSVC `/analyze` run is scheduled anywhere.** The plan schedules fixes *for* `C6262`
  findings and fixes *to* the gate, but never the run that proves either.
- **P3-2 has no fault-injection case.** Swapping `last_write_time` to the `error_code` overload is
  only meaningfully verified when the call actually fails (file deleted between `exists` and the
  call, or permission denied). The plan offers no such case; the repository already has the fault
  hooks (`should_fail_next_sync()` pattern) to do it cheaply.
- **No rollback story for the `narrow` sweep.** If the sweep lands and a downstream consumer of a
  header expects the previous implicit conversion, a single revert commit is needed. The plan's
  "one PR closes the class" phrasing invites one large unrevertable change; prefer per-header
  commits.

---

## 6. 🛠️ Actionable Revision Directives

**D-1 (blocks R-1) — Split the converter population.**
`narrow<To>()` (asserting) for internal bounded values only; `checked_narrow<To>() -> std::optional<To>`
for anything read out of a header, with call sites failing closed. The plan must tabulate which of
the 35 sites is which before any edit. Acceptance: a hostile-archive test that feeds `unp_size =
2^63` through the 32-bit path fails closed, not silently truncated.

**D-2 (blocks R-2) — Drop the baseline file.**
Drive the extended suite to zero, then gate with `--warnings-as-errors`. No `file:line` allowlist.
If a genuine false positive must be accepted, it is a single `NOLINT` with a reason at the site —
self-documenting and immune to line churn.

**D-3 (blocks R-3) — Per-call heap buffer only.**
Delete the "member scratch" alternative from the plan. `std::make_unique<core::byte[]>(64 * 1024)`
hoisted once per call, outside the extent loop. Add a TSan test that runs `-mt 4` over an archive
with multiple extents, since that is the configuration the race would live in.

**D-4 (blocks R-4/R-5) — Build the gates before the refactors.**
(a) Repair the extra compile DB and add `worker_main.cpp` + `wasm/archive_api.cpp` to the standing
suite; (b) add a stdout-capture harness for CLI commands *before* touching `cli_main` — the
existing exit-code assertions are a useful complement but cannot detect output regressions;
(c) verify P2-2 with a fresh `/analyze` at default threshold, not a lowered one.

**D-5 — Sequence for evidence, not convenience.**
Order: D-4 gates → fresh `/analyze` baseline → P2-1 (widening, per-header commits) → P2-2 → P3-1/2/3
→ P2-3 last, with an extraction benchmark bracketing it. Exclude `main.cpp` from the P2-1 sweep if
P2-3 is deferred.

**D-6 — Record the alpha disposition in source.**
`NOLINTNEXTLINE` with a one-line reason at the four `emplace(...).first` sites and the three struct
definition sites — or drop `ReturnPtrRange` from the standing suite and keep it manual. Either is
acceptable; silence is not, because silence is what makes the next reviewer re-do §4.6–4.7.

**D-7 — Qualify the verdict.**
§1 should read "no P0/P1 *memory-safety or overflow* defect found by static analysis and manual
review", with the TSan/ASan gap named in the same sentence, not only in §7.

**D-8 — Add the fault-injection case for P3-2.**
A test that deletes the destination archive (or revokes write permission) between `exists()` and
`last_write_time()`, asserting the `-tk` path reports its own error rather than a generic top-level
one.

---

*"A gate you cannot finish is a gate you will eventually regenerate. A fix that compiles out in
release is a comment with better formatting — and a review that schedules no measurement for its own
'no regression' claim is asking to be believed rather than verified."*
