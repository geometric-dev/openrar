# OpenRAR — Deep Code Quality & Static-Analysis Review

**Subject:** whole-tree review of `src/` + `include/` at HEAD `701abb1` (2026-11-13)
**Scope:** 61,456 lines across 69 first-party translation units (plus 2 externally-analysed TUs)
**Reviewer posture:** independent, evidence-first — every flagged site was read in context
**Date:** 2026-11-13
**Verdict:** 🟢 **no P0/P1 memory-safety or overflow defect found by static analysis and manual
review**; the tree is materially hardened. The remaining findings are one process gap with real
future risk (P2), two structural/resource items (P2), and cosmetics (P3–P4). This verdict is
static-only — no TSan/ASan run backs it (§7), and the concurrency claims for `ThreadPool` /
`ByteBudget` are from inspection, not instrumentation.

> **Revision status:** the remediation plan in §5 was challenged adversarially; three fix outlines
> were found unsound as written. The challenge is recorded in
> [`code-quality-review-202611-architect-challenge.md`](code-quality-review-202611-architect-challenge.md)
> and the corrected outlines are in §9. Read §9, not §5, before implementing.

---

## 1. Executive summary

The single most important result: **the Clang static analyser finds nothing actionable.**
A fresh, full extended-suite run (including alpha, taint and CERT checkers) over all 69
translation units produced no null dereference, no use-after-free, no leak, no
tainted allocation and no out-of-bounds index. Of the 209 unique diagnostics, 202 are
arithmetic style findings in two classes and every one was traced and found value-bounded;
the remaining **7 are `clang-analyzer-alpha` false positives**, each traced to its cause in
§4.6–§4.7 (a known `unordered_map::emplace(...).first` modelling limitation, and a pointer
range attributed to a struct definition rather than to any expression).

The second most important result: **CI would not have told you any of that.**
`tools/clang_tidy_check.sh` runs the repository `.clang-tidy`, which *disables* the two
arithmetic checks that produced all 202 arithmetic diagnostics and also disables
`clang-analyzer-core.CallAndMessage` and `cert-err33-c` — two of the highest-value
null-dereference / unchecked-return detectors. The gate that passes today is narrower than
the analysis that would catch a regression tomorrow. That is finding **P2-1**.

There is no reachable memory-safety defect, integer overflow, or unbounded recursion in the
current tree. The five places that *look* like they should be one (recursive extraction,
`get_bits64`, 64→`size_t` truncation, varint overflow, header `rec_end` arithmetic) are each
explicitly bounded by construction; the evidence is recorded in §4 so the next reviewer does
not have to re-derive it.

### Ranked findings

| ID | Priority | Finding | Files |
|---|---|---|---|
| P2-1 | P2 | Static-analysis gate is weaker than the review suite: 202 arithmetic sites and 7 unexplained alpha findings are invisible to CI, and the archived findings CSVs are stale | `.clang-tidy`, `tools/clang_tidy_check.sh`, `build/static-analysis/` |
| P2-2 | P2 | 64 KiB stack frames in copy/extract paths (`C6262`) | `archive_reader.cpp:2247`, `archive_mutator.cpp:1105`, +5 |
| P2-3 | P2 | `cli_main` is a single 1,095-line function with ~300 branch points | `cli/main.cpp:4268` |
| P3-1 | P3 | Platform process-path helper lives in the archive layer, violating the downward-only rule | `archive_mutator.cpp:1105` |
| P3-2 | P3 | One throwing `std::filesystem` call, against a 132-site `error_code` convention | `cli/main.cpp:1991` |
| P3-3 | P3 | Three bounded recursion cycles are undocumented and unasserted | `archive_reader.cpp`, `decompressor50.cpp`, `compressor50.cpp` |
| P4-1 | P4 | Cryptic field offset (`e + 16 - 4`) in the ZIP EOCD parser | `foreign_zip.cpp:94` |
| P4-2 | P4 | Unreachable-by-construction `is_encrypted` branch | `archive_reader.cpp:3403` |
| P4-3 | P4 | `goto`-based cleanup — the only `goto` in `src/` | `sandbox/channel.cpp:24–88` |
| P4-4 | P4 | Stale generated artefacts and stray `.obj` debris in the workspace | `build/static-analysis/`, repo root |

No P0 and no P1 items were found. §4 records the near-misses and why each is not one.

---

## 2. Method

### 2.1 Toolchain

| Item | Version / detail |
|---|---|
| Compiler DB | `build/static-analysis/compile_commands.json`, 69 TUs, MSVC 14.51 (VS 18 Community) |
| Clang-Tidy | LLVM 22.1.3 shipped with VS 18 (`VC\Tools\Llvm\x64\bin\clang-tidy.exe`) |
| Base config | repository `.clang-tidy` (naming + bugprone/cert/performance/misc) |
| Extended suite | `-*` then `clang-analyzer-{core,cplusplus,unix,security,alpha}.*`, alpha
`cstring`/`IteratorRange`/`SmartPtr`/`ReturnPtrRange`/`PointerArithm`, `optin.taint.TaintedAlloc`,
CERT `oop54`/`mem57`, and the overflow family (`implicit-widening-of-multiplication-result`,
`narrowing-conversions`, `misplaced-widening-cast`, `too-small-loop-variable`, `sizeof-*`,
`undefined-memory-manipulation`, `suspicious-memset-usage`, `unchecked-optional-access`, …) |
| Required flags | `--allow-enabling-analyzer-alpha-checkers`, `-Xclang -analyzer-config -Xclang
aggressive-binary-operation-simplification=true` (alpha iterator checkers crash without it in LLVM 22) |
| Coverage | 69/69 TUs; `header-filter` scoped to `src/` and `include/` |
| Recursion | separate isolated `misc-no-recursion` pass over the same 69 TUs |
| MSVC `/analyze` | existing artefacts only (see §6 limitation) |

### 2.2 Procedure

1. Fresh full Clang-Tidy pass (4-way parallel, ~13 min), one log per TU.
2. Isolated `misc-no-recursion` pass (~2 min).
3. Deduplicated findings regenerated into `build/static-analysis/tidy_findings.csv`
   (209 rows) and `recursion_findings.csv` (9 rows).
4. **Every** distinct diagnostic site read in context and classified as *bounded*,
   *false positive*, or *defect*.
5. Targeted adversarial review of the classic failure modes for an archive reader/writer:
   varint overflow, header size caps, name-length arithmetic, extra-record clamping,
   decompressed-size truncation on 32-bit, path traversal, symlink/hardlink materialisation,
   fd inheritance across the sandbox boundary, exception egress across the C ABI.
6. Repo-wide hygiene scans: unsafe libc, integer↔pointer casts, `using namespace` in headers,
   `goto`, TODO markers, filesystem error-handling consistency, function/TU size.

---

## 3. Static-analysis results (fresh run)

```
Unique diagnostic sites: 209      (256 raw warnings across 32 distinct messages)
```

| Checker | Sites | Assessment |
|---|---|---|
| `bugprone-implicit-widening-of-multiplication-result` | 167 | **All benign.** Constant-folded size expressions widened `int` → `size_t` (e.g. `2 * 1024 * 1024`). |
| `bugprone-narrowing-conversions` | 35 | **All value-bounded.** Iterator deltas, MSVC 32-bit `unsigned long` from `_BitScanForward`, `uint32` method ids, `size_t`→`int64` prints. |
| `clang-analyzer-alpha.cplusplus.IteratorRange` | 4 | **False positive** — `unordered_map::emplace(...).first` modelled as possibly `end()` (§4.6). |
| `clang-analyzer-alpha.security.ReturnPtrRange` | 3 | **False positive** — diagnostic anchored to a `struct` definition line (§4.7). |
| `clang-analyzer-{core,unix,cplusplus,security}.*` (non-alpha) | **0** | No null deref, leak, UAF, OOB, taint. |
| `misc-no-recursion` (separate pass) | 9 | 3 chains, all provably depth ≤ 2 (§4.1–4.3). |

The header-dominated counts are misleading: 167 "sites" collapse to ~60 distinct source
lines because header diagnostics repeat once per including TU (`compressor50.hpp` alone
accounts for 55 of them across 7 different includers).

**This is a material difference from the archived ledger.** The previously checked-in
`build/static-analysis/tidy_findings.csv` referenced `src/compress/compressor50.hpp:890` —
but that file is **867 lines** at HEAD. The ledger had drifted far enough to cite source
lines that no longer exist. A review that trusted it would have been reviewing a different
revision. See P2-1.

---

## 4. Critical/high findings: none — and the near-misses

Each of these was traced to its bound (or its false-positive cause). They are recorded so
that the next reviewer starts from evidence rather than re-deriving it.

**4.1 Recursive extraction (`extract_entry` ↔ `extract_store_entry`).**
`misc-no-recursion` flags the chain `extract_entry` → `extract_entry_impl` →
`extract_store_entry` → `extract_entry` (`archive_reader.cpp:2851/2888/3396/2857/3346/3404`).
**Not unbounded:** `extract_entry_impl` enters the encrypted branch at `:3247` for every
entry with `is_encrypted`, and every path through that branch returns (`return true` on
success, `return fail()` otherwise). The `method == 0` fall-through at `:3344` that calls
`extract_store_entry` is therefore unreachable for encrypted entries, so the back-edge at
`extract_store_entry:3404` cannot fire. Depth is 1. (The guard at `:3403` is dead code — P4-2.)

**4.2 FILECOPY resolution (`extract_entry_sink` / `test_entry_stream`).**
Both recurse on `redir_type == 5` (`:1936`, `:2097`) but only into an entry that is
`redir_type == 0`, which is filtered out immediately below (`:1946`, `:2103`) and cannot
re-enter. Depth is 1.

**4.3 `BitReader::get_bits64` recursion (`decompressor50.cpp:173`).**
Recurses only when `count > 32`, splitting into `count - 32` and `32`; both children are
≤ 32 and take the non-recursive path. Depth is 2, independent of input.

**4.4 64-bit sizes on 32-bit targets (WASM32 / i386).**
All four 64→`size_t` conversions of attacker-controlled sizes are immediately preceded by a
`SIZE_MAX` guard: `archive_reader.cpp:1877`, `:2234`, `:2344`, `:2388`, `:3211`, `:3311`
(and `archive_mutator.cpp:398`, `:3353` for volume/header sizes). No truncation path exists.

**4.6 `IteratorRange` × 4 — false positive on `unordered_map::emplace`.**
`extraction_journal.cpp:448` and `:459` (`register_temp`), `:531` and `:542`
(`register_temp_contained`) are reported as "past-the-end iterator dereferenced". Both
functions open with the same shape:

```cpp
auto it = journals_.find(key);
if (it == journals_.end()) {
    ... every failure path returns false ...
    it = journals_.emplace(key, DirJournal{...}).first;
}
if (granularity_ == Batch) { lru_.push_back(key); it->second.lru_it = std::prev(lru_.end()); }
```

`it` cannot be `end()` at either dereference: the `find` mismatch is repaired unconditionally
by `emplace`, and every path that skips the repair returns. The alpha checker models
`emplace(...).first` as possibly `end()` (it cannot prove the insertion succeeded), which is
a known imprecision of this experimental checker. Verified by reading both functions end to
end. **Fix outline:** replace `emplace(...).first` with an explicit
`insert` + `find` + `if (it == journals_.end()) return false;` — that both satisfies the
checker and makes iterator validity self-evident to a human reader (one of the four sites per
function). Low-cost, removes the only recurring alpha diagnostic in this file.

**4.7 `ReturnPtrRange` × 3 — false positive anchored to a `struct` line.**
`format/headers.hpp:285` (`struct FileXattr`), `cli/diff.cpp:49` (`struct DiffItem`) and
`cli/main.cpp:1749` (`struct PendingFile`) are each reported as "returned pointer value points
outside the original object". None of these lines returns a pointer — they are aggregate
definitions. The checker's path notes start in unrelated functions (`apply_xattrs`,
`diff_archives`, the add pipeline) and terminate at the struct because a `std::vector`
member's reallocation is inlined into its constructor. No expression is implicated.
**Fix outline:** a file-scoped `NOLINT` at the struct with a reason, or drop
`clang-analyzer-alpha.security.ReturnPtrRange` from the standing suite (alpha tier, known
false-positive rate) and keep it for periodic manual runs only.

**4.5 Header extra-record arithmetic.**
`parse_main_header` / `parse_file_header` compute `rec_end` from an attacker-supplied vint.
Every record parser uses the subtraction form (`rec_size <= extra_end - offset ? … : extra_end`)
rather than `offset + rec_size`, and each bounds check is performed against `rec_end - offset`
before the read. The `FHEXTRA_CRYPT` record re-validates with a single `need` computation
(`1 + 16 + 16 [+ 12]`) instead of per-field checks. The `FHEXTRA_XATTR` parser bounds the
entry count by `(rec_end - xoff) / 3` and rejects trailing bytes. No overflow is reachable.

---

## 5. Prioritised findings and fix outlines

### P2-1 — The static-analysis gate is narrower than the review suite

**Evidence.**

- `.clang-tidy` disables, globally: `bugprone-narrowing-conversions`,
  `bugprone-implicit-widening-of-multiplication-result`, `clang-analyzer-core.CallAndMessage`,
  `clang-analyzer-deadcode.DeadStores`, `cert-err33-c`, `bugprone-easily-swappable-parameters`,
  `misc-no-recursion` (and others).
- `tools/clang_tidy_check.sh` — the CI gate (`.github/workflows/build.yml:143`) — uses exactly
  that config. So the 202 arithmetic diagnostics in §3 cannot fail a build, and neither can a
  future null-dereference through a function call (`CallAndMessage`) or an unchecked return
  (`cert-err33-c`). The 7 alpha findings are invisible to CI for the same reason, which is
  arguably fine — but no one has recorded *why* they are accepted, so the next reviewer must
  re-derive §4.6–§4.7 or chase them again.
- `build/static-analysis/tidy_findings.csv` cited a line number (`compressor50.hpp:890`) past
  the end of the file at HEAD; `msvc_findings.csv` was last derived from
  `*.nativecodeanalysis.xml` dated 2026-11-03, ten days before HEAD.

**Impact.** The project's stated assurance claim is about overflow and memory safety. The
exact check family that detects the overflow half is disabled in the gate, and the only
record of it is a stale CSV. A one-line regression such as `int n = span.size();` in a
parsing path passes CI today.

**Fix outline.**

0. Record the disposition of the 7 alpha findings (§4.6–§4.7) as `NOLINT` with reasons, or
   drop the alpha checkers from the standing suite. Accepted findings without a recorded
   reason are indistinguishable from unnoticed ones.
1. Add `src/core/narrow.hpp`:
   ```cpp
   template <class To, class From>
   constexpr To narrow(From v) noexcept {
       assert(v >= static_cast<From>(std::numeric_limits<To>::min()) &&
              v <= static_cast<From>(std::numeric_limits<To>::max()));
       return static_cast<To>(v);
   }
   ```
   Apply at the 35 narrowing sites (they are mechanical: iterator deltas → `narrow<ptrdiff_t>`,
   MSVC `unsigned long` → keep the existing 32-bit cast with a `NOLINT` and a comment).
2. Rewrite the ~60 distinct widening sites as `2u * 1024u * 1024u` (or `size_t{1} << 21`).
   These are single-token edits in headers; one PR closes the class.
3. Re-enable `bugprone-narrowing-conversions` and `clang-analyzer-core.CallAndMessage` in
   `.clang-tidy`; keep `cert-err33-c` off only until the `[[nodiscard]]` sweep lands.
4. Extend `tools/clang_tidy_check.sh` with an `--extended` leg (the §2.1 suite) that fails on
   any finding not present in a checked-in `tools/static_analysis_baseline.json`. New findings
   fail; the baseline may only shrink.
5. Generate the CSVs in CI and upload as an artefact; delete them from the working tree.

**Verification.** After step 2, `run_tidy.ps1` reports 0 findings. Gate test: introducing
`int n = v.size();` into `archive_reader.cpp` must fail the extended leg.

**Effort** ~1 day, mechanical, no behaviour change. **Risk** low.

---

### P2-2 — 64 KiB stack frames in copy/extract paths

**Evidence (MSVC `/analyze` `C6262`).**

| Location | Frame | Local |
|---|---|---|
| `archive_reader.cpp:2247` `stream_stored_entry_chunks` | 65,648 B | `core::byte buf[64 * 1024]` |
| `archive_mutator.cpp:1105` `get_exe_dir` | 65,604 B | `wchar_t buf[32768]` (64 KiB on Windows) |
| `archive_reader.cpp:2888`, `:3413`; `buffer_archive.cpp:665`; `compressor50.cpp:52`; `sfx_main.cpp:257`; `sfx_gui_main.cpp:83` | 18–67 KiB | aggregate frames |

`stream_stored_entry_chunks` is a template instantiated three times (`:2470`, `:2510`,
`:2544`), so three separate 64 KiB frames exist in the image.

**Impact.** Today this is a budget concern, not a defect: `core::ThreadPool` uses
`std::thread` defaults (1 MiB Windows / 8 MiB Linux), so a single 64 KiB frame in a pool
worker is ~6% of stack. It becomes a real defect the moment a thread stack is reduced — a
`-mt N` path with custom stacks, an embedded/wasm configuration, or a future sanitizer build
with larger redzones. The 64 KiB frame is also pure overhead on the hottest per-entry path.

**Fix outline.**

1. `stream_stored_entry_chunks`: replace the frame with a caller-owned scratch
   (`std::vector<core::byte>& scratch`, `resize` once, reused across extents). Keeps the
   template header-only and removes the frame entirely.
2. `get_exe_dir`: use `std::wstring buf(32768, L'\0')` (heap) or the two-call
   `GetModuleFileNameW` probe; POSIX branch can use `std::filesystem::read_symlink("/proc/self/exe")`.
   Move the function to `io::path_util` (see P3-1) while doing this.
3. Add `C6262` to the MSVC `/analyze` ruleset with a 16 KiB threshold so the class cannot
   regress silently.

**Verification.** `C6262` count for those TUs drops to 0; extraction benchmarks unchanged
within noise (the buffer change is neutral for a 64 KiB sequential read).

**Effort** ~2 h. **Risk** low.

---

### P2-3 — `cli_main` is a 1,095-line function

**Evidence.** `src/cli/main.cpp:4268–5363` — 1,095 lines in one function, ~300 branch points
(`if`/`for`/`while`/`switch`/`case`/`catch`) inside it. The next largest functions in the tree
are `sfx_main_impl` (228 lines) and `sandbox::main` (192). `main.cpp` is 5,410 lines total;
`cli_main` alone is 20% of it.

**Impact.** Purely a maintainability/risk finding: no unit-testable seam, so every CLI change
requires a full-binary integration test, and merge conflicts concentrate in one function. It
is also the reason CLI-side overflow diagnostics (`main.cpp` carries 10 of the 35 narrowing
sites) are hard to audit.

**Fix outline.**

- Keep `cli_main` as the front controller only: global-flag parse → `Command` variant →
  `dispatch(cmd)`.
- Move each verb body into `src/cli/cmd_<verb>.cpp` exposing
  `int cmd_add(const AddOptions&)`, `cmd_extract`, `cmd_list`, `cmd_test`, `cmd_repair`,
  `cmd_convert`, `cmd_diff`. The existing `parse_mc_switch`/`DiffOptions` structs are already
  the right shape for option structs.
- Target: `cli_main` ≈ 150 lines; each command ≤ 250 lines and unit-testable with captured
  stdout.
- Gate on byte-identical output for the existing CLI golden tests — no behaviour change.

**Effort** ~2–3 days. **Risk** medium (large mechanical move; the golden-output gate is the
safety net). Not urgent; schedule as a standalone refactor with no other changes.

---

### P3-1 — Platform code in the archive layer

`get_exe_dir` (`archive_mutator.cpp:1105`) calls `GetModuleFileNameW` / `readlink("/proc/self/exe")`
directly. `ARCHITECTURE.md` §2 makes `io` the only layer that touches OS interfaces; the
archive layer should call `io::executable_dir()`. `io/path_util.cpp` is the existing home.
**Fix:** move the function (with the P2-2 heap buffer), keep `resolve_sfx_stub` calling it.
**Effort** 30 min. **Verification:** `tools/layer_check.py` stays green.

### P3-2 — One throwing filesystem call among 132 `error_code` call sites

`std::filesystem::last_write_time(arc_path)` at `cli/main.cpp:1991` uses the throwing overload;
every other mutating filesystem call in `src/` (`remove` 132/132, `rename` 13/13,
`create_directories` 12/12, `copy_file`, `permissions`) uses the `error_code` form. The call is
inside `main`'s top-level handler (L10), so it degrades to a generic error rather than
`std::terminate` — but it defeats the `-tk` path's own error reporting. **Fix:** use the
`error_code` overload and fall back to `file_time_type{}`. **Effort** 5 min.

### P3-3 — Bounded recursion should be asserted, not just true

Three cycles are bounded (§4.1–4.3) but nothing in the code records *why*. A future refactor
that removes the "target must be `redir_type == 0`" filter, or that lets the encrypted branch
fall through, silently creates a stack-overflow DoS on a crafted archive.

**Fix:** add an explicit depth parameter with `assert(depth < 2)` to `extract_entry_sink`,
`test_entry_stream` and `extract_entry`, and a one-line "bounded: depth 1/2" comment at each
back-edge. Keep `misc-no-recursion` disabled in the main config (the isolated pass in §2.2 is
the right place for it).

### P4 items

- **P4-1** `foreign_zip.cpp:94`: `cd_size_ = rd32(e + 16 - 4)` is correct (EOCD field at offset
  12) but reads as an off-by-four. Use a named `EOCD_CD_SIZE_OFF = 12` constant alongside the
  existing `*_FIXED` constants.
- **P4-2** `archive_reader.cpp:3403`: the `is_encrypted` branch in `extract_store_entry` is
  unreachable by construction (§4.1). Convert to `assert(!entry.header.is_encrypted)` plus a
  comment, so the invariant is stated once instead of implied by control flow.
- **P4-3** `sandbox/channel.cpp:24,28,80,84`: the only `goto` in `src/`, used for two-handle
  cleanup. Correct today (both fail paths close what they opened). Replace with a small
  `unique_handle` RAII guard for consistency with the rest of the tree.
- **P4-4** Workspace hygiene: `build/static-analysis/*.csv` and `*.nativecodeanalysis.xml` are
  generated artefacts kept in the tree (and were stale); repo root holds untracked
  `f1probe.obj`, `f2blocks.obj`, `f2dbg.obj`, `f2detail.obj`, `f2probe.obj` and several
  `test_*` scratch directories from earlier investigations. Delete the debris; generate the
  CSVs in CI.

---

## 6. What was verified clean (evidence inventory)

| Property | Evidence |
|---|---|
| No analyzer-reported memory defect | 0 non-alpha `clang-analyzer-*` findings; 7 alpha findings traced to FP causes (§4.6–4.7) |
| Iterator validity in `extraction_journal` | `find`/`emplace` repair with all failure paths returning (§4.6) |
| Varint decoding cannot overflow | `vint.cpp` masks bit 0 at `shift == 63` and rejects bits 1–6; 10-byte cap |
| Header body size capped | 2 MiB cap at `header_reader.cpp` (both stream and memory paths) |
| Name length cannot wrap | subtraction form `name_len > body_size - offset` (`parse_file_header`) |
| Extra-record end cannot wrap | `rec_end` clamped via `rec_size <= extra_end - offset` |
| Crypt record cannot be truncated | single `need` check before salt/IV/psw_check reads |
| XATTR count bounded | `xcount > (rec_end - xoff) / 3` + trailing-byte rejection |
| Block size cannot exceed `int` | `byte_cnt` max 3 (4 rejected) → `block_size` ≤ 0xFFFFFF |
| 64→32-bit size truncation | `SIZE_MAX` guard at all 8 sites |
| Decompress bomb cap | `effective_stream_limit` (512 MiB default / 64 GiB floor) |
| Recursion depth | 3 chains, all depth ≤ 2 (§4) |
| C ABI exception egress | 65 `openrar_*` exports: 47 guard with `try`/`catch`; the other 18 are leaf accessors (constants, `malloc`/`free`, POD get/set, `copy_message_to` — allocation-free `memcpy`) or one-line delegators to guarded implementations. No export can let an exception cross the C boundary |
| Symlink/hardlink materialisation | default-deny; `validate_symlink_target`; anchored containment walk |
| Path containment | NT anchored opens with `FILE_OPEN_REPARSE_POINT`; POSIX `openat` + `RESOLVE_BENEATH`/`O_NOFOLLOW` |
| fd inheritance into sandbox | every fd created with `O_CLOEXEC`; only the 3 channel ends cleared |
| Filesystem error handling | `remove` 132/132, `rename` 13/13, `create_directories` 12/12 with `error_code` |
| Unsafe libc | zero `strcpy`/`strcat`/`sprintf`/`gets`/`alloca` |
| Integer↔pointer casts | none on offsets/addresses |
| TODO/FIXME/HACK markers | zero in `src/` and `include/` |
| `using namespace` in headers | zero |

---

## 7. Limitations of this review

- **MSVC `/analyze` was not re-run.** The `C6262`/`C26451`/`C6001`/`C26830`/`C6297` results in
  §P2-2 and §P3 come from `build/static-analysis/*.nativecodeanalysis.xml` dated 2026-11-03,
  ten days before HEAD. `C6001` (uninitialised `salt`/`iv`, `archive_mutator.cpp:1676–1677`)
  and `C26830` (empty-optional unwrap, `:2485`) were manually checked and are **false
  positives** — `salt`/`iv` are initialised under the same `do_encrypt` gate that uses them,
  and the optional is guarded by `updated_headers[i].has_value()` on the same line. A fresh
  `/analyze` pass is still recommended to confirm no regression since 2026-11-03.
- **The two extra TUs were not actually re-analysed.** `run_extra.ps1` produced empty logs for
  `src/sandbox/worker_main.cpp` and `src/wasm/archive_api.cpp` (0 bytes), so those two units
  are covered only by the manual pattern scans, not by the analyzer. They need a re-run with a
  valid `build/static-analysis/extra/compile_commands.json`.
- **No dynamic analysis in this pass.** No sanitizer (ASan/UBSan/TSan) run, no fuzzing, no
  test-suite execution. This review is static + manual only; the concurrency claims
  (`ThreadPool`, `ByteBudget`) are from inspection, not from TSan.
- **`clang-cl`/MSVC `-W4` was not compared against GCC/Clang on Linux.** The 209 diagnostics
  are from the MSVC-triple Clang-Tidy; a Linux run may surface additional
  `-Wsign-compare`-class diagnostics.
- **Only `src/` and `include/` were reviewed**; `tests/`, `tools/`, `bindings/`, `scripts/`
  were not.

---

## 8. Recommended order of work (revised per the architect challenge)

Sequenced for evidence, not convenience (directive D-5):

1. **Build the gates first** (D-4): repair `build/static-analysis/extra/compile_commands.json` so
   `sandbox/worker_main.cpp` and `wasm/archive_api.cpp` are actually analysed; add a stdout-capture
   harness for CLI commands; run a fresh MSVC `/analyze` to re-baseline `C6262`.
2. **P2-1, widened fixes only** — one commit per header, nothing else in the commit (D-5).
3. **P2-2** (stack frames, per-call heap buffer — see §9 D-3) — 2 hours.
4. **P3-1 / P3-2 / P3-3** — an hour total, all small and self-contained.
5. **Re-enable the checks** (`CallAndMessage`, `bugprone-narrowing-conversions`) with zero-tolerance
   `--warnings-as-errors` and **no line-numbered baseline** (§9 D-2).
6. **P2-3** (`cli_main` decomposition) last, bracketed by an extraction benchmark, with
   `cli/main.cpp` excluded from step 2 if the decomposition is likely to land soon (§9 D-5).

---

## 9. Architect-challenge outcome — corrected fix outlines

The §5 plan was put through an adversarial principal-architect challenge
([record](code-quality-review-202611-architect-challenge.md)). Verdict:
**🟡 conditional approval, revisions required.** Three of the proposed fixes were unsound and two
verification gates did not exist in the claimed form. The corrected outlines below **supersede §5**
for P2-1 and P2-2; P2-3's gate is corrected in D-4(b).

### D-1 — Split the conversion population (supersedes P2-1 step 1)

The proposed `narrow<T>()` checked only by `assert` compiles to a bare `static_cast` under
`NDEBUG`, i.e. in every shipped binary — it would remove the warning without removing the
truncation, which is worse than leaving it visible. Two APIs are required:

```cpp
// internal, provably-bounded values only (iterator deltas, capped reserves, masked ids)
template <class To, class From> constexpr To narrow(From v) noexcept;

// values parsed out of a header: failure is an input-validation outcome, not an assertion
template <class To, class From>
constexpr std::optional<To> checked_narrow(From v) noexcept;   // fail-closed at the call site
```

Tabulate the 35 sites by trust level before editing. Acceptance test: an archive declaring
`unp_size = 2^63` on a 32-bit target must fail closed, not truncate.

### D-2 — No line-keyed baseline (supersedes P2-1 step 4)

A `(file, line, check)` allowlist of 209 entries invalidates itself on the first refactor —
and the P2-3 decomposition rewrites 1,095 lines, invalidating most of it. The practical response
to that noise is regenerating the baseline, which re-accepts whatever was fixed. Fix to zero
(~60 real edits, already mechanical) and gate with `--warnings-as-errors`; accept a genuine false
positive as a single `NOLINT` with a reason at the site.

### D-3 — Per-call heap buffer only (supersedes P2-2 step 1)

The "per-reader scratch member" option in §5 is a **data race**: `-mt` extract/test submits
per-entry jobs to `core::ThreadPool`, so a mutable member buffer would be shared mutable state
across workers. Use a per-call allocation hoisted **outside** the extent loop:

```cpp
const auto buf = std::make_unique<core::byte[]>(64 * 1024);  // once per call, not per extent
```

Race-free by construction, realises the full stack saving, and adds one allocation per entry
rather than per 64 KiB read. Verification: a TSan run of `-mt 4` over a multi-extent archive.

### D-4 — Build the gates before the refactors

- (a) Repair `build/static-analysis/extra/compile_commands.json` and add
  `sandbox/worker_main.cpp` + `wasm/archive_api.cpp` to the standing suite. `worker_main.cpp` is
  the sandboxed parser for hostile input — leaving it unanalysed defeats the purpose of the gate.
- (b) **P2-3 has no output gate today.** `tests/unit/cli_tests.cpp` (3,052 lines) spawns the binary
  via `std::system` and asserts exit codes and filesystem effects — it does **not** compare stdout.
  Add a capture harness over the extracted `cmd_*` functions *before* decomposing `cli_main`.
- (c) P2-2 is verified by the named frame disappearing from a **fresh** `/analyze` at the default
  threshold — not by lowering the threshold to 16 KiB, which would raise the count.

### D-5 — Sequencing

Gates (D-4) → fresh `/analyze` baseline → P2-1 widening sweep (one commit per header) → P2-2 →
P3-1/2/3 → re-enable checks with zero tolerance → P2-3 last, bracketed by an extraction benchmark.
Exclude `cli/main.cpp` from the widening sweep if the decomposition is imminent, to avoid redoing
10 of the 35 sites.

### D-6 — Record the alpha disposition in source

Add `NOLINTNEXTLINE` with a one-line reason at the four `emplace(...).first` sites and the three
struct-definition sites, or drop `ReturnPtrRange` from the standing suite and keep it manual.
Either is fine; leaving them unexplained means every future reviewer re-derives §4.6–§4.7.

### D-7 — Qualify the verdict

Applied to §1: the verdict now names the static-only basis and the missing TSan/ASan leg.

### D-8 — Fault-injection case for P3-2

Delete the destination archive (or revoke write permission) between `exists()` and
`last_write_time()`, and assert the `-tk` path reports its own error rather than the generic
top-level one. The repository already has the hook shape for this (`should_fail_next_sync()`).

### Directive disposition

| Directive | Addresses | Status in this revision |
|---|---|---|
| D-1 | `narrow<>` assert-only (R-1) | adopted, §9 |
| D-2 | line-keyed baseline (R-2) | adopted, §9 |
| D-3 | scratch-buffer race (R-3) | adopted, §9 |
| D-4 | missing gates (R-4, R-5) | adopted, §9 |
| D-5 | sequencing | adopted, §8 |
| D-6 | alpha disposition | adopted, §9 |
| D-7 | verdict qualification | applied, §1 |
| D-8 | P3-2 fault injection | adopted as an acceptance criterion |

**Not yet done, and required before implementing:** the fresh `/analyze` baseline (D-4c), the CLI
capture harness (D-4b) and the extra-TU compile DB repair (D-4a). The fixes above are sound; the
evidence base for two of them is not yet in place.
