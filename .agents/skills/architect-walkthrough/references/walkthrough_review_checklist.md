# Deep-Dive Multi-Stack Walkthrough & Implementation Review Checklist

This checklist provides exhaustive criteria for evaluating completed code implementations and their accompanying walkthroughs across C++, TypeScript, Flutter, and multi-stack codebases.

---

## 1. Plan Reconciliation & Deviation Control
- [ ] **Commitment Traceability:** Did every item committed to in the implementation plan get delivered in code?
- [ ] **Deviation Transparency:** Are all deviations from the plan explicitly called out in the walkthrough?
- [ ] **Justification Rigor:** For any deviation, is the engineering justification valid (e.g. discovered OS/platform constraint, benchmark bottleneck, spec edge case)? Or was it a shortcut?
- [ ] **No Silent Scope Reductions:** Were any planned tests, error handling routines, or edge cases silently skipped?
- [ ] **No Scope Creep:** Were opportunistic refactorings or unnecessary dependencies bundled in without prior architectural approval?

---

## 2. Hard Verification & Test Evidencing (Receipts)
- [ ] **Receipts Provided:** Does the walkthrough include concrete test runner invocations and actual console outputs (`ctest`, `npm test`, `jest`, `vitest`, `flutter test`, `cargo test`)?
- [ ] **Edge & Negative Path Coverage:** Did tests specifically exercise invalid inputs, boundary numbers (e.g. `UINT32_MAX`), empty payloads, and abrupt EOF / timeouts?
- [ ] **Fault Injection:** If error handling was added (e.g. disk full, read error, permission denied, HTTP 500), was it actually triggered in a test?
- [ ] **Regression Tests for Bug Fixes:** For any bug fix, is there a deterministic regression test that fails before the fix and passes after?
- [ ] **Stress & Concurrency Validation:** If concurrency or lock changes were made, was ThreadSanitizer, an async stress loop, or parallel load test executed?

---

## 3. Code Quality & Systems Invariants
- [ ] **Resource Lifecycle & Unwinding:**
  - C/C++: Are all descriptors, memory blocks, and mutexes guaranteed to unwind via RAII?
  - TypeScript: Are connections, timers, and workers cleaned up in `finally` blocks?
  - Flutter: Are all controllers, streams, and notifiers disposed of in `dispose()`? Are async gaps protected with `if (!mounted)`?
- [ ] **Bounds & Arithmetic Safety:** Are offsets and indices checked against buffer/list capacity before access? Are additions/multiplications checked against overflow?
- [ ] **Mechanical Sympathy:**
  - Are there new heap allocations inside hot processing loops?
  - In TypeScript, is the event loop blocked by heavy synchronous work?
  - In Flutter, are widgets rebuilding excessively during animations or state emissions?

---

## 4. Platform Robustness & Environmental Hygiene
- [ ] **Cross-Platform Path Handling:** Are path operations portable across Windows (`\`) and POSIX (`/`)? Are paths canonicalized and checked for traversal (`../`)?
- [ ] **Atomicity of File Operations:** Are file writes performed using atomic swap/rename to avoid leaving partial or corrupted output files?
- [ ] **Unprivileged Fallbacks:** Does the code handle environments where elevated privileges or specific device features are unavailable without crashing?

---

## 5. API & Contract Stability
- [ ] **Public Interface Preservation:** Were any public headers, exports, RPC schemas, or database schemas altered in a breaking manner?
- [ ] **Error Reporting Consistency:** Do new functions adhere to the established error reporting idiom (e.g. `Result<T>`, standard exceptions, typed error codes)?

---

## 6. Code Hygiene & Delivery Cleanliness
- [ ] **No Lingering Artifacts:** Are there leftover `TODO`s, temporary print statements (`console.log`, `printf`, `std::cout`, `debugPrint`), commented-out blocks, or temporary scratch files?
- [ ] **Accurate Documentation:** Are updated functions documented with accurate parameter descriptions, return codes, and precondition/postcondition invariants?
