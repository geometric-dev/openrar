---
name: static-analysis
description: >-
  Run, configure, and interpret static analysis on OpenRAR using Clang-Tidy (VS LLVM 22.1.3) and MSVC /analyze.
  Activates when checking for memory leaks, buffer bounds violations, integer overflows, lifetime hazards,
  or recursion issues (such as misc-no-recursion), or when generating compile commands and deduplicated CSV findings.
---

# OpenRAR Static Analysis Runbook

This skill guides running, reproducing, and analyzing static analysis passes across the OpenRAR codebase using **Clang-Tidy (LLVM 22.1.3 from Visual Studio 18)** and **MSVC `/analyze`**.

It covers deep leak/lifetime checking, buffer bounds, arithmetic overflow, an isolated recursion analysis pass (which caught the `nfc()` stack overflow bug), and deduplication of findings into CSV reports.

---

## Quick Reference

If the compile database and scripts are already prepared in `build/static-analysis/`, the quickest way to execute analysis:

```powershell
# Quickest way to repeat main Clang-Tidy analysis (4 workers parallel):
pwsh -NoProfile -File build\static-analysis\run_tidy.ps1

# Or run the complete automated end-to-end pipeline:
pwsh -NoProfile -File .agents\skills\static-analysis\scripts\run_all.ps1
```

Helper scripts are available under [scripts/](./scripts/):
- [run_all.ps1](./scripts/run_all.ps1): Executes steps 1 through 6 sequentially.
- [generate_db.ps1](./scripts/generate_db.ps1): Generates `compile_commands.json` via Ninja inside the VS developer environment.
- [run_tidy.ps1](./scripts/run_tidy.ps1): Runs the main Clang-Tidy analysis pass.
- [run_recursion.ps1](./scripts/run_recursion.ps1): Runs the dedicated `misc-no-recursion` pass.
- [run_extra.ps1](./scripts/run_extra.ps1): Runs analysis on `worker_main.cpp` and `archive_api.cpp`.
- [run_msvc_analyze.ps1](./scripts/run_msvc_analyze.ps1): Runs MSVC `/analyze:only` with `EspXEngine.dll`.
- [export_findings.ps1](./scripts/export_findings.ps1): Deduplicates warnings into CSV reports.

---

## Detailed Step-by-Step Procedure

### 1. Generate `compile_commands.json`

The Visual Studio generator does not write a compile database, so configure a separate Ninja tree inside the VS developer environment (`vcvars64.bat`). Configuring is sufficient; building is not required.

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake -G Ninja -S . -B build/static-analysis -DCMAKE_BUILD_TYPE=Debug -DOPENRAR_BUILD_TESTS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_MAKE_PROGRAM=C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"'
```

- **Scope:** Covers **69 of the 71** source files.
- **Excluded Targets:** `src/sandbox/worker_main.cpp` (only compiled when tests are enabled) and `src/wasm/archive_api.cpp` (only compiled with `OPENRAR_INMEM_ARCHIVE=ON`) are excluded from the main Ninja tree.
- **Handling Excluded Targets:** A dedicated 2-entry database is stored at `build/static-analysis/extra/compile_commands.json` (backed up at [resources/extra_compile_commands.json](./resources/extra_compile_commands.json)). Analyze these using `-p build/static-analysis/extra`.

---

### 2. Main Clang-Tidy Analysis (Leaks, Bounds, Overflow)

Runs path-sensitive analysis, alpha security checkers, CERT C++ checks, and bugprone checkers across the codebase.

```powershell
$ct = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe'

$checks = '-*,' +
  'clang-analyzer-core.*,clang-analyzer-cplusplus.*,clang-analyzer-unix.*,' +
  'clang-analyzer-security.*,-clang-analyzer-security.insecureAPI.*,' +
  'clang-analyzer-alpha.unix.SimpleStream,clang-analyzer-alpha.cplusplus.SmartPtr,' +
  'clang-analyzer-alpha.cplusplus.InvalidatedIterator,clang-analyzer-alpha.cplusplus.IteratorRange,' +
  'clang-analyzer-alpha.cplusplus.MismatchedIterator,clang-analyzer-alpha.security.ReturnPtrRange,' +
  'clang-analyzer-alpha.unix.cstring.OutOfBounds,clang-analyzer-alpha.unix.cstring.BufferOverlap,' +
  'clang-analyzer-alpha.unix.cstring.UninitializedRead,clang-analyzer-alpha.core.PointerArithm,' +
  'clang-analyzer-optin.taint.TaintedAlloc,' +
  'bugprone-unique-ptr-array-mismatch,bugprone-shared-ptr-array-mismatch,' +
  'bugprone-unhandled-self-assignment,bugprone-use-after-move,bugprone-dangling-handle,' +
  'bugprone-return-const-ref-from-parameter,cert-oop54-cpp,cert-mem57-cpp,' +
  'bugprone-implicit-widening-of-multiplication-result,bugprone-misplaced-widening-cast,' +
  'bugprone-too-small-loop-variable,bugprone-signed-char-misuse,bugprone-sizeof-expression,' +
  'bugprone-sizeof-container,bugprone-not-null-terminated-result,' +
  'bugprone-misplaced-pointer-arithmetic-in-alloc,bugprone-undefined-memory-manipulation,' +
  'bugprone-suspicious-memset-usage,bugprone-suspicious-memory-comparison,' +
  'bugprone-suspicious-stringview-data-usage,bugprone-string-constructor,bugprone-infinite-loop,' +
  'bugprone-unchecked-optional-access,bugprone-pointer-arithmetic-on-polymorphic-object,' +
  'bugprone-narrowing-conversions,bugprone-unsafe-functions'

& $ct -p build/static-analysis `
    "--checks=$checks" `
    --allow-enabling-analyzer-alpha-checkers `
    --extra-arg=-Xclang --extra-arg=-analyzer-config `
    --extra-arg=-Xclang --extra-arg=aggressive-binary-operation-simplification=true `
    '--header-filter=.*[\\/]src[\\/].*' `
    --quiet src/unicode/unicode_nfc.cpp
```

#### Crucial Flags Explained
1. **`-*` prefix in `--checks`:** Clang-tidy appends `--checks` to checks defined in the repository's `.clang-tidy`. Starting with `-*` suppresses all inherited checks so only this explicit suite runs.
2. **`--allow-enabling-analyzer-alpha-checkers`:** Required by Clang-Tidy to permit experimental alpha checks.
3. **Four `--extra-arg` flags:** In LLVM 22.1.3, alpha iterator checkers crash if `aggressive-binary-operation-simplification` is `false`. Setting this via `CheckOptions` in `.clang-tidy` or `--config` does not take effect in Clang-Tidy 22; it must be injected into the frontend using `-Xclang`.
4. **`--header-filter=.*[\\/]src[\\/].*`:** Reports warnings occurring in header files inside `src/`. Header warnings will appear once for every source file that includes the header, which is resolved during the deduplication step.

#### Execution & Timings
- Run in parallel using `ForEach-Object -ThrottleLimit 4 -Parallel` writing one log per file to `build/static-analysis/tidy/<file>.txt`.
- **Duration:** ~10 minutes total for 69 files.
- **Slowest files:**
  - `src/cli/main.cpp` (~200 seconds)
  - `src/dll/dll_api.cpp` (~155 seconds)

---

### 3. Dedicated Recursion Check (`misc-no-recursion`)

The project's `.clang-tidy` explicitly disables `misc-no-recursion`. It must be executed as an isolated pass:

```powershell
& $ct -p build/static-analysis --checks='-*,misc-no-recursion' '--header-filter=.*[\\/]src[\\/].*' --quiet src/unicode/unicode_nfc.cpp
```

- **Run over all files:** Execute across all 69 units via [run_recursion.ps1](./scripts/run_recursion.ps1).
- **Duration:** Only 1–2 minutes total.
- **Significance:** This specific check surfaced the uncontrolled recursion vulnerability in `nfc()` (`src/unicode/unicode_nfc.cpp`).

---

### 4. Extra Files Analysis (`worker_main.cpp` & `archive_api.cpp`)

Execute Clang-Tidy and MSVC `/analyze` against the secondary compilation database:

```powershell
pwsh -NoProfile -File .agents\skills\static-analysis\scripts\run_extra.ps1
# Or:
pwsh -NoProfile -File build\static-analysis\run_extra.ps1
```

Logs are written to `build/static-analysis/extra/`.

---

### 5. MSVC `/analyze` Pass

Runs MSVC's native static analysis engine using the curated leak and overflow ruleset:

```powershell
pwsh -NoProfile -File .agents\skills\static-analysis\scripts\run_msvc_analyze.ps1
# Or:
pwsh -NoProfile -File build\static-analysis\run_msvc_analyze.ps1
```

- **Ruleset:** [resources/leaks_overflows.ruleset](./resources/leaks_overflows.ruleset)
- **Plugin:** `EspXEngine.dll`
- **Compiler flags:** `/analyze:only /analyze:external- /external:anglebrackets /external:W0 /analyze:ruleset "<ruleset>" /analyze:plugin EspXEngine.dll`
- **Output:** Individual logs in `build/static-analysis/msvc/`.

---

### 6. Combining & Deduplicating Logs into CSV

Collapse individual text logs into deduplicated CSV tables:

```powershell
# Clang-Tidy findings deduplication:
$rx = '^(?<file>[A-Za-z]:\\[^:]+):(?<line>\d+):(?<col>\d+): (?<sev>warning|error): (?<msg>.*) \[(?<check>[\w.,-]+)\]$'
Get-ChildItem build/static-analysis/tidy/*.txt | ForEach-Object { Get-Content $_.FullName } |
  ForEach-Object { if ($_ -match $rx) { [pscustomobject]@{ File=$matches.file; Line=[int]$matches.line; Check=$matches.check; Msg=$matches.msg } } } |
  Sort-Object File, Line, Check, Msg -Unique | Export-Csv build/static-analysis/tidy_findings.csv -NoTypeInformation

# MSVC /analyze findings deduplication:
$rxMsvc = '^(?<file>[A-Za-z]:\\[^()]+)\((?<line>\d+)\)\s*:\s*(?<sev>warning|error)\s+(?<code>C\d+)\s*:\s*(?<msg>.*)$'
Get-ChildItem build/static-analysis/msvc/*.txt | ForEach-Object { Get-Content $_.FullName } |
  ForEach-Object { if ($_ -match $rxMsvc) { [pscustomobject]@{ File=$matches.file; Line=[int]$matches.line; Code=$matches.code; Msg=$matches.msg } } } |
  Sort-Object File, Line, Code, Msg -Unique | Export-Csv build/static-analysis/msvc_findings.csv -NoTypeInformation
```

Or run the consolidation helper:
```powershell
pwsh -NoProfile -File .agents\skills\static-analysis\scripts\export_findings.ps1
```

---

## Detailed Checkers and Flag Reference

For the comprehensive explanation of all enabled checks, ruleset IDs, and compiler flag workarounds, see [checkers.md](./references/checkers.md).
