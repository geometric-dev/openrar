# Static Analysis Checkers & Compiler Flags Reference

## 1. Clang-Tidy Checks

The OpenRAR deep static analysis checks focus on memory safety, bounds violations, lifetime/ownership hazards, integer overflows, and unhandled recursion.

### Core & Security Analyzers
- `clang-analyzer-core.*`: Core path-sensitive analysis (null dereference, uninitialized values, division by zero).
- `clang-analyzer-cplusplus.*`: C++ specific semantics (new/delete leaks, virtual call in constructor/destructor).
- `clang-analyzer-unix.*`: POSIX/C standard library calls, memory management (`malloc`/`free`).
- `clang-analyzer-security.*`: Buffer overflows, insecure string operations.
- `-clang-analyzer-security.insecureAPI.*`: Explicitly disabled to avoid excessive noise regarding standard runtime functions.
- `clang-analyzer-optin.taint.TaintedAlloc`: Detects allocations governed by untrusted, unconstrained inputs.

### Alpha (Experimental) Checkers
Alpha checkers provide cutting-edge path sensitivity but require explicit enablement flags:
- `clang-analyzer-alpha.unix.SimpleStream`: Stream and file handle leak detection.
- `clang-analyzer-alpha.cplusplus.SmartPtr`: Smart pointer null dereferences and dangling access.
- `clang-analyzer-alpha.cplusplus.InvalidatedIterator`: Iterator invalidation tracking across container mutations.
- `clang-analyzer-alpha.cplusplus.IteratorRange`: Iterator out-of-range checks.
- `clang-analyzer-alpha.cplusplus.MismatchedIterator`: Mismatched container iterator comparisons.
- `clang-analyzer-alpha.security.ReturnPtrRange`: Returning pointers to memory outside valid bounds.
- `clang-analyzer-alpha.unix.cstring.OutOfBounds`: String and buffer out-of-bounds operations.
- `clang-analyzer-alpha.unix.cstring.BufferOverlap`: Overlapping memory buffers in copy functions (e.g. `memcpy`).
- `clang-analyzer-alpha.unix.cstring.UninitializedRead`: Reads from uninitialized C-string buffers.
- `clang-analyzer-alpha.core.PointerArithm`: Pointer arithmetic into invalid memory regions.

### CERT & Bugprone Checkers
- `cert-oop54-cpp`: Self-assignment handling in copy/move assignment operators.
- `cert-mem57-cpp`: Memory alignment errors in allocation.
- `bugprone-unique-ptr-array-mismatch`, `bugprone-shared-ptr-array-mismatch`: Smart pointer array delete mismatch (`delete` vs `delete[]`).
- `bugprone-unhandled-self-assignment`: Self-assignment detection.
- `bugprone-use-after-move`: Use of an object after `std::move`.
- `bugprone-dangling-handle`: Views/handles pointing to temporary or deallocated memory.
- `bugprone-return-const-ref-from-parameter`: Returning const reference to local/temporary parameter.
- `bugprone-implicit-widening-of-multiplication-result`: 32-bit multiplication implicitly widened to 64-bit size (overflow hazard).
- `bugprone-misplaced-widening-cast`: Widening cast applied after calculation rather than before.
- `bugprone-too-small-loop-variable`: Loop index type too small for container bounds.
- `bugprone-signed-char-misuse`: `char` signedness causing sign extension bugs on indexing or comparison.
- `bugprone-sizeof-expression`, `bugprone-sizeof-container`: Misuse of `sizeof` operator on pointers/containers.
- `bugprone-not-null-terminated-result`: Functions that may produce non-null-terminated strings.
- `bugprone-misplaced-pointer-arithmetic-in-alloc`: Pointer offset placed inside allocation calculation.
- `bugprone-undefined-memory-manipulation`: Raw `memset`/`memcpy` on non-trivially-copyable types.
- `bugprone-suspicious-memset-usage`, `bugprone-suspicious-memory-comparison`: Common buffer comparison/clearing errors.
- `bugprone-suspicious-stringview-data-usage`: `std::string_view::data()` used as null-terminated C-string.
- `bugprone-string-constructor`: Invalid string length or null pointer in constructor.
- `bugprone-infinite-loop`: Unbounded loop invariants.
- `bugprone-unchecked-optional-access`: Accessing `std::optional` value without verification.
- `bugprone-pointer-arithmetic-on-polymorphic-object`: Pointer math on polymorphic objects (slicing risk).
- `bugprone-narrowing-conversions`: High-risk truncation conversions.
- `bugprone-unsafe-functions`: Obsolete or unsafe CRT functions.

### Recursion Checker (Separated)
- `misc-no-recursion`: Flags recursive functions.
- **Why separated:** OpenRAR's root `.clang-tidy` explicitly disables `misc-no-recursion`. Running it in isolation allows quick identification of uncontrolled or deeply nested recursion (which uncovered the `nfc()` recursion stack overflow in `unicode_nfc.cpp`). It completes across the full codebase in 1–2 minutes.

---

## 2. Critical Clang-Tidy Flags

### `-*` Reset Prefix
Clang-tidy merges command-line `--checks` onto the repository's `.clang-tidy` file. Starting the check argument with `-*` disables all repo defaults and ensures only the specified checks run.

### Alpha Checker Enablement
- `--allow-enabling-analyzer-alpha-checkers`: Permits running the `clang-analyzer-alpha.*` family.

### LLVM 22 `-Xclang` Analyzer Option Workaround
```powershell
--extra-arg=-Xclang --extra-arg=-analyzer-config `
--extra-arg=-Xclang --extra-arg=aggressive-binary-operation-simplification=true
```
- In LLVM 22.1.3 (Visual Studio 18), alpha iterator checks fail with:
  `checker cannot be enabled with analyzer option 'aggressive-binary-operation-simplification' == false`
- Setting `CheckOptions` in `--config` does not pass this configuration into the analyzer in LLVM 22. It must be explicitly injected via `-Xclang -analyzer-config` compiler frontend arguments.

### Header Filter & Deduplication
- `--header-filter=.*[\\/]src[\\/].*`: Ensures warnings occurring in headers within `src/` are reported during analysis of translation units.
- Because a header may be `#include`d by dozens of translation units, the same diagnostic will repeat in each unit's log. The CSV export step deduplicates these findings across all translation units.

---

## 3. MSVC `/analyze` Configuration

- **Ruleset:** `leaks_overflows.ruleset`
- **Plugin:** `EspXEngine.dll` (MSVC static analysis engine plugin)
- **Flag Breakdown:**
  - `/analyze:only`: Skips object code generation for speed.
  - `/analyze:external-`: Skips analyzing external headers.
  - `/external:anglebrackets /external:W0`: Suppresses third-party/system header noise.
  - `/analyze:ruleset "<path>"`: Applies targeted leak/overflow ruleset.

---

## 4. Concurrency & Performance Profile

On modern multi-core machines (4-worker parallel execution):
- **Full Clang-Tidy Run:** ~10 minutes total.
- **Slow Translation Units:**
  - `src/cli/main.cpp`: ~200 seconds (heavy symbol resolution and complex CLI flow).
  - `src/dll/dll_api.cpp`: ~155 seconds (wide API surface and foreign transcoding paths).
- **Fast Passes:**
  - Recursion check (`misc-no-recursion`): ~1–2 minutes for all 69 files.
  - Extra files (`worker_main.cpp`, `archive_api.cpp`): ~30 seconds.
