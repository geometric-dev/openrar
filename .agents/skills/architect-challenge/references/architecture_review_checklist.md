# Deep-Dive Multi-Stack Architecture Review Checklist

This checklist provides exhaustive interrogations across systems programming, web/TypeScript runtimes, and Flutter/mobile architectures.

---

## 1. Systems & Low-Level (C, C++, Rust, Go)
- [ ] **Allocation in Hot Loops:** Are buffers allocated repeatedly in inner loops? Can reusable buffers or stack allocation avoid heap overhead?
- [ ] **Data Locality & Cache Lines:** Are structures aligned to 64-byte cache lines to prevent false sharing in multi-threaded contexts?
- [ ] **Syscall Density:** Are filesystem operations batched (e.g. buffered I/O chunks of 64 KiB / 1 MiB) rather than issuing high-frequency byte-by-byte syscalls?
- [ ] **Resource Lifecycle (RAII):** Are all descriptors, handles, sockets, and memory blocks bound strictly to RAII scopes with no leak vectors on early returns or exceptions?
- [ ] **ABI Freeze & Interop:** Are public C/C++ ABI structs untouched unless an ABI major bump is planned? Do exceptions cross `extern "C"` boundaries?

---

## 2. Web & TypeScript Architectures (Node, Deno, Web)
- [ ] **Event Loop Non-Blocking:** Are heavy CPU calculations (crypto, image/data crunching, large JSON parsing) partitioned or delegated to worker threads?
- [ ] **Async Concurrency vs Sequence:** Are independent async operations executed with `Promise.all()` / `Promise.allSettled()` instead of serial `await` chains?
- [ ] **Error Propagation & Unhandled Rejections:** Are all promises guarded with rejection handling? Are errors typed or normalized rather than swallowing `any`?
- [ ] **Memory Leaks in Closures & Event Emitters:** Are event listeners, websocket connections, and intervals explicitly detached when components or sessions unmount?
- [ ] **Bundle & Dependency Bloat:** Does the architecture introduce heavy external dependencies for trivial utilities? Is tree-shaking preserved?

---

## 3. Flutter & Dart Architectures (Mobile, Desktop, Web)
- [ ] **Frame Budget & Jank (16ms Rule):** Is any long-running computation executed on the main UI thread? Is heavy parsing/decoding offloaded to `compute()` / background isolates?
- [ ] **Rebuild Scope Minimization:** Are widget trees rebuilt unnecessarily? Are `const` constructors used wherever possible? Are state selectors (e.g. `ref.watch(provider.select(...))`) scoped to leaves?
- [ ] **Async Gap & Context Safety:** Are `BuildContext` accesses guarded by `if (!mounted) return` after any `await` point?
- [ ] **Resource Disposal:** Are all `StreamSubscription`, `TextEditingController`, `AnimationController`, `ScrollController`, and `FocusNode` instances disposed of in `State.dispose()`?
- [ ] **Platform Channel Serialization:** Are platform channel invocations batched or minimized to prevent binary bridge serialization bottlenecks?

---

## 4. Concurrency, Async & Synchronization (All Stacks)
- [ ] **Lock Granularity & Contention:** Are locks held during expensive I/O, network, or computation?
- [ ] **Lock Inversion & Deadlocks:** Is there a strict, total acquisition order across all mutexes or resources?
- [ ] **Race Conditions (TOCTOU):** Is there a gap between checking a condition (existence, permission, state) and using the resource?
- [ ] **Thread & Task Starvation:** Can tasks submitted to a pool or async queue block indefinitely waiting on subtasks in the same pool?

---

## 5. Storage, Filesystem & Network Resilience (All Stacks)
- [ ] **Durable Writes & Atomicity:** Are file updates written to a temporary sibling file and atomically renamed to prevent corruption on crash?
- [ ] **Path Traversal & Sanitization:** Are all user-supplied paths verified against directory traversal (`../`), null bytes, and platform-specific reserved names (e.g. Windows DOS devices)?
- [ ] **Network Fallbacks & Idempotency:** Are network requests equipped with timeouts, exponential backoff, and idempotent retry keys?
- [ ] **Offline & Disconnected Modes:** How does the design behave when storage is full, network drops, or permissions are revoked?

---

## 6. Verification & Testability (All Stacks)
- [ ] **Negative & Boundary Scenarios:** Are tests asserting 0-length inputs, maximum boundary values, malformed inputs, and abrupt EOF?
- [ ] **Fault Injection:** Are disk errors, network timeouts, and permission denials tested via mocks or test harnesses?
- [ ] **Automated Receipts:** Does the verification plan specify deterministic commands and expected outputs across all target environments?
