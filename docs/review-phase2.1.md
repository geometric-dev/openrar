## Phase 2.1: Memory Safety, Lifetimes & Pointer Integrity

---

### `[CRITICAL]`

#### 1. Wild-Pointer Read & Heap Buffer Overflow in `ParallelBlockPipeline::compress_stream()` When `cfg.win_size == 0`
* **Location:** [`src/compress/parallel_compressor.cpp:20-24, 254-268`](file:///c:/Users/Matt/dev/openrar/src/compress/parallel_compressor.cpp#L254-L268)
* **Finding:** In `Compressor50::compress_buffer_parallel()` ([`compressor50.cpp:2189`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L2189)), a guard `if (win_size == 0) win_size = 0x200000;` was added because `std::min(win_size, off) - 1` underflows to `SIZE_MAX` when `win_size == 0`. However, `ParallelBlockPipeline::compress_stream()` lacks this guard:
  ```cpp
  size_t next_seed_len =
      static_cast<size_t>(std::min<core::uint64>(cfg_.win_size, next_off) - 1);
  ...
  next_job->uncompressed.resize(next_seed_len + next_len);
  if (next_seed_len > 0) {
      std::memcpy(next_job->uncompressed.data(),
                  job->uncompressed.data() + job->uncompressed.size() - next_seed_len,
                  next_seed_len);
  }
  ```
  If an embedder or caller passes `cfg.win_size = 0` (the standard convention for "use default window" in `StreamEncoder`, `Compressor50`, and `Decompressor50`), `next_seed_len` underflows to `SIZE_MAX`. `next_seed_len + next_len` wraps modulo $2^{64}$ (or $2^{32}$) to `next_len - 1`, resizing `next_job->uncompressed` to a small buffer, and `std::memcpy` reads from a wild pointer (`data() + size() - SIZE_MAX`) while copying `SIZE_MAX` bytes into `next_job->uncompressed.data()`.
* **Remediation:** Normalize `cfg_.win_size` in `ParallelBlockPipeline::ParallelBlockPipeline()` (e.g., `if (cfg_.win_size == 0) cfg_.win_size = 0x800000;`), and guard the subtraction with `std::min<core::uint64>(cfg_.win_size, next_off) > 0`.

---

#### 2. Round-Trip Data Corruption on Filtered Streams in `Compressor50::process_available()` (Partial In-Place Filter Transform vs. Emitted Filter Token Length)
* **Location:** [`src/compress/compressor50.cpp:1734, 1761-1811`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L1761-L1811) and [`src/compress/compressor50.cpp:487-517`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L487-L517)
* **Finding:**
  1. **One-shot file/memory-stream path (`!external_buf_ && !filter_pretransformed_`):** At line 1734, `load_data(cur_ + 0x80000)` only loads **512 KiB** (`0x80000`) ahead of `cur_`. When `win_size_ >= 2 MiB`, `filter_max_chunk(win_size_)` at line 1763 returns **1 MiB** (`0x100000`), and `chunk_len` is set to `1 MiB` (for inputs $\ge 1\text{ MiB}$). However, `avail_in_buf` (`src_loaded_ - filter_emitted_until_`) is only **512 KiB**. Line 1782 clamps `trans_len = std::min<size_t>(chunk_len, avail_in_buf)` (512 KiB) and transforms only the first 512 KiB in `buf_`, but lines 1805–1809 emit a `FilterToken` with `ft.block_length = chunk_len` (1 MiB) and advance `filter_emitted_until_ += chunk_len` (1 MiB). As `cur_` advances past 512 KiB, the second 512 KiB of the filter region is loaded from `src_file_` / `mem_src_ptr_` **without ever being transformed**, whereas `Decompressor50` applies the inverse filter (`apply_e8` / `apply_arm` / `apply_delta`) across the full 1 MiB region—corrupting the second 512 KiB of every 1 MiB filter chunk (and scrambling all channel strides in `Delta` mode).
  2. **Incremental `Compressor50::feed()` path:** In `feed()`, `src_size_` is updated incrementally (`src_size_ = src_loaded_`). When a caller feeds small chunks (e.g., 4 KiB at a time) of executable/audio data, `chunk_len = std::min(src_size_ - filter_emitted_until_, max_chunk)` at line 1764 sizes the filter token to the partial `src_loaded_` rather than `max_chunk` before line 1819 (`cur_ + STREAM_LOOKAHEAD > src_loaded_`) defers match finding, breaking byte-identity with `compress_buffer()`.
* **Remediation:** Before emitting and transforming a filter region at `filter_emitted_until_`, ensure at least `filter_emitted_until_ + max_chunk` bytes are loaded (via `load_data(filter_emitted_until_ + max_chunk)` in one-shot mode, or by deferring in `process_available(!final)` when `streaming_ && !final && filter_emitted_until_ + max_chunk > src_loaded_`), and assert `trans_len == chunk_len`.

---

#### 3. Unsigned Underflow & Silent Output Truncation on Cross-Block Filter Regions in `Decompressor50::ApplyEngine::flush_pending()`
* **Location:** [`src/compress/decompressor50.cpp:475-536, 757, 804-805, 845`](file:///c:/Users/Matt/dev/openrar/src/compress/decompressor50.cpp#L475-L536)
* **Finding:** In per-block streaming mode (`Decompressor50::decompress_block()` / `StreamDecoder`), `engine_.begin_call()` resets `last_flushed_ = 0` on each block while `base_at_entry` advances by prior blocks' `total_written`, and pending `FilterEntry` items persist in `engine_.filters_` with absolute coordinates (`f.block_start`).
  When a filter region starts in Block $N$ (`f.block_start < base_at_entry`) and completes in Block $N+1$ (`f.block_start + f.block_length <= abs_pos()`), Block $N$ held back `(base_at_entry - f.block_start)` bytes in `window_`. In Block $N+1$, `flush_pending()` extracts and emits the full `to_emit = f.block_length` bytes and executes:
  ```cpp
  last_flushed_ += to_emit;
  ```
  Because `to_emit` includes the `(base_at_entry - f.block_start)` bytes from Block $N$, `last_flushed_` (which is in Block $N+1$'s call-local frame `[0, total_written]`) overshoots Block $N+1$'s local position by `base_at_entry - f.block_start`. This causes:
  1. **Unsigned underflow:** `total_written - engine_.last_flushed_` at lines 845, 948, 1010, and 1034 underflows in `size_t` to `~SIZE_MAX >= 65536`, triggering `flush_pending()` on every subsequent token.
  2. **Silent data loss:** Up to `(base_at_entry - f.block_start)` plain bytes decoded after the filter in Block $N+1$ have local offsets `< last_flushed_` and are silently skipped by `flush_plain_up_to()`.
* **Remediation:** Advance `last_flushed_` to the local end of the filter region (`region_local + to_emit - (f.block_start < base_at_entry ? static_cast<size_t>(base_at_entry - f.block_start) : 0)`), and clamp `to_emit` against `dest_size` using the local end rather than `last_flushed_ + to_emit`.

---

#### 4. 32-Bit `size_t` Overflow on Untrusted `f_start` Filter Offsets & Unchecked Window Wrap in `Decompressor50`
* **Location:** [`src/compress/decompressor50.cpp:449-450, 477, 886, 915, 1599, 1620`](file:///c:/Users/Matt/dev/openrar/src/compress/decompressor50.cpp#L886)
* **Finding:**
  1. In `decompress_internal()` (line 886) and `apply_span_records()` (line 1599), `FilterEntry::block_start` is a `size_t` computed as:
     ```cpp
     fe.block_start = static_cast<size_t>(base_at_entry) + total_written + f_start;
     ```
     where `f_start` is an untrusted 32-bit unsigned integer read from the bitstream (`0..0xFFFFFFFF`). On 32-bit/WASM targets (`sizeof(size_t) == 4`), an attacker-chosen `f_start` wraps `fe.block_start` modulo $2^{32}$ to an arbitrary offset `< total_written` (and `< last_flushed_`). Even on 64-bit targets, `fe.block_start + fe.block_length` is not overflow-checked before lines 477, 915, and 1620. When `fe.block_start` wraps below `last_flushed_`, `flush_pending()` re-emits already-flushed window bytes and advances `last_flushed_` past `total_written`.
  2. In `ApplyEngine::flush_plain_up_to()` (line 449):
     ```cpp
     size_t back = (total_written - last_flushed_) % win_size_;
     ```
     There is no check that `total_written - last_flushed_ <= win_size_`. If a caller constructs `Decompressor50` (or calls `openrar_decompress2`) with `win_size < 65536` (smaller than the 64 KiB flush cadence), the circular `window_` wraps and overwrites unflushed bytes before `flush_pending()` runs, and `% win_size_` silently reads corrupted bytes.
* **Remediation:** Store `FilterEntry::block_start` as `core::uint64`, reject filter tokens where `f_start > UINT64_MAX - (base_at_entry + total_written)` or `fe.block_length > UINT64_MAX - fe.block_start`, enforce a minimum window floor (`>= 0x10000` or flush cadence `std::min<size_t>(65536, engine_.win_size_)`), and fail closed in `flush_plain_up_to()` if `total_written - last_flushed_ > win_size_`.

---

#### 5. Strict Aliasing & Alignment Violations via Raw Pointer Type-Punning (`reinterpret_cast`)
* **Location:**
  * [`src/api/abi_contract.hpp:74, 105`](file:///c:/Users/Matt/dev/openrar/src/api/abi_contract.hpp#L105) (`reinterpret_cast<ArchiveEntryOut*>(entries.data())`)
  * [`src/io/win32_meta.cpp:223, 330, 385`](file:///c:/Users/Matt/dev/openrar/src/io/win32_meta.cpp#L223) (`reinterpret_cast<const REPARSE_DATA_BUFFER*>(data)`)
  * [`src/io/file_stream.cpp:476, 591`](file:///c:/Users/Matt/dev/openrar/src/io/file_stream.cpp#L476) (`reinterpret_cast<OpenrarRenameInfo*>(buf.data())`)
  * [`src/crypto/aes256.cpp:305, 317`](file:///c:/Users/Matt/dev/openrar/src/crypto/aes256.cpp#L305) (`reinterpret_cast<const uint8x16_t*>(enc_buf)`)
* **Finding:**
  1. In `win32_meta.cpp:330`, `core::byte buf[16384]` is declared on the stack with 1-byte alignment and passed to `parse_reparse_buffer()`, which casts it to `const REPARSE_DATA_BUFFER*` (`alignof(REPARSE_DATA_BUFFER) == 4`) and dereferences its members directly—violating both alignment requirements and C++ strict aliasing (`[basic.lval]`).
  2. In `abi_contract.hpp:105`, `std::vector<uint8_t> entries` is cast to `ArchiveEntryOut*` (`alignas(8)`) and mutated through `out[i]` without placement `new` or `std::memcpy`.
  3. In `aes256.cpp:305, 317`, `enc_buf` / `dec_buf` (`const core::byte*`) are cast to `const uint8x16_t*` and indexed directly (`rk[r]`) rather than loaded via `vld1q_u8()`.
* **Remediation:** Construct structs in local stack variables and copy to/from byte buffers via `std::memcpy`, add `alignas(REPARSE_DATA_BUFFER)` to stack buffers where OS APIs require struct layout, and use `vld1q_u8(enc_buf + r * 16)` in `aes256.cpp`.

---

#### 6. Data Races on Shared State Across Multi-Threaded CLI & Handle Boundaries
* **Location:**
  * [`src/cli/main.cpp:1051, 1095, 1112`](file:///c:/Users/Matt/dev/openrar/src/cli/main.cpp#L1112)
  * [`src/dll/dll_api.cpp:1396-1421, 2109-2163, 2348-2485`](file:///c:/Users/Matt/dev/openrar/src/dll/dll_api.cpp#L2348-L2485)
  * [`src/wasm/archive_api.cpp:452-662`](file:///c:/Users/Matt/dev/openrar/src/wasm/archive_api.cpp#L452-L662)
* **Finding:**
  1. In `src/cli/main.cpp:1051, 1112`, `int skipped_count = 0;` is a non-atomic stack variable captured by reference and incremented (`skipped_count++`) concurrently by worker threads inside `pool.submit(...)` without synchronization—Undefined Behavior under `[intro.races]`.
  2. In `src/dll/dll_api.cpp:2348-2485`, `g_stream_handles.pin(handle)` releases the table lock and returns a `std::shared_ptr<StreamHandle>`, allowing concurrent calls to `openrar_stream_feed`, `openrar_stream_finish`, `openrar_stream_set_progress`, and `openrar_stream_set_cancel` on the same handle to race on `StreamEncoder` state and callback pointers without a `BusyGuard` or mutex.
  3. In `src/dll/dll_api.cpp:1396-1421, 2109-2163`, `openrar_archive_handle_list`, `openrar_archive_handle_entry_ex`, and `openrar_archive_handle_entry_owner` access handle state without acquiring `BusyGuard`.
  4. In `src/wasm/archive_api.cpp:452-662`, `ArchiveHandle` has no `BusyGuard`. Re-entering `openrar_archive_handle_extract`, `openrar_archive_handle_extract_all2`, or `openrar_archive_handle_set_limits` from a host `progress` or `cancel` callback mutates `h->ba.cached_entries_` and `h->limit_state` mid-iteration.
* **Remediation:** Change `skipped_count` to `std::atomic<int>` in `main.cpp`, add `BusyGuard` to `StreamHandle` and to the remaining `ArchiveHandle` entry points in `dll_api.cpp` and `archive_api.cpp`.

---

### `[HIGH]`

#### 1. Manual `new` / `delete` Exception-Safety Leaks of Heap Memory & OS File Handles
* **Location:**
  * [`src/recovery/recovery_writer.cpp:1594-1706`](file:///c:/Users/Matt/dev/openrar/src/recovery/recovery_writer.cpp#L1594-L1706)
  * [`src/io/extraction_journal.cpp:375, 419, 447-452, 504, 535-540, 639, 761`](file:///c:/Users/Matt/dev/openrar/src/io/extraction_journal.cpp#L419)
* **Finding:**
  1. In `RecoveryWriter::repair_rev_volumes()`, up to $2 \times \text{nd}$ `io::FileStream` objects are allocated with raw `new io::FileStream()` and held in `std::vector<io::FileStream*> owned` and `out`. Subsequently, lines 1667–1668 allocate `srcbuf` (1 MiB) and `recon` (`missing` $\times$ 1 MiB, up to 200 MiB) before the manual `delete` loops at lines 1698–1706. If `std::bad_alloc` is thrown during buffer allocation (which is caught at the DLL boundary in `openrar_archive_repair`), all `io::FileStream` objects and their underlying OS file handles are leaked, leaving volume files locked on disk.
  2. In `ExtractionSession::register_temp()` and `register_temp_contained()`, `LockedFile* lf = new LockedFile()` is stored as a raw `void*` in `DirJournal`. If `journals_.emplace(...)`, `journal_paths_[key] = ...`, or `lru_.push_back(key)` throws `std::bad_alloc`, `lf` and its exclusive OS lock handle are leaked.
* **Remediation:** Replace `std::vector<io::FileStream*>` with `std::vector<std::unique_ptr<io::FileStream>>` in `recovery_writer.cpp`, and manage `LockedFile` via `std::unique_ptr<LockedFile>` (or a custom deleter) in `ExtractionSession`.

---

#### 2. Heap Memory Leaks on Exception or `RAR_ERR_PARTIAL_OK` in Public C++17 Header Wrapper (`openrar.hpp`)
* **Location:** [`include/openrar/openrar.hpp:67-76, 87-96, 128-136, 143-152, 164-173, 184-194, 209-219, 223-230, 236-254, 275-284, 415-433, 457-490, 572-579`](file:///c:/Users/Matt/dev/openrar/include/openrar/openrar.hpp#L236-L254)
* **Finding:** Across `include/openrar/openrar.hpp`, raw buffers returned by the C ABI (`out`, `entries`, `paths`, `buf`, `offs`, `extra`, `cmt`) are held as bare pointers across operations that can throw (`std::vector::assign`, `std::string` construction, `std::map::operator[]`, and `list_archive()`). For example, in `openrar::extract_all()` (lines 236–254), `buf` and `offs` are allocated by `openrar_archive_extract_all()`, and then `list_archive(data, size)` and `v.assign(...)` are executed before `openrar_free(buf)` and `openrar_free(offs)`. If `list_archive()` throws or `std::bad_alloc` occurs during container population, the `malloc`'d C ABI buffers are leaked.
* **Remediation:** Wrap all C ABI output pointers in RAII guards (e.g., `std::unique_ptr<uint8_t, decltype(&openrar_free)>`) immediately after the C ABI call.

---

#### 3. Stale `external_buf_` / `mem_src_ptr_` Pointer State Across `Compressor50::start_file()`
* **Location:** [`src/compress/compressor50.cpp:531-564, 586-618`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L531-L564)
* **Finding:** `Compressor50::start_file(io::FileStream* src, core::uint64 src_file_size, bool continue_window)` sets `src_file_ = src`, but does not reset `external_buf_ = false` or `mem_src_ptr_ = nullptr`. If a `Compressor50` instance previously used `set_external_buffer()`, `load_data()` short-circuits at `if (external_buf_) return;`, either reading from a dangling external pointer (if `continue_window == true`) or failing to load any bytes from `src_file_`.
* **Remediation:** Reset `external_buf_ = false;` and `mem_src_ptr_ = nullptr;` in `Compressor50::start_file()` and ensure `ensure_internal_buffer()` refreshes `buf_data_`.

---

#### 4. Shared `stream_` File-Position Race in `ArchiveReader::extract_store_entry_impl()`
* **Location:** [`src/archive/archive_reader.cpp:3308-3328`](file:///c:/Users/Matt/dev/openrar/src/archive/archive_reader.cpp#L3308-L3328)
* **Finding:** While `ArchiveReader::read_packed_data()` ([`archive_reader.cpp:2116-2120`](file:///c:/Users/Matt/dev/openrar/src/archive/archive_reader.cpp#L2116-L2120)) opens a private `io::FileStream` via `open_extent_source()` in the legacy single-extent fallback (`entry.extents.empty()`) to avoid racing on the shared `stream_` file offset, `ArchiveReader::extract_store_entry_impl()` still calls `stream_.seek()` and `stream_.read()` directly on `this->stream_`.
* **Remediation:** Open a local `io::FileStream` via `open_extent_source(path_, local)` in `extract_store_entry_impl()`'s fallback path.

---

### `[MEDIUM]`

#### 1. `Compressor50::begin_stream(..., continue_window = true)` Rejects Non-Power-of-Two Window Sizes
* **Location:** [`src/compress/compressor50.cpp:411-415, 454`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L454)
* **Finding:** On the first member of a solid stream (`continue_window = false`), `begin_stream()` calls `begin_archive()`, which rounds `win_size_` up to a power of two (`pow2_ws`, with a `0x10000` floor). On the second member (`continue_window = true`), line 454 compares `win_size_ != win_size` against the caller's unrounded `win_size`. Any caller passing a RAR7 fractional window size (or a window `< 64 KiB`) to `StreamEncoder` / `SolidPacker` will fail on `pack_solid_begin(true)`.
* **Remediation:** Apply the same power-of-two rounding helper to `win_size` before comparing `win_size_ != pow2_ws` in `begin_stream()`.

---

#### 2. Unvalidated Null Pointer Arithmetic in `BitReader(const core::byte* data, size_t size)`
* **Location:** [`src/compress/decompressor50.cpp:14-15`](file:///c:/Users/Matt/dev/openrar/src/compress/decompressor50.cpp#L14-L15)
* **Finding:** `BitReader::BitReader(const core::byte* data, size_t size)` computes `end_(data + size)` unconditionally. If constructed directly with `data == nullptr` and `size > 0`, `nullptr + size` is Undefined Behavior (`[expr.add]`) and `refill()` dereferences `nullptr`.
* **Remediation:** Guard the constructor: if `data == nullptr`, clamp `size = 0` so `p_ = end_ = nullptr` and `size_ = fetched_ = 0`.

---

#### 3. Over-Wide Unterminated String Scan in `openrar_archive_create()`
* **Location:** [`src/dll/dll_api.cpp:1308-1310`](file:///c:/Users/Matt/dev/openrar/src/dll/dll_api.cpp#L1308-L1310), [`src/wasm/archive_api.cpp:283-285`](file:///c:/Users/Matt/dev/openrar/src/wasm/archive_api.cpp#L283-L285)
* **Finding:** `while (plen < 4096 && p[plen] != 0) ++plen;` scans up to 4096 bytes before `validate_archive_path()` rejects `plen > 2048`. If a caller passes an unterminated buffer between 2049 and 4095 bytes near a page boundary, scanning to 4096 can fault unnecessarily.
* **Remediation:** Bound the scan loop to `plen <= 2048` (2049 bytes max), which is sufficient to trigger `validate_archive_path()`'s `> 2048` rejection without reading up to 4096 bytes.

---

### `[LOW]`

#### 1. Raw Pimpl Pointer in `MappedFile` Instead of `std::unique_ptr<Impl>`
* **Location:** [`src/io/mapped_file.hpp:61-62`](file:///c:/Users/Matt/dev/openrar/src/io/mapped_file.hpp#L61-L62), [`src/io/mapped_file.cpp:51-56, 147-152`](file:///c:/Users/Matt/dev/openrar/src/io/mapped_file.cpp#L51-L56)
* **Finding:** `MappedFile` manually allocates `impl_` with `new Impl` and frees it with `delete impl_;` in `~MappedFile()`.
* **Remediation:** Replace `Impl* impl_` with `std::unique_ptr<Impl> impl_`.

---

#### 2. Redundant `std::malloc(1)` + Immediate `std::free()` on Empty Output Buffers
* **Location:** [`src/dll/dll_api.cpp:1188-1196`](file:///c:/Users/Matt/dev/openrar/src/dll/dll_api.cpp#L1188-L1196), [`src/wasm/wasm_api.cpp:220-225, 313-318`](file:///c:/Users/Matt/dev/openrar/src/wasm/wasm_api.cpp#L220-L225), [`src/wasm/archive_api.cpp:199-207`](file:///c:/Users/Matt/dev/openrar/src/wasm/archive_api.cpp#L199-L207)
* **Finding:** These functions allocate `std::malloc(out.size() > 0 ? out.size() : 1)` and then immediately call `if (out.empty()) std::free(buf);` to return `*out_ptr = nullptr`.
* **Remediation:** Use `openrar::api::heap_dup(out.data(), out.size())` (as already used in `ArchiveHandle::extract`) to avoid allocating and immediately freeing 1 byte on empty outputs.
