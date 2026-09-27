# OpenRAR ABI Freeze — v1.30.0 (2.0 LTS)

> **Status:** Normative freeze inventory for the C ABI surface
> (`include/openrar/openrar_dll.h` + `include/openrar/openrar.hpp`), the two
> WASM modules (`src/wasm/wasm_api.{hpp,cpp}`, `src/wasm/archive_api.{hpp,cpp}`)
> and the shared contract core (`src/api/abi_contract.hpp`). Referenced from
> `docs/versioning.md` (what may change per bump class) and
> `docs/dll-integration-spec.md` (embedding contracts). Enforced by
> `tests/unit/abi_layout_tests.cpp`, the `abi_export_parity` and
> `js_error_mirror_parity` ctest gates, and the compile-time pins in
> `src/dll/dll_api.cpp:39-134`.
>
> **Freeze rule:** this document and the code must match at the v1.30.0 tag.
> Any change after the freeze commit is an ABI event and requires the
> `docs/versioning.md` MAJOR/MINOR decision made explicitly in the same
> change, with this document updated in the same commit.

## 1. Versioning contract (unchanged by the freeze)

- `OPENRAR_DLL_API_VERSION` stays **1** (openrar_dll.h:62). Additive exports
  MUST NOT bump it: embedders probe with strict equality (spec §3), so a bump
  strands every host. New capability ships as a new export behind a feature
  bit and/or `GetProcAddress`/`dlsym`.
- The shared-library file version / soname derives from the project version
  (`VERSION/SOVERSION` target properties) — a MAJOR bump moves it
  automatically.
- Patch is a monotonic commit counter; it never implies compatibility change
  (`docs/versioning.md`).

## 2. Calling conventions & ownership (the bit-level contract)

- **Calling convention:** `OPENRAR_DLL_CALL` = `__cdecl` on Windows, default
  elsewhere. Never `__stdcall`/COM. Export macro `OPENRAR_DLL_API`:
  `dllexport`/`dllimport` on Windows, `visibility("default")` on POSIX
  (openrar_dll.h:32–54).
- **Single heap:** `openrar_alloc`/`openrar_archive_alloc` are aliases of
  `malloc`; every returned buffer (`out_ptr`, `entries_out`, `paths_out`,
  `offsets`, comments, owner names) is freed by the caller with
  `openrar_free`/`openrar_archive_free` (also aliases). Never
  `free()`/`delete` — allocator pairing is part of the contract.
- **malloc(0):** never relied upon — empty outputs are `nullptr` + length 0
  (`heap_dup`, abi_contract.hpp:90; audit report M12). `free(nullptr)` is
  safe.
- **Error state:** thread-local. `openrar_last_error` /
  `openrar_archive_get_error` return the message of the most recent call ON
  THE CALLING THREAD, valid until the next archive call on that thread.
- **Handles:** ids are nonzero; 0 = failure/absence. Handle ids are
  monotonically increasing with wrap-around skipping 0 (abi_contract.hpp:176;
  audited I3). `openrar_archive_close` on an id twice is safe (the second
  erase is a no-op on an unknown id).
- **Callbacks:** progress/cancel run on the calling thread with NO DLL-internal
  lock held (`HandleTable::pin` returns a `shared_ptr` so the lock is released
  before work — report L12). Callbacks must not re-enter the same handle.
- **Struct packing:** all four public structs are `#pragma pack(push,1)`.
  `_pad` in the 64-byte entry is RESERVED by the shared WASM contract
  ("consumers must zero"); widening it is a documented non-goal
  (dll-enhancement-plan §5). NOTE: `openrar_archive_info_t.recovery_size` is
  an UNALIGNED `uint64_t` at offset 12 (a consequence of pack(1) with the
  leading three `uint32_t`s) — frozen reality since v1.6.0; hosts on
  strict-alignment targets must read it byte-wise.
- **Integer widths:** every field is fixed-width (`uint32_t`/`uint64_t`);
  `MAX_WIN_SIZE` is deliberately `uint64_t` so the 4 GiB wasm32 cap cannot
  truncate through `size_t` (abi_contract.hpp:62–69 — the wasm32 defect the
  comment records).

## 3. Public struct layouts (machine-checked golden: `tools/abi_layout.json`)

Runtime-verified on every platform by `abi_layout_tests` (C and C++ compile
modes); compile-time pinned in `src/dll/dll_api.cpp:107-134` (absolute
offsets + `alignof`); the canonical C++ `ArchiveEntryOut` is pinned to the C
struct by offsetof pairs (`dll_api.cpp:86-105`).

| Struct | Size | Fields (offset, width) |
| :--- | ---: | :--- |
| `openrar_archive_entry_t` | 64 | path_offset(0,4) path_len(4,4) is_dir(8,4) method(12,4) is_encrypted(16,4) crc32(20,4) size(24,8) packed_size(32,8) mtime(40,8) _pad(48,16) |
| `openrar_entry_ex_t` | 48 | attrs(0,4) host_os(4,4) mtime_ft(8,8) ctime_ft(16,8) atime_ft(24,8) flags(32,4) win_size(36,4) redir_type(40,4) version_needed(44,4) |
| `openrar_entry_owner_t` | 20 | uid(0,8) gid(8,8) flags(16,4) |
| `openrar_archive_info_t` | 24 | flags(0,4) volume_index(4,4) volume_count(8,4) recovery_size(12,8, UNALIGNED) comment_len(20,4) |
| `ArchiveInputFile` (wasm32 only) | 32 | path(0,4) data(4,4) data_len(8,4) [pad 12..15] mtime_unix(16,8) is_dir(24,1) reserved(25,3) |

`crc32 = 0` means UNVERIFIED, not a verified-zero CRC. Timestamps in
`openrar_entry_ex_t` are FILETIME (UTC, 100 ns); absence = 0 with the HAS_*
flag clear. `redir_type`: 0 none / 1 unixsymlink / 2 winsymlink / 3 junction
/ 4 hardlink / 5 filecopy.

## 4. Error-code table (`enum RarError`, openrar_dll.h:122–164)

| Value | Code | Since |
| ---: | :--- | :--- |
| 0 | `RAR_OK` | v1.1.0 |
| 1 | `RAR_ERR_PARTIAL_OK` — ONLY `extract_all` (some entries succeeded) | v1.1.0 |
| −1 | `RAR_ERR_NOT_RAR` | v1.1.0 |
| −2 | `RAR_ERR_UNSUPPORTED_FEATURE` | v1.1.0 |
| −3 | `RAR_ERR_TRUNCATED` | v1.1.0 |
| −4 | `RAR_ERR_CRC_MISMATCH` | v1.1.0 |
| −5 | `RAR_ERR_NOMEM` | v1.1.0 |
| −6 | `RAR_ERR_IO` | v1.1.0 |
| −7 | `RAR_ERR_BAD_PASSWORD` | v1.2.0 |
| −8 | **RESERVED — never assign** (binary compat with range-checking hosts) | — |
| −9 | `RAR_ERR_INVALID_ARG` | v1.1.0 |
| −10 | **RESERVED — never assign** (same rationale as −8) | — |
| −11 | `RAR_ERR_ABORTED` | v1.1.0 |
| −12 | `RAR_ERR_ENCRYPTED` (HEAD_CRYPT reached, no password; `_ex`/`_pw`/file-handle surfaces) | v1.2.0 |
| −13 | `RAR_ERR_MISSING_VOLUME` | v1.3.0 |
| −14 | `RAR_ERR_BUSY` (open file-mode handle holds the archive; mutation exports refuse up front) | v1.4.0 |
| −15 | `RAR_ERR_LIMIT_EXCEEDED` (caller-imposed cap hit) | v1.21.x |
| −16 | **next available slot** | |

Numeric values are pinned by `static_assert` in `src/dll/dll_api.cpp:58-80,
943-945` and `src/api/abi_contract.hpp:59-60`, and by the C-mode runtime
check in `tests/unit/abi_header_c_compat.c`. The JS mirror is gated by the
`js_error_mirror_parity` ctest test (CODE_MAP + RarErrorCode union == this
table, minus OK/PARTIAL_OK which are never thrown as errors).

## 5. Feature-bit registry (`openrar_abi_features()`, openrar_dll.h:72–112)

Bits are assigned ONLY when the gated exports ship; the next free slot is
**bit 17**. No speculative reservations. Parity between the registry and
`openrar_abi_features()`'s returned mask is machine-checked
(`abi_layout_tests`, test 3).

| Bit | Name | Ships |
| ---: | :--- | :--- |
| 0 | LIST_PROGRESS | v1.1.0 |
| 1 | LIST_PASSWORD | v1.2.0 |
| 2 | HANDLE_OPEN_PROGRESS | v1.2.0 |
| 3 | FILE_HANDLE | v1.3.0 |
| 4 | MUTATION | v1.4.0 |
| 5 | ENTRY_EX | v1.5.0 |
| 6 | PACKAGE_VERSION | v1.7.0 |
| 7 | SET_LIMITS | v1.10.0 |
| 8 | REPAIR | v1.11.0 |
| 9 | CREATE | v1.14.0 |
| 10 | FILTERS | v1.15.0 |
| 11 | OWNER | v1.17.0 |
| 12 | DICT_EX | (RAR 7.0 dict sizing) |
| 13 | VOL_ENCRYPT | (multi-volume encryption) |
| 14 | REC_VOL | v1.20.0 |
| 15 | PARALLEL_COMPRESS | (−mt) |
| 16 | MMAP | v1.25.0 |
| 17+ | free (next assignment order: 17) | — |

Reserve convention (openrar_dll.h:86–88): future open-time options ship as a
new `_ex` open export behind a new bit — never as signature changes to
`openrar_archive_open_file`.

## 6. Export list (58 symbols)

The canonical, per-symbol list lives in `tools/abi_exports_canonical.txt`
(one symbol per line, sorted) — regenerated ONLY by
`tools/abi_exports.py --generate` when an additive export legitimately
lands, and machine-diffed against every built binary by the
`abi_export_parity` ctest gate (dumpbin/llvm-nm/nm; loud `[SKIP]` on hosts
without a symbol tool; CI wiring lands with the post-rebase workflow
changes). Families, with since-versions:

- **Version/capability (4):** `openrar_version`, `openrar_archive_version`,
  `openrar_package_version_string` (v1.7.0), `openrar_abi_features`.
- **Allocator (4):** `openrar_alloc`/`openrar_free`,
  `openrar_archive_alloc`/`openrar_archive_free` (aliases; single malloc
  heap).
- **Error state (2):** `openrar_last_error`, `openrar_archive_get_error`
  (thread-local; see §2).
- **Block codec (4):** `openrar_compress`/`_compress2`/`_decompress`/
  `_decompress2` — `win_size` in BYTES (`OPENRAR_WINDOW_*` constants);
  0 = 2 MiB default.
- **Streaming block codec (6):** `openrar_stream_create` (`window_log2`,
  log2 — units deliberately differ from compress2, documented in
  openrar_dll.h:174–177), `_stream_feed`, `_stream_finish`, `_stream_free`,
  `_stream_set_progress`, `_stream_set_cancel`.
- **Buffer archive one-shot (5):** `openrar_archive_list`, `_list_free`,
  `_extract`, `_extract_all` (`RAR_ERR_PARTIAL_OK` lives here only),
  `_create`.
- **Handle API — buffer (5):** `openrar_archive_open`, `_open_ex`,
  `_close`, `_handle_list`, `_handle_extract`, `_handle_extract_all`.
- **File helpers (5):** `openrar_archive_list_file`, `_extract_file`,
  `_extract_file_to_path`, `_create_from_paths`, `_create_to_file`.
- **Listing `_ex`/`_pw` (3):** `openrar_archive_list_ex`,
  `openrar_archive_list_file_ex`, `openrar_archive_list_file_pw`.
- **File-mode handles (5):** `openrar_archive_open_file`,
  `openrar_archive_handle_extract_to_path`,
  `openrar_archive_handle_test`,
  `openrar_archive_handle_read_entry_region` (bit 16),
  `openrar_archive_handle_set_limits` (bit 7 — sets CALLER budgets;
  the non-disableable floors are documented in §8).
- **Mutation (2):** `openrar_archive_delete_entries_file`,
  `openrar_archive_add_files_file`.
- **Creation (4):** `openrar_archive_create_file`, `_ex`, `_opts`,
  `_opts_mt`.
- **Extended metadata (6):** `openrar_archive_handle_entry_ex`,
  `openrar_archive_entry_ex_free`, `openrar_archive_handle_entry_owner`,
  `openrar_archive_entry_owner_free`, `openrar_archive_handle_info`,
  `openrar_archive_handle_info` free path via `openrar_free`.
- **Recovery (3):** `openrar_archive_repair`,
  `openrar_archive_create_rev_volumes`,
  `openrar_archive_add_recovery_record`.

(The `*_compress_*` aliases visible in the WASM block codec
(`src/wasm/wasm_api.hpp:54–63`) are a JS-binding compatibility layer of the
wasm surface, not part of the native export list.)

## 7. WASM module surfaces (frozen as-shipped)

- **Block codec** (`wasm/dist/openrar.js`): `WASM_API_VERSION 2`
  (`src/wasm/wasm_api.hpp:16`; "v2 was never shipped as embind — the module
  surface is the raw C ABI"). Exports: version/alloc/free,
  compress/decompress ×plain/2, streaming encoder (+ `*_compress_*` aliases)
  and the streaming decoder family.
- **Archive module** (`wasm/dist/openrar-archive.js`):
  `ARCHIVE_WASM_API_VERSION 2` (`src/wasm/archive_api.hpp:34`); v1→v2
  corrected the export set (`_openrar_archive_list_free`) and added
  struct-based create + hooks + numeric error getter — THE cautionary tale
  for binding conformance (ROADMAP freeze prereq 3). `ArchiveInputFile` is
  32 bytes on wasm32 (static_assert, archive_api.hpp:102–104); i64 callback
  params cross as BigInt (WASM_BIGINT=1, addFunction signature "vijj").
- **JS mirror:** `wasm/js/openrar-archive.js` `CODE_MAP` +
  `openrar-archive.d.ts` `RarErrorCode` — parity gated by
  `js_error_mirror_parity`.
- Export-set check: `wasm/js/check-exports.mjs` (wasm CI job) — the native
  `abi_export_parity` gate mirrors it for the shared library.

## 8. Library-mode containment (the non-disableable floor; §5.1)

Two DISTINCT layers — the freeze documentation draws the line explicitly
(pre-analysis §1.2):

1. **Absolute floors — NOT reachable by any embedder call, non-disableable:**
   `Decompressor50::MAX_STREAM_OUTPUT` allocation cap (constexpr,
   decompressor50.hpp:203; enforced decompressor50.cpp:942/952); KDF
   iteration ceiling (`lg2_count ≤ 24`, pinned at both header paths,
   `test_kdf_cap_pinned`); dictionary/window caps; header-walk loop bounds
   and entry-count caps; the §4.1 syscall-level path-containment walk (NO
   disable switch exists — `--no-mmap`/`OPENRAR_NO_MMAP` downgrades the read
   engine only, never containment). Pinned by
   `library_mode_limits_non_disableable` and
   `library_containment_no_disable_switch` (v1.30.0 M2).
2. **Caller budgets — tunable by design, up to unlimited:**
   `ExtractionLimits` (extraction_limits.hpp:33–58, every field defaults
   UNLIMITED) and `openrar_archive_handle_set_limits` (`UINT64_MAX` =
   unlimited, openrar_dll.h:310–314). These are the embedder's OWN resource
   policy, not containment.

Embedders who need the broker/worker containment model (SECURITY
ARCHITECTURE §5.1) are advised to self-sandbox the host process (§7.3);
the advisory is normative in `docs/dll-integration-spec.md` §13 and
`SECURITY.md` (v1.30.0 M2/M6).

## 9. C++ wrapper (`include/openrar/openrar.hpp`)

Header-only RAII overloads over the C ABI — a SOURCE-compatibility surface,
not a binary one (headers compile into the host; no ABI pins beyond the C
structs it re-exports). Source-breaking wrapper changes are a MAJOR trigger
per `docs/versioning.md`; the freeze covers it by reference to the C rules.

## 10. Version plumbing status (verified v1.30.0)

- `openrar/version.h` — GENERATED at build time via `configure_file`
  (`CMakeLists.txt:103–105` from `include/openrar/version.h.in`); installed
  with the package. (`docs/versioning.md` "Planned" list is stale on this
  point — sync lands in the M7 docs pass.)
- `openrar_package_version_string` — SHIPPED (bit 6), backed by the project
  version.
- `openrar --version` — SHIPPED (`src/cli/main.cpp:3081`).
- Version-consistency gate: `project(VERSION)` == `wasm/js/package.json` is
  a CI gate-leg step; the 1.30.0 release commit bumps BOTH (post-rebase).

## 11. Solid-parameter cleanup (freeze prereq 1, third clause) — VERIFIED

The v1.21.2 front-load closed the solid/window-parameter debt on the C ABI:
off-grid dictionary values snap to the FCI grid before the compressor is
constructed (`test_off_grid_dict_snap_roundtrip`), window units are
documented per surface (§6 block codec note), and the creation surface's
`int solid` is a documented boolean flag on `create_file_ex`/`_opts`/`_opts_mt`
— not the ambiguous parameter the audit flagged. No `solid`-parameter debt
remains on the frozen surface. Verification: CHANGELOG 1.21.2 (off-grid
dictionary windows) + the current header (openrar_dll.h:623–639) + the
mutation solid-refusal contract (openrar_dll.h:531–538).

## 12. Audit P2 ledger — freeze triage record (prereq 7) — COMPLETE (M2)

The v1.6.0→v1.21.0 audit's residual P2s shipped fixed in v1.21.2
(CHANGELOG.md:738–800). The M2 re-walk verified each item is STILL fixed
in the current tree (source guards present with their v1.21.2 citations,
regression tests where named) and swept the frozen-surface defect classes
(allocator pairing, error mapping, handle lifetime, callback re-entrancy,
struct packing — all enforced or pinned by the §14 gates; the
dll-enhancement-plan §5 non-goals re-affirmed as frozen debt, nothing
reopened):

| P2 item (v1.21.2) | Evidence still fixed |
| :--- | :--- |
| Off-grid dictionary windows | `test_off_grid_dict_snap_roundtrip` (tests/unit/mutation_tests.cpp:1265), green in the suites |
| `:`/`:$DATA` stream names (truncation vector) | guard present, src/io/win32_meta.cpp:147–155 (rejects empty/`$DATA` any-case before CREATE_ALWAYS; cites v1.21.2) |
| FHEXTRA_OWNER 255-byte name limit | reader discards over-long records (src/format/header_reader.cpp:809–815), writer truncates (src/format/header_writer.cpp:363–368) |
| `start_vol` orphan volume | cleanup guard registered immediately after volume open (src/archive/archive_mutator.cpp:3459–3462) |
| `-hp` append error fidelity | `RAR_ERR_UNSUPPORTED_FEATURE` + "mutating header-encrypted archive" (tests/unit/mutation_tests.cpp:587) |
| Non-throwing cleanup | structural: `std::error_code` overloads throughout the boundary code (B2 residual sweep note, src/dll/dll_api.cpp:46–53) |

New findings touching the frozen surface: none — the sweep found no open
defect in the allocator-pairing, error-mapping, handle-lifetime,
callback-re-entrancy, or struct-packing classes beyond what the M1 gates
now pin mechanically. The freeze commit may proceed.

## 13. v1.29.0 inherited surface (placeholder — filled at the freeze commit)

The transcoder arc (arc/v1.29.0-transcoder, merged FIRST per the parallel
coordination rules) may land additive C ABI exports for migration flows.
Per Gate 0: this arc INHERITS and documents whatever shipped — no redesign.
At the freeze commit (M7, post-rebase) this section records: new exports
(name, since v1.29.0, feature bit if gated), error-code additions (next
slot −16), registry additions, and the layout goldens of any new structs.
The `abi_layout_tests`/`abi_export_parity` gates are re-run against the
rebased tree as the freeze evidence. *(Pending rebase.)*

## 14. Enforcement inventory (how this freeze is checked, not just written)

| Gate | What | Where |
| :--- | :--- | :--- |
| `static_assert` (compile) | enum equivalence ×16, struct sizes, absolute offsets ×4 structs, `alignof` == 1, LIMIT/BUSY numeric pins | src/dll/dll_api.cpp:58–134, 943–945; src/api/abi_contract.hpp:59–60, 86; src/wasm/archive_api.hpp:102–104 |
| `abi_layout_tests` (ctest) | runtime layout vs `tools/abi_layout.json`, C-mode header compile + layout (`abi_header_c_compat.c`), feature-mask parity, version probes | tests/unit/abi_layout_tests.cpp |
| `abi_export_parity` (ctest) | built binary's `^openrar_\w+$` export set == canonical list | tools/abi_exports.py + tools/abi_exports_canonical.txt |
| `js_error_mirror_parity` (ctest) | JS CODE_MAP + RarErrorCode union == C enum (minus OK/PARTIAL_OK) | tools/check_js_error_mirror.py |
| wasm `check-exports.mjs` | wasm module export contract | wasm CI job |
| version consistency (CI gate-leg) | `project(VERSION)` == `wasm/js/package.json` | CMakeLists.txt ↔ wasm/js/package.json |

CI workflow wiring for the two new ctest gates is explicit (they run via
`ctest` in the existing build-and-test legs — no workflow edit required);
the dedicated export-list CI step on release assets lands with the
post-rebase workflow changes (parallel-coordination ownership rule).
