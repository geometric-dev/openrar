# Deep-Dive Walkthrough & Implementation Review Checklist

This checklist provides exhaustive criteria for evaluating completed code implementations and their accompanying walkthroughs.

---

## 1. Plan Reconciliation & Deviation Control
- [ ] **Commitment Traceability:** Did every change committed to in the `implementation_plan.md` get implemented in code?
- [ ] **Deviation Transparency:** Are all deviations from the plan explicitly called out in the walkthrough?
- [ ] **Justification Rigor:** For any deviation, is the engineering justification valid (e.g. discovered hardware/OS constraint, performance bottleneck, spec contradiction)? Or was it a shortcut to avoid hard work?
- [ ] **No Silent Scope Reductions:** Were any planned tests, error handling routines, or edge cases silently skipped or deferred without explicit stakeholder agreement?
- [ ] **No Scope Creep:** Were opportunistic refactorings or unnecessary dependencies bundled in without prior architectural approval?

---

## 2. Hard Verification & Test Evidencing
- [ ] **Receipts Provided:** Does the walkthrough include concrete test commands and actual console outputs, rather than generic statements like "tests passed"?
- [ ] **Edge & Negative Path Coverage:** Did tests specifically exercise invalid inputs, corrupted files, zero-length files, boundary numbers (e.g. `UINT32_MAX`), and premature EOF?
- [ ] **Fault Injection:** If error handling was added (e.g. disk full, read error, permission denied), was it actually triggered in a test (e.g. via mock, read-only temp dir, or fault injection)?
- [ ] **Regression Tests for Bug Fixes:** For any bug fix, is there a deterministic regression test that fails before the fix and passes after?
- [ ] **Stress & Concurrency Validation:** If concurrency or lock changes were made, was ThreadSanitizer or a high-iteration stress loop run to catch race conditions?

---

## 3. Code Quality & Systems Invariants
- [ ] **RAII & Resource Cleanup:** Are all newly allocated memory blocks, file descriptors, and synchronization primitives guaranteed to unwind cleanly on all exit paths (normal return, early error return, or exception)?
- [ ] **Bounds & Arithmetic Safety:**
  - Are buffer offsets and index calculations checked against buffer capacity before access?
  - Are integer additions/multiplications for buffer allocations checked against overflow?
- [ ] **Mechanical Sympathy:**
  - Are there new heap allocations (`new`, `malloc`, `std::vector` reallocations) inside hot processing loops?
  - Are large structs passed by value instead of `const&` or `std::string_view`?
  - Are I/O buffers appropriately sized (e.g. 64 KiB / 1 MiB) rather than issuing high-frequency byte-by-byte syscalls?
- [ ] **Data Locality & False Sharing:** In concurrent code, are thread-local accumulators separated by cache-line padding (64 bytes) to prevent false sharing?

---

## 4. Platform Robustness & OS Realities
- [ ] **Path Handling:** Are path operations portable across Windows (`\`) and POSIX (`/`)? Are paths canonicalized and checked for path traversal (`../`)?
- [ ] **Windows DOS Device Names:** On Windows, are reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`) protected against?
- [ ] **Atomicity of File Operations:** Are file writes performed using atomic swap/rename on the same filesystem/volume to avoid leaving partial or corrupted output files?
- [ ] **Unprivileged Fallbacks:** Does the code handle environments where privileges (e.g. symlinks, hardlinks, specific ACLs) are unavailable without crashing?

---

## 5. ABI, API & Interface Stability
- [ ] **ABI Layouts:** Were any public headers, structs, or class layouts modified in a way that breaks existing binary callers?
- [ ] **Exception Boundaries:** Do any C++ exceptions escape unhandled across `extern "C"` interfaces?
- [ ] **Error Reporting Consistency:** Do new functions adhere to the established error reporting idiom of the module (e.g. `Result<T>`, error codes, exceptions)?

---

## 6. Code Hygiene & Delivery Cleanliness
- [ ] **No Lingering Artifacts:** Are there leftover `TODO`s, temporary print statements (`std::cout`, `printf`), commented-out blocks, or temporary files?
- [ ] **Accurate Documentation:** Are updated functions documented with accurate parameter descriptions, return codes, and precondition/postcondition invariants?
