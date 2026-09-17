# Deep-Dive Architecture Review Checklist

This checklist provides exhaustive interrogations across critical systems programming and software architecture domains.

---

## 1. Memory Management & Allocations
- [ ] **Allocation in Hot Loops:** Are buffers allocated repeatedly in inner loops? Can a reusable `std::vector<uint8_t>` or scratch buffer with `.clear()` / `.reserve()` avoid reallocations?
- [ ] **Small Buffer Optimization (SBO):** Can small strings or small array buffers stay on the stack instead of heap allocation?
- [ ] **Excessive Copies:** Are string or vector parameters passed by `const&` or `std::string_view` where ownership is not required?
- [ ] **Alignment & Cache Lines:** Are structures structured to avoid false sharing across cache lines (64 bytes) in multi-threaded contexts?
- [ ] **OOM Handling:** What happens if an allocation fails on a huge file or corrupted archive size header? Is there an upper sanity cap (e.g. 16 MiB, 2 GiB) before requesting allocations based on untrusted inputs?

---

## 2. Concurrency, Threading & Synchronization
- [ ] **Lock Granularity:** Are mutex locks held during expensive operations (disk I/O, hash calculation, decompression, compression)?
- [ ] **Lock Inversion & Deadlock:** If multiple mutexes exist, is there a strictly defined acquisition order?
- [ ] **Thread Pool Starvation:** Can tasks submitted to a thread pool block indefinitely waiting on other tasks in the same pool?
- [ ] **Race Conditions (TOCTOU):** Is there a gap between checking a filesystem property (e.g. existence, symlink check, permissions) and using the file handle?
- [ ] **Thread Safety of Shared State:** Are lookup caches, deduplication tables, or stats maps accessed concurrently without proper synchronization or partitioning?

---

## 3. Filesystem & OS Syscall Mechanics
- [ ] **Syscall Density:** Can filesystem operations be combined (e.g. `statx`, `fstat` on open handle rather than repeated path traversals)?
- [ ] **Directory Traversal:** Are directory loops handling recursive symlink loops or deep directory trees without stack overflow?
- [ ] **Hardlink & Inode Invariants:**
  - On POSIX, `st_ino` is only unique within the same filesystem (`st_dev`). A composite key of `(st_dev, st_ino)` is strictly required.
  - On Windows, `nFileIndexHigh` and `nFileIndexLow` are only unique within `dwVolumeSerialNumber`. Furthermore, ReFS uses 128-bit file IDs (`FILE_ID_128`), which require `GetFileInformationByHandleEx(FileIdInfo)`.
- [ ] **Path Sanitization & Canonicalization:**
  - Are paths checked for traversal (`../`, absolute paths, Windows drive letters, UNC paths `\\` or `\\?\`)?
  - Are case-insensitive filesystem collisions handled?
  - Are reserved DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`) rejected or safely escaped on Windows?
- [ ] **Durable Writes & Atomicity:**
  - Are temp files created in the same filesystem/volume as the destination file so that rename is atomic?
  - Are permissions, ACLs, and timestamps restored *after* writing contents to avoid write permission denial?

---

## 4. Binary Wire Format & Data Integrity
- [ ] **Varint / VINT Safety:** Is there an upper limit on bytes read for variable-length integers to prevent infinite loops on EOF or maliciously padded bytes?
- [ ] **CRC & Digest Validation:** Is validation performed streaming-style or end-to-end? Does zero CRC mean zero hash or uncomputed?
- [ ] **End-of-File & Truncation:** Does the decoder gracefully fail when a block declares 1000 bytes but only 20 bytes remain in the stream?
- [ ] **Header Flags & Reserved Bits:** Are unknown flags preserved or rejected appropriately per specification?

---

## 5. ABI & Boundary Interop
- [ ] **Frozen Layouts:** Are public C ABI structs untouched unless an ABI major version bump is explicitly planned?
- [ ] **Calling Convention & Packing:** Are all exported symbols declared with consistent calling conventions (`__cdecl`, `__stdcall`) and explicitly packed if required (`#pragma pack`)?
- [ ] **Resource Ownership across DLL/WASM boundaries:** Who frees allocated memory? Never allocate in DLL with one allocator and free in host with another (`malloc`/`free` boundary symmetry).
- [ ] **Exception Containment:** Every C entry point must have a `try { ... } catch (...)` block converting C++ exceptions into standardized integer error codes.
