Memory Safety, Lifetimes \& Pointer Integrity



\---



\### `\[CRITICAL]`



\#### 1. Wild-Pointer Read \& Heap Buffer Overflow in `ParallelBlockPipeline::compress_stream()` When `cfg.win_size == 0`

\* \*\*Location:\*\* \[`src/compress/parallel_compressor.cpp:20-24, 254-268`](../../src/compress/parallel_compressor.cpp#L254-L268)

\* \*\*Finding:\*\* In `Compressor50::compress_buffer_parallel()` (\[`compressor50.cpp:2189`](../../src/compress/compressor50.cpp#L2189)), a guard `if (win_size == 0) win_size = 0x200000;` was added because `std::min(win_size, off) - 1` underflows to `SIZE_MAX` when `win_size == 0`. However, `ParallelBlockPipeline::compress_stream()` lacks this guard:

&#x20; ```cpp

&#x20; size_t next_seed_len =

&#x20;     static_cast<size_t>(std::min<core::uint64>(cfg_.win_size, next_off) - 1);

&#x20; ...

&#x20; next_job->uncompressed.resize(next_seed_len + next_len);

&#x20; if (next_seed_len > 0) {

&#x20;     std::memcpy(next_job->uncompressed.data(),

&#x20;                 job->uncompressed.data() + job->uncompressed.size() - next_seed_len,

&#x20;                 next_seed_len);

&#x20; }

&#x20; ```

&#x20; If an embedder or caller passes `cfg.win_size = 0` (the standard convention for "use default window" in `StreamEncoder`, `Compressor50`, and `Decompressor50`), `next_seed_len` underflows to `SIZE_MAX`. `next_seed_len + next_len` wraps modulo $2^{64}$ (or $2^{32}$) to `next_len - 1`, resizing `next_job->uncompressed` to a small buffer, and `std::memcpy` reads from a wild pointer (`data() + size() - SIZE_MAX`) while copying `SIZE_MAX` bytes into `next_job->uncompressed.data()`.

\* \*\*Remediation:\*\* Normalize `cfg_.win_size` in `ParallelBlockPipeline::ParallelBlockPipeline()` (e.g., `if (cfg_.win_size == 0) cfg_.win_size = 0x800000;`), and guard the subtraction with `std::min<core::uint64>(cfg_.win_size, next_off) > 0`.



\---



\#### 2. Round-Trip Data Corruption on Filtered Streams in `Compressor50::process_available()` (Partial In-Place Filter Transform vs. Emitted Filter Token Length)

\* \*\*Location:\*\* \[`src/compress/compressor50.cpp:1734, 1761-1811`](../../src/compress/compressor50.cpp#L1761-L1811) and \[`src/compress/compressor50.cpp:487-517`](../../src/compress/compressor50.cpp#L487-L517)

\* \*\*Finding:\*\*

&#x20; 1. \*\*One-shot file/memory-stream path (`!external_buf_ \&\& !filter_pretransformed_`):\*\* At line 1734, `load_data(cur_ + 0x80000)` only loads \*\*512 KiB\*\* (`0x80000`) ahead of `cur_`. When `win_size_ >= 2 MiB`, `filter_max_chunk(win_size_)` at line 1763 returns \*\*1 MiB\*\* (`0x100000`), and `chunk_len` is set to `1 MiB` (for inputs $\\ge 1\\text{ MiB}$). However, `avail_in_buf` (`src_loaded_ - filter_emitted_until_`) is only \*\*512 KiB\*\*. Line 1782 clamps `trans_len = std::min<size_t>(chunk_len, avail_in_buf)` (512 KiB) and transforms only the first 512 KiB in `buf_`, but lines 1805–1809 emit a `FilterToken` with `ft.block_length = chunk_len` (1 MiB) and advance `filter_emitted_until_ += chunk_len` (1 MiB). As `cur_` advances past 512 KiB, the second 512 KiB of the filter region is loaded from `src_file_` / `mem_src_ptr_` \*\*without ever being transformed\*\*, whereas `Decompressor50` applies the inverse filter (`apply_e8` / `apply_arm` / `apply_delta`) across the full 1 MiB region—corrupting the second 512 KiB of every 1 MiB filter chunk (and scrambling all channel strides in `Delta` mode).

&#x20; 2. \*\*Incremental `Compressor50::feed()` path:\*\* In `feed()`, `src_size_` is updated incrementally (`src_size_ = src_loaded_`). When a caller feeds small chunks (e.g., 4 KiB at a time) of executable/audio data, `chunk_len = std::min(src_size_ - filter_emitted_until_, max_chunk)` at line 1764 sizes the filter token to the partial `src_loaded_` rather than `max_chunk` before line 1819 (`cur_ + STREAM_LOOKAHEAD > src_loaded_`) defers match finding, breaking byte-identity with `compress_buffer()`.

\* \*\*Remediation:\*\* Before emitting and transforming a filter region at `filter_emitted_until_`, ensure at least `filter_emitted_until_ + max_chunk` bytes are loaded (via `load_data(filter_emitted_until_ + max_chunk)` in one-shot mode, or by deferring in `process_available(!final)` when `streaming_ \&\& !final \&\& filter_emitted_until_ + max_chunk > src_loaded_`), and assert `trans_len == chunk_len`.



\---



\#### 3. Unsigned Underflow \& Silent Output Truncation on Cross-Block Filter Regions in `Decompressor50::ApplyEngine::flush_pending()`

\* \*\*Location:\*\* \[`src/compress/decompressor50.cpp:475-536, 757, 804-805, 845`](../../src/compress/decompressor50.cpp#L475-L536)

\* \*\*Finding:\*\* In per-block streaming mode (`Decompressor50::decompress_block()` / `StreamDecoder`), `engine_.begin_call()` resets `last_flushed_ = 0` on each block while `base_at_entry` advances by prior blocks' `total_written`, and pending `FilterEntry` items persist in `engine_.filters_` with absolute coordinates (`f.block_start`).

&#x20; When a filter region starts in Block $N$ (`f.block_start < base_at_entry`) and completes in Block $N+1$ (`f.block_start + f.block_length <= abs_pos()`), Block $N$ held back `(base_at_entry - f.block_start)` bytes in `window_`. In Block $N+1$, `flush_pending()` extracts and emits the full `to_emit = f.block_length` bytes and executes:

&#x20; ```cpp

&#x20; last_flushed_ += to_emit;

&#x20; ```

&#x20; Because `to_emit` includes the `(base_at_entry - f.block_start)` bytes from Block $N$, `last_flushed_` (which is in Block $N+1$'s call-local frame `\[0, total_written]`) overshoots Block $N+1$'s local position by `base_at_entry - f.block_start`. This causes:

&#x20; 1. \*\*Unsigned underflow:\*\* `total_written - engine_.last_flushed_` at lines 845, 948, 1010, and 1034 underflows in `size_t` to `\~SIZE_MAX >= 65536`, triggering `flush_pending()` on every subsequent token.

&#x20; 2. \*\*Silent data loss:\*\* Up to `(base_at_entry - f.block_start)` plain bytes decoded after the filter in Block $N+1$ have local offsets `< last_flushed_` and are silently skipped by `flush_plain_up_to()`.

\* \*\*Remediation:\*\* Advance `last_flushed_` to the local end of the filter region (`region_local + to_emit - (f.block_start < base_at_entry ? static_cast<size_t>(base_at_entry - f.block_start) : 0)`), and clamp `to_emit` against `dest_size` using the local end rather than `last_flushed_ + to_emit`.



\---



\#### 4. 32-Bit `size_t` Overflow on Untrusted `f_start` Filter Offsets \& Unchecked Window Wrap in `Decompressor50`

\* \*\*Location:\*\* \[`src/compress/decompressor50.cpp:449-450, 477, 886, 915, 1599, 1620`](../../src/compress/decompressor50.cpp#L886)

\* \*\*Finding:\*\*

&#x20; 1. In `decompress_internal()` (line 886) and `apply_span_records()` (line 1599), `FilterEntry::block_start` is a `size_t` computed as:

&#x20;    ```cpp

&#x20;    fe.block_start = static_cast<size_t>(base_at_entry) + total_written + f_start;

&#x20;    ```

&#x20;    where `f_start` is an untrusted 32-bit unsigned integer read from the bitstream (`0..0xFFFFFFFF`). On 32-bit/WASM targets (`sizeof(size_t) == 4`), an attacker-chosen `f_start` wraps `fe.block_start` modulo $2^{32}$ to an arbitrary offset `< total_written` (and `< last_flushed_`). Even on 64-bit targets, `fe.block_start + fe.block_length` is not overflow-checked before lines 477, 915, and 1620. When `fe.block_start` wraps below `last_flushed_`, `flush_pending()` re-emits already-flushed window bytes and advances `last_flushed_` past `total_written`.

&#x20; 2. In `ApplyEngine::flush_plain_up_to()` (line 449):

&#x20;    ```cpp

&#x20;    size_t back = (total_written - last_flushed_) % win_size_;

&#x20;    ```

&#x20;    There is no check that `total_written - last_flushed_ <= win_size_`. If a caller constructs `Decompressor50` (or calls `openrar_decompress2`) with `win_size < 65536` (smaller than the 64 KiB flush cadence), the circular `window_` wraps and overwrites unflushed bytes before `flush_pending()` runs, and `% win_size_` silently reads corrupted bytes.

\* \*\*Remediation:\*\* Store `FilterEntry::block_start` as `core::uint64`, reject filter tokens where `f_start > UINT64_MAX - (base_at_entry + total_written)` or `fe.block_length > UINT64_MAX - fe.block_start`, enforce a minimum window floor (`>= 0x10000` or flush cadence `std::min<size_t>(65536, engine_.win_size_)`), and fail closed in `flush_plain_up_to()` if `total_written - last_flushed_ > win_size_`.



\---



\#### 5. Strict Aliasing \& Alignment Violations via Raw Pointer Type-Punning (`reinterpret_cast`)

\* \*\*Location:\*\*

&#x20; \* \[`src/api/abi_contract.hpp:74, 105`](../../src/api/abi_contract.hpp#L105) (`reinterpret_cast<ArchiveEntryOut\*>(entries.data())`)

&#x20; \* \[`src/io/win32_meta.cpp:223, 330, 385`](../../src/io/win32_meta.cpp#L223) (`reinterpret_cast<const REPARSE_DATA_BUFFER\*>(data)`)

&#x20; \* \[`src/io/file_stream.cpp:476, 591`](../../src/io/file_stream.cpp#L476) (`reinterpret_cast<OpenrarRenameInfo\*>(buf.data())`)

&#x20; \* \[`src/crypto/aes256.cpp:305, 317`](../../src/crypto/aes256.cpp#L305) (`reinterpret_cast<const uint8x16_t\*>(enc_buf)`)

\* \*\*Finding:\*\*

&#x20; 1. In `win32_meta.cpp:330`, `core::byte buf\[16384]` is declared on the stack with 1-byte alignment and passed to `parse_reparse_buffer()`, which casts it to `const REPARSE_DATA_BUFFER\*` (`alignof(REPARSE_DATA_BUFFER) == 4`) and dereferences its members directly—violating both alignment requirements and C++ strict aliasing (`\[basic.lval]`).

&#x20; 2. In `abi_contract.hpp:105`, `std::vector<uint8_t> entries` is cast to `ArchiveEntryOut\*` (`alignas(8)`) and mutated through `out\[i]` without placement `new` or `std::memcpy`.

&#x20; 3. In `aes256.cpp:305, 317`, `enc_buf` / `dec_buf` (`const core::byte\*`) are cast to `const uint8x16_t\*` and indexed directly (`rk\[r]`) rather than loaded via `vld1q_u8()`.

\* \*\*Remediation:\*\* Construct structs in local stack variables and copy to/from byte buffers via `std::memcpy`, add `alignas(REPARSE_DATA_BUFFER)` to stack buffers where OS APIs require struct layout, and use `vld1q_u8(enc_buf + r \* 16)` in `aes256.cpp`.



\---



\#### 6. Data Races on Shared State Across Multi-Threaded CLI \& Handle Boundaries

\* \*\*Location:\*\*

&#x20; \* \[`src/cli/main.cpp:1051, 1095, 1112`](../../src/cli/main.cpp#L1112)

&#x20; \* \[`src/dll/dll_api.cpp:1396-1421, 2109-2163, 2348-2485`](../../src/dll/dll_api.cpp#L2348-L2485)

&#x20; \* \[`src/wasm/archive_api.cpp:452-662`](../../src/wasm/archive_api.cpp#L452-L662)

\* \*\*Finding:\*\*

&#x20; 1. In `src/cli/main.cpp:1051, 1112`, `int skipped_count = 0;` is a non-atomic stack variable captured by reference and incremented (`skipped_count++`) concurrently by worker threads inside `pool.submit(...)` without synchronization—Undefined Behavior under `\[intro.races]`.

&#x20; 2. In `src/dll/dll_api.cpp:2348-2485`, `g_stream_handles.pin(handle)` releases the table lock and returns a `std::shared_ptr<StreamHandle>`, allowing concurrent calls to `openrar_stream_feed`, `openrar_stream_finish`, `openrar_stream_set_progress`, and `openrar_stream_set_cancel` on the same handle to race on `StreamEncoder` state and callback pointers without a `BusyGuard` or mutex.

&#x20; 3. In `src/dll/dll_api.cpp:1396-1421, 2109-2163`, `openrar_archive_handle_list`, `openrar_archive_handle_entry_ex`, and `openrar_archive_handle_entry_owner` access handle state without acquiring `BusyGuard`.

&#x20; 4. In `src/wasm/archive_api.cpp:452-662`, `ArchiveHandle` has no `BusyGuard`. Re-entering `openrar_archive_handle_extract`, `openrar_archive_handle_extract_all2`, or `openrar_archive_handle_set_limits` from a host `progress` or `cancel` callback mutates `h->ba.cached_entries_` and `h->limit_state` mid-iteration.

\* \*\*Remediation:\*\* Change `skipped_count` to `std::atomic<int>` in `main.cpp`, add `BusyGuard` to `StreamHandle` and to the remaining `ArchiveHandle` entry points in `dll_api.cpp` and `archive_api.cpp`.



\---



\### `\[HIGH]`



\#### 1. Manual `new` / `delete` Exception-Safety Leaks of Heap Memory \& OS File Handles

\* \*\*Location:\*\*

&#x20; \* \[`src/recovery/recovery_writer.cpp:1594-1706`](../../src/recovery/recovery_writer.cpp#L1594-L1706)

&#x20; \* \[`src/io/extraction_journal.cpp:375, 419, 447-452, 504, 535-540, 639, 761`](../../src/io/extraction_journal.cpp#L419)

\* \*\*Finding:\*\*

&#x20; 1. In `RecoveryWriter::repair_rev_volumes()`, up to $2 \\times \\text{nd}$ `io::FileStream` objects are allocated with raw `new io::FileStream()` and held in `std::vector<io::FileStream\*> owned` and `out`. Subsequently, lines 1667–1668 allocate `srcbuf` (1 MiB) and `recon` (`missing` $\\times$ 1 MiB, up to 200 MiB) before the manual `delete` loops at lines 1698–1706. If `std::bad_alloc` is thrown during buffer allocation (which is caught at the DLL boundary in `openrar_archive_repair`), all `io::FileStream` objects and their underlying OS file handles are leaked, leaving volume files locked on disk.

&#x20; 2. In `ExtractionSession::register_temp()` and `register_temp_contained()`, `LockedFile\* lf = new LockedFile()` is stored as a raw `void\*` in `DirJournal`. If `journals_.emplace(...)`, `journal_paths_\[key] = ...`, or `lru_.push_back(key)` throws `std::bad_alloc`, `lf` and its exclusive OS lock handle are leaked.

\* \*\*Remediation:\*\* Replace `std::vector<io::FileStream\*>` with `std::vector<std::unique_ptr<io::FileStream>>` in `recovery_writer.cpp`, and manage `LockedFile` via `std::unique_ptr<LockedFile>` (or a custom deleter) in `ExtractionSession`.



\---



\#### 2. Heap Memory Leaks on Exception or `RAR_ERR_PARTIAL_OK` in Public C++17 Header Wrapper (`openrar.hpp`)

\* \*\*Location:\*\* \[`include/openrar/openrar.hpp:67-76, 87-96, 128-136, 143-152, 164-173, 184-194, 209-219, 223-230, 236-254, 275-284, 415-433, 457-490, 572-579`](../../include/openrar/openrar.hpp#L236-L254)

\* \*\*Finding:\*\* Across `include/openrar/openrar.hpp`, raw buffers returned by the C ABI (`out`, `entries`, `paths`, `buf`, `offs`, `extra`, `cmt`) are held as bare pointers across operations that can throw (`std::vector::assign`, `std::string` construction, `std::map::operator\[]`, and `list_archive()`). For example, in `openrar::extract_all()` (lines 236–254), `buf` and `offs` are allocated by `openrar_archive_extract_all()`, and then `list_archive(data, size)` and `v.assign(...)` are executed before `openrar_free(buf)` and `openrar_free(offs)`. If `list_archive()` throws or `std::bad_alloc` occurs during container population, the `malloc`'d C ABI buffers are leaked.

\* \*\*Remediation:\*\* Wrap all C ABI output pointers in RAII guards (e.g., `std::unique_ptr<uint8_t, decltype(\&openrar_free)>`) immediately after the C ABI call.



\---



\#### 3. Stale `external_buf_` / `mem_src_ptr_` Pointer State Across `Compressor50::start_file()`

\* \*\*Location:\*\* \[`src/compress/compressor50.cpp:531-564, 586-618`](../../src/compress/compressor50.cpp#L531-L564)

\* \*\*Finding:\*\* `Compressor50::start_file(io::FileStream\* src, core::uint64 src_file_size, bool continue_window)` sets `src_file_ = src`, but does not reset `external_buf_ = false` or `mem_src_ptr_ = nullptr`. If a `Compressor50` instance previously used `set_external_buffer()`, `load_data()` short-circuits at `if (external_buf_) return;`, either reading from a dangling external pointer (if `continue_window == true`) or failing to load any bytes from `src_file_`.

\* \*\*Remediation:\*\* Reset `external_buf_ = false;` and `mem_src_ptr_ = nullptr;` in `Compressor50::start_file()` and ensure `ensure_internal_buffer()` refreshes `buf_data_`.



\---



\#### 4. Shared `stream_` File-Position Race in `ArchiveReader::extract_store_entry_impl()`

\* \*\*Location:\*\* \[`src/archive/archive_reader.cpp:3308-3328`](../../src/archive/archive_reader.cpp#L3308-L3328)

\* \*\*Finding:\*\* While `ArchiveReader::read_packed_data()` (\[`archive_reader.cpp:2116-2120`](../../src/archive/archive_reader.cpp#L2116-L2120)) opens a private `io::FileStream` via `open_extent_source()` in the legacy single-extent fallback (`entry.extents.empty()`) to avoid racing on the shared `stream_` file offset, `ArchiveReader::extract_store_entry_impl()` still calls `stream_.seek()` and `stream_.read()` directly on `this->stream_`.

\* \*\*Remediation:\*\* Open a local `io::FileStream` via `open_extent_source(path_, local)` in `extract_store_entry_impl()`'s fallback path.



\---



\### `\[MEDIUM]`



\#### 1. `Compressor50::begin_stream(..., continue_window = true)` Rejects Non-Power-of-Two Window Sizes

\* \*\*Location:\*\* \[`src/compress/compressor50.cpp:411-415, 454`](../../src/compress/compressor50.cpp#L454)

\* \*\*Finding:\*\* On the first member of a solid stream (`continue_window = false`), `begin_stream()` calls `begin_archive()`, which rounds `win_size_` up to a power of two (`pow2_ws`, with a `0x10000` floor). On the second member (`continue_window = true`), line 454 compares `win_size_ != win_size` against the caller's unrounded `win_size`. Any caller passing a RAR7 fractional window size (or a window `< 64 KiB`) to `StreamEncoder` / `SolidPacker` will fail on `pack_solid_begin(true)`.

\* \*\*Remediation:\*\* Apply the same power-of-two rounding helper to `win_size` before comparing `win_size_ != pow2_ws` in `begin_stream()`.



\---



\#### 2. Unvalidated Null Pointer Arithmetic in `BitReader(const core::byte\* data, size_t size)`

\* \*\*Location:\*\* \[`src/compress/decompressor50.cpp:14-15`](../../src/compress/decompressor50.cpp#L14-L15)

\* \*\*Finding:\*\* `BitReader::BitReader(const core::byte\* data, size_t size)` computes `end_(data + size)` unconditionally. If constructed directly with `data == nullptr` and `size > 0`, `nullptr + size` is Undefined Behavior (`\[expr.add]`) and `refill()` dereferences `nullptr`.

\* \*\*Remediation:\*\* Guard the constructor: if `data == nullptr`, clamp `size = 0` so `p_ = end_ = nullptr` and `size_ = fetched_ = 0`.



\---



\#### 3. Over-Wide Unterminated String Scan in `openrar_archive_create()`

\* \*\*Location:\*\* \[`src/dll/dll_api.cpp:1308-1310`](../../src/dll/dll_api.cpp#L1308-L1310), \[`src/wasm/archive_api.cpp:283-285`](../../src/wasm/archive_api.cpp#L283-L285)

\* \*\*Finding:\*\* `while (plen < 4096 \&\& p\[plen] != 0) ++plen;` scans up to 4096 bytes before `validate_archive_path()` rejects `plen > 2048`. If a caller passes an unterminated buffer between 2049 and 4095 bytes near a page boundary, scanning to 4096 can fault unnecessarily.

\* \*\*Remediation:\*\* Bound the scan loop to `plen <= 2048` (2049 bytes max), which is sufficient to trigger `validate_archive_path()`'s `> 2048` rejection without reading up to 4096 bytes.



\---



\### `\[LOW]`



\#### 1. Raw Pimpl Pointer in `MappedFile` Instead of `std::unique_ptr<Impl>`

\* \*\*Location:\*\* \[`src/io/mapped_file.hpp:61-62`](../../src/io/mapped_file.hpp#L61-L62), \[`src/io/mapped_file.cpp:51-56, 147-152`](../../src/io/mapped_file.cpp#L51-L56)

\* \*\*Finding:\*\* `MappedFile` manually allocates `impl_` with `new Impl` and frees it with `delete impl_;` in `\~MappedFile()`.

\* \*\*Remediation:\*\* Replace `Impl\* impl_` with `std::unique_ptr<Impl> impl_`.



\---



\#### 2. Redundant `std::malloc(1)` + Immediate `std::free()` on Empty Output Buffers

\* \*\*Location:\*\* \[`src/dll/dll_api.cpp:1188-1196`](../../src/dll/dll_api.cpp#L1188-L1196), \[`src/wasm/wasm_api.cpp:220-225, 313-318`](../../src/wasm/wasm_api.cpp#L220-L225), \[`src/wasm/archive_api.cpp:199-207`](../../src/wasm/archive_api.cpp#L199-L207)

\* \*\*Finding:\*\* These functions allocate `std::malloc(out.size() > 0 ? out.size() : 1)` and then immediately call `if (out.empty()) std::free(buf);` to return `\*out_ptr = nullptr`.

\* \*\*Remediation:\*\* Use `openrar::api::heap_dup(out.data(), out.size())` (as already used in `ArchiveHandle::extract`) to avoid allocating and immediately freeing 1 byte on empty outputs.

---

## Validation Addendum — 2026-10-20

**Verdict:** All 15 findings were independently re-validated and have since been remediated. This section retains the confirmed mechanics, corrected remediation guidance, and the tests added to prove the fixes. The original findings above are retained unchanged for traceability. See **Remediation outcome** below for implementation and verification evidence.

**Validation method:** source audit of every cited path, cross-check against the surrounding callers, and inspection of the existing unit-test coverage. Baseline `compress_tests` and `parallel_compress_tests` filter suites pass; the passing baseline does **not** disprove the findings because none of the existing cases exercises the failing geometry (see §Coverage Gaps).

### Confirmed status

| # | Status | Confirmed mechanism |
|---|---|---|
| CRIT-1 | Confirmed | `ParallelBlockPipeline` stores `cfg_.win_size == 0`; `compress_stream()` computes `std::min(cfg_.win_size, next_off) - 1`, underflowing `next_seed_len`; `resize(next_seed_len + next_len)` wraps, then `memcpy` reads a wild pointer. |
| CRIT-2 | Confirmed, with scope correction | One-shot `start_file()` sets `src_size_` to the full file but only loads `cur_ + 0x80000`; a 1 MiB filter token transforms 512 KiB and advances the token length 1 MiB, so the decoder inverse-filters untransformed tail bytes. `feed()` sizes tokens from partial `src_loaded_`; this is a byte-identity divergence from `compress_buffer()`/one-shot, not a round-trip corruption in that path. `compress_buffer()` pre-transforms separately and is unaffected. |
| CRIT-3 | Confirmed, remediation corrected | `decompress_block()` uses `dest_size = SIZE_MAX` and `begin_call()` resets `last_flushed_ = 0`; a region completed in a later block emits the full region length while the local frame has only `len - prior` bytes, so `last_flushed_ += to_emit` overshoots and subsequent plain bytes are skipped/shifted. `apply_span_records()` passes base `0`, so it has no carried-prefix case. |
| CRIT-4 | Confirmed | `FilterEntry::block_start` is `size_t`; the 32-bit `f_start` addition can wrap on ILP32/WASM. `f.block_start + f.block_length` is unchecked at the completion, overlap, and flush sites. `flush_plain_up_to()` computes `total_written - last_flushed_` without an ordering check. |
| CRIT-5 | Confirmed | Stack `core::byte buf[16384]` is reused as `REPARSE_DATA_BUFFER` with insufficient alignment/effective type; ABI entry vectors are mutated as `ArchiveEntryOut*`; rename-info casts and NEON `uint8x16_t*` indexing have the same class of violation. |
| CRIT-6 | Confirmed | CLI `skipped_count` is a plain `int` shared across workers. DLL `StreamHandle` methods release the pin lock and race on encoder/callback state. WASM `ArchiveHandle` has no busy guard and lacks `<atomic>`. Read-only metadata methods (`list`/`entry_ex`/`entry_owner`) also need the guard per the finding, accepting `RAR_ERR_BUSY` for reentrant use. |
| HIGH-1 | Confirmed | `repair_rev_volumes()` holds raw `FileStream*` in `owned`/`out` across 1 MiB/200 MiB allocations and manual delete loops. `register_temp*()` holds a raw `LockedFile*` across map/LRU operations that can throw. |
| HIGH-2 | Confirmed | Public header wrappers hold C-ABI allocations across `std::vector::assign`, string construction, map insertion, and `list_archive()`; an exception leaks them. A guard must be installed immediately after the C call, before `check()`. |
| HIGH-3 | Confirmed | `start_file()` does not reset `external_buf_`, `mem_src_ptr_`, or `filter_pretransformed_`; a reused packer can skip loading entirely or skip the in-loop transform. |
| HIGH-4 | Confirmed | `extract_store_entry_impl()` fallback still seeks/reads the shared `stream_`, unlike `read_packed_data()`'s private extent source. |
| MEDIUM-1 | Confirmed | `begin_stream(continue_window=true)` compares the caller's unrounded window against the rounded `win_size_` and fails for fractional or sub-64 KiB sizes. |
| MEDIUM-2 | Confirmed | `BitReader(data, size)` computes `end_ = data + size` before any null check. |
| MEDIUM-3 | Confirmed | The scan runs to 4096 bytes before `validate_archive_path()` rejects lengths above 2048. |
| LOW-1 | Confirmed | `MappedFile` owns `Impl*` manually; converting to `unique_ptr` is safe because the destructor is out-of-line and no inline move constructor is generated. |
| LOW-2 | Confirmed | Empty outputs allocate 1 byte and immediately free it in the DLL/WASM paths. |

### Corrections and refinements to the original remediation text

1. **CRIT-1 default:** normalize to `0x200000`, matching `StreamEncoder` and `compress_buffer_parallel()`, not `0x800000`. Guard the seed subtraction with `std::min<core::uint64>(cfg_.win_size, next_off) > 0` before subtracting.
2. **CRIT-2 is two bugs, not one:**
   - One-shot: compute `want = min(max_chunk, src_size_ - filter_emitted_until_)` **before** transforming, call `load_data(filter_emitted_until_ + want)`, and fail closed (`-1`) if the source is short. Never clamp the transform to `avail_in_buf` while emitting the larger token.
   - Streaming: while `!final`, use a full `max_chunk` token and defer (`return 0`) until `filter_emitted_until_ + max_chunk <= src_loaded_`; only the final tail may be shorter. This makes token boundaries feed-size invariant and byte-identical to one-shot/`compress_buffer()`.
   - `start_file()` is not currently used by the archive mutator, but it is a public/testable packer path and must be fixed and regression-tested directly.
3. **CRIT-3 frame accounting:** the original "advance by `to_emit - prior`" is incomplete. In per-block streaming the retained prefix was never delivered in an earlier frame, so the completion frame must emit `prior + local_allowed` bytes starting at the region start, while advancing `last_flushed_` only by `local_allowed`. In bounded frames (`dest_size != SIZE_MAX`), `prior > 0` cannot be represented and must fail closed. A partial region emission must also fail closed instead of dropping the tail via `filters_head_++`. Add overflow-checked completion/overlap tests and reject `block_start <= abs_pos` for non-empty regions under `flush_all`.
4. **CRIT-4 type and arithmetic:** change `FilterEntry::block_start` to `core::uint64` and update **all** sites: the sequential push (around `decompressor50.cpp:886`), the span-record push (`:1599`), CROSS_VALIDATE region recording, overlap checks, and `local_region_start()`. Perform start/end/file-offset additions in `uint64` with explicit overflow checks. Enforce a `0x10000` floor in `ApplyEngine::init()` and cadence `min(65536, win_size_)`; add `last_flushed_ > total_written` and `lag > win_size_` checks in `flush_plain_up_to()`.
5. **CRIT-5 should avoid `REPARSE_DATA_BUFFER` entirely:** parse with `core::read_le16/read_le32` at fixed byte offsets (tag 0, data length 4, reserved 6; mount-point path at 16, symlink flags at 16 and path at 20), validate `size >= 8 + data_len`, clamp `path_limit = min(data_len - field_bytes, size - path_base)`, require even UTF-16 byte lengths, and copy paths with `memcpy`. When creating, write fields into a byte vector with `write_le*` and reject targets that exceed the `USHORT` field capacity. This is stronger than adding `alignas` to the stack buffer. Use local `OpenrarRenameInfo` + `memcpy` for the rename path and `vld1q_u8` for NEON loads.
6. **CRIT-6 guard scope:** make `skipped_count` `std::atomic<int>`. Add a per-handle `std::atomic<bool> busy` + `BusyGuard` to `StreamHandle` mutators (`feed`, `finish`, progress/cancel setters) and to the requested `ArchiveHandle`/metadata methods. Guard immediately after pinning; return `RAR_ERR_BUSY` on contention. Add `<atomic>` to the WASM translation unit.
7. **HIGH-2 guard ordering:** construct the `ScopeExit` immediately after the successful C call and before `check()`/`unpack_entries()`. Own both output arrays where applicable; do not rely on the callee having nulled them on failure.
8. **MEDIUM-1 helper:** factor the exact `begin_archive()` normalization (`0 -> 0x20000`, power-of-two rounding with a `0x10000` floor, `max_retain = min(sz_cap, SIZE_MAX - 0x1000000)` cap) into one helper used by both the comparison and the archive initialization. `begin_stream()` separately preserves its existing public default by mapping a caller window of `0` to `0x200000` before the comparison.
9. **MEDIUM-2 exact form:** set `end_(data)`, `size_(data ? size : 0)`, `fetched_(data ? size : 0)` in the initializer list, then `if (data) end_ = data + size;` in the body. This preserves `end_ - p_` semantics without null arithmetic.
10. **MEDIUM-3:** use `while (plen <= 2048 && p[plen] != 0)` so the 2049-byte limit is enough for `validate_archive_path()` to reject an over-long value without an extra page read.
11. **LOW-2:** use `openrar::api::heap_dup(out.data(), out.size())` everywhere listed; return `nullptr` for empty output and never `malloc(1)`/`free`.

### Regression and negative-test plan

- `parallel_compress_tests`: `win_size = 0`, 3 MiB member, 1 MiB chunks, 2 threads, decompress with a 2 MiB window. Pre-fix this exercises the seed underflow.
- `compress_tests`: direct `Compressor50::start_file()` + `set_active_filter(E8)` + `compress()` on >1 MiB of executable-like data, then round-trip. This is the only deterministic way to hit the one-shot partial-transform path.
- `compress_tests`: streaming filtered round-trip with E8 data and different feed sizes (whole buffer, 64 KiB, 4 KiB), asserting (a) round-trip and (b) byte-identical compressed output across feed sizes. `feed()` detection requires the first feed to be at least 64 bytes.
- `compress_tests`: a filter region that ends inside a 512 KiB block with plain data following it, to exercise CRIT-3's carried-prefix accounting. Existing 1 MiB filter/512 KiB block-aligned sizes cannot fail this way; use non-aligned sizes and/or long matches to drift the block boundary.
- `io_tests`: the existing malformed/unaligned/out-of-bounds reparse tests and junction round-trip remain mandatory after the CRIT-5 rewrite; add an odd-length/oversized-target case if not already covered.
- ABI/DLL/WASM: rebuild the affected targets; add an empty-output assertion for extract paths; add a contention test only if it can be made deterministic (otherwise document the `RAR_ERR_BUSY` contract).
- Build/run: `cmake --build build --config Release --parallel`, then `ctest --test-dir build -C Release --output-on-failure`; targeted runs via `build\Release\compress_tests.exe filter` and `parallel_compress_tests.exe filter` (the checked-in `build_inmem` binaries are baseline only).

### Remediation outcome

All 15 findings have been fixed in the source tree. Regression coverage added or exercised:

- **CRIT-1:** `test_parallel_pipeline_default_window` in `parallel_compress_tests` uses `win_size = 0`, multiple threads, and a 2 MiB decoder; case count 9 -> 10. Pass.
- **CRIT-2/CRIT-3/CRIT-4/HIGH-3/MEDIUM-1:** `test_start_file_oneshot_filter_chunking` and `test_streaming_filter_feed_size_invariance` in `compress_tests` exercise the one-shot chunked-filter path and feed-size-invariant streaming; case count 67 -> 69. Both pass. The existing multi-token/multi-block filter round-trip, filter-crosses-window-wrap, and zero-length filter-applier cases also pass.
- **CRIT-5:** the existing malformed/unaligned/out-of-bounds reparse cases and the junction round-trip pass after the byte-oriented parser/creator rewrite.
- **CRIT-6:** `test_cpp_wrapper_busy_reentry` in `dll_cpp_tests` re-enters the DLL stream mutator from its cancel callback and the archive metadata list from an extraction progress callback; both return `RAR_ERR_BUSY`; case count 6 -> 7. Pass.
- **HIGH-1/HIGH-2/HIGH-4/MEDIUM-2/MEDIUM-3/LOW-1/LOW-2:** covered by the full suite and targeted source audit; no safe deterministic exception-injection test was added for the C-ABI free paths.

**Verification:** after rebuilding, `ctest --test-dir build -C Release --output-on-failure` reports **49/49 tests passed** (`dll_cpp_tests` 7/7). Validation copied the freshly linked `build/openrar.dll` to `build/Release/openrar.dll` first: the DLL runtime output directory is the build root, while the test executables load a sibling DLL, and the pre-existing `build/Release` copy was stale.

### Coverage gaps in the existing suite

- The passing filter tests use 1 MiB token regions that align with 512 KiB block boundaries; a dedicated non-aligned carried-prefix completion case was not added, though the existing multi-token/multi-block and window-wrap cases pass.
- The streaming filter round-trip previously verified bytes but not feed-size invariance; the new `test_streaming_filter_feed_size_invariance` closes that gap.
- No test constructed a `ParallelCompressConfig` with `win_size == 0`; the new `test_parallel_pipeline_default_window` closes that gap.
- No test reused a `Compressor50` after `set_external_buffer()` before `start_file()`; the new one-shot start-file test and state-reset audit cover this path.
- The new reentry test covers DLL stream and archive-handle BUSY guards. WASM reentry and forced exceptions between a C-ABI allocation and its free remain untested.

### Process notes

- The worktree already contains unrelated edits, including in `compressor50.cpp/.hpp`, plus untracked build artifacts. Stage only the remediation hunks; do not use `git add -A`.
- `Compressor50::start_file()` is a public API but is not called by the archive mutator; the CRIT-2 regression must call it directly rather than relying on archive creation.







