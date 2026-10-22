## Phase 2.3: Hot Paths, Zero-Allocation & SIMD Optimisations

---

### `[CRITICAL]`

*(No new `[CRITICAL]` memory-safety or corruption defects introduced in this phase; all hot-path allocation and vectorisation findings are classified under `[HIGH]`, `[MEDIUM]`, and `[LOW]` per the Priority Classification rules.)*

---

### `[HIGH]`

#### 1. Per-Chunk 1 MiB Heap Allocations (`std::vector<core::byte> tmp`) Inside Delta Filter Loops
* **Location:**
  * [`src/compress/compressor50.cpp:1795-1800`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L1795-L1800) (`Compressor50::process_available()`)
  * [`src/compress/compressor50.cpp:2146-2151`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L2146-L2151) (`Compressor50::compress_buffer()`)
  * [`src/compress/filters50.cpp:422-427, 774-779`](file:///c:/Users/Matt/dev/openrar/src/compress/filters50.cpp#L422-L427) (`Filters50::apply_delta()`, `Filters50::encode_delta()`)
* **Finding:** While `Decompressor50::ApplyEngine` maintains persistent `filter_scratch_` and `delta_scratch_` members (`decompressor50.hpp:371-372`), the compression paths allocate, zero-fill, and free a fresh `std::vector<core::byte> tmp(trans_len)` (up to 1 MiB per filter chunk) on **every iteration** of the filter-chunk loop in `Compressor50::process_available()` and `Compressor50::compress_buffer()`. Furthermore, if `Filters50::apply_delta()` or `Filters50::encode_delta()` is invoked in-place (`src == dest`), it allocates another temporary `std::vector<core::byte>` of size `size`.
* **Remediation:** Add a persistent `std::vector<core::byte> filter_scratch_;` member to `Compressor50` (and hoist `tmp` outside the `while (chunk_start < src_size)` loop in `compress_buffer()`), resizing once to `max_chunk`.

---

#### 2. Multi-Channel (`channels >= 2`) Delta Filter Lacks SIMD & Thrashes L1/L2 Cache via Strided Multi-Pass Loops
* **Location:** [`src/compress/filters50.cpp:432-498, 781-824`](file:///c:/Users/Matt/dev/openrar/src/compress/filters50.cpp#L487-L493)
* **Finding:** `Filters50::detect_filter()` configures `channels >= 2` for almost all real-world Delta inputs (default override is `2`; 16-bit mono/stereo PCM WAV is `2` or `4` channels). However, the SSE2/NEON paths in `apply_delta()` and `encode_delta()` only execute when `channels == 1`. For `channels >= 2`:
  ```cpp
  for (core::uint8 ch = 0; ch < channels; ++ch) {
      core::byte prev = 0;
      for (size_t dest_pos = ch; dest_pos < size; dest_pos += channels) {
          prev = static_cast<core::byte>(prev - src[src_pos++]);
          out[dest_pos] = prev;
      }
  }
  ```
  1. **Vectorisation Block:** In `apply_delta()`, the inner loop has a loop-carried data dependency on `prev` combined with non-unit-stride scatter stores (`out[dest_pos]`, stride `channels`), completely blocking compiler auto-vectorisation.
  2. **Cache Thrashing:** Because `size` is typically 1 MiB (exceeding L1 and L2 caches), the outer loop makes `channels` separate passes (up to 32 passes) over the entire 1 MiB buffer, evicting cache lines on every pass.
* **Remediation:** Add specialized SIMD kernels for `channels == 2` and `channels == 4` (using `vld2q_u8`/`vst2q_u8` or `_mm_shuffle_epi8` de-interleave/interleave + SIMD prefix-sum), and for arbitrary `channels`, tile the transpose + prefix-sum across L1-resident strips (e.g., 16 KiB tiles) so `out` (in `apply_delta`) and `src` (in `encode_delta`) are traversed in a single cache-friendly pass.

---

#### 3. Per-32-Bit-Word `std::vector::resize()` & Zero-Fill Inside `BitOutput::put_bits()` Hot Path
* **Location:** [`src/compress/compressor50.hpp:47-70`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.hpp#L47-L70), [`src/compress/compressor50.cpp:1403-1497`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L1403-L1497)
* **Finding:** `BitOutput::put_bits()` is the innermost bit-packing primitive, called 1–4 times per token in `Compressor50::emit_tokens()`. Whenever `acc_bits_ >= 32`, it executes:
  ```cpp
  std::vector<core::byte>& dst = (mem_dest_ != nullptr) ? *mem_dest_ : buf_;
  size_t sz = dst.size();
  if (sz + 4 > dst.capacity()) dst.reserve(sz + OUTBUF_SIZE);
  dst.resize(sz + 4);
  std::memcpy(&dst[sz], &val, 4);
  ```
  * `dst.resize(sz + 4)` value-initializes (zeroes) the 4 new bytes before `std::memcpy` immediately overwrites them, and writes the updated vector size to memory on every 32-bit word—creating a pointer-aliasing barrier that prevents the compiler from keeping the write pointer and `acc_` in registers across consecutive `put_bits()` calls.
  * When `sz + 4 > dst.capacity()`, `dst.reserve(sz + OUTBUF_SIZE)` grows **linearly** by 256 KiB (`OUTBUF_SIZE`) rather than geometrically, causing repeated reallocations on large blocks.
* **Remediation:** Pre-size or reserve the output buffer for the block (`token_seq_.size() * 10 + 1024`), maintain a raw `core::byte* out_ptr` cursor during `emit_table()` / `emit_tokens()`, and flush 32-bit (or 64-bit via `_byteswap_uint64` / `__builtin_bswap64`) words directly to `out_ptr`, updating `dst`'s size once in `finish_block_data()`.

---

#### 4. Serial Shift-and-Branch `while` Loops Computing $\lfloor\log_2(x)\rfloor$ in `Compressor50::find_match_impl()`
* **Location:** [`src/compress/compressor50.cpp:906-914, 996-1004, 1932`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L906-L914)
* **Finding:** In `Compressor50::find_match_impl()`, the shipped v1.44 A-6 back-shift evaluator defines `price_fresh` and invokes it up to **5 times per match search**:
  ```cpp
  auto price_fresh = [](size_t len, core::uint32 inc, size_t dist) -> core::uint32 {
      core::uint32 m = 0;
      size_t v = len - inc;
      while (v > 1) { v >>= 1; ++m; }
      size_t dv = dist;
      core::uint32 d = 0;
      while (dv > 1) { dv >>= 1; ++d; }
      return m + 9 + (d > 3 ? d - 3 : 0);
  };
  ```
  Each invocation runs two data-dependent `while` loops (up to ~40 iterations and unpredictable loop-exit branches per call, or up to ~200 iterations per byte position) to compute the integer $\log_2$ of `len - inc` and `dist`.
* **Remediation:** Replace both `while` loops with a single hardware leading-zero count intrinsic (`_BitScanReverse64` on MSVC / `63u - static_cast<core::uint32>(__builtin_clzll(v | 1))` on GCC/Clang), collapsing both loops into 2 branchless single-cycle instructions.

---

#### 5. Per-Block Heap Allocations (`std::vector<core::byte> lengths`) in `Decompressor50::prescan_member()`
* **Location:** [`src/compress/decompressor50.hpp:196-199, 206-212`](file:///c:/Users/Matt/dev/openrar/src/compress/decompressor50.hpp#L196-L199), [`src/compress/decompressor50.cpp:1085-1123`](file:///c:/Users/Matt/dev/openrar/src/compress/decompressor50.cpp#L1095-L1101)
* **Finding:** Because RAR5 encoders emit `table_present = true` on every block, `Decompressor50::prescan_member()` executes:
  ```cpp
  PrescanDescription d;
  d.extra_dist = use_extra_dist_;
  d.lengths.assign(table_, table_ + cur_table_size_);
  out.descriptions.push_back(std::move(d));
  ```
  on **every block in the member**, performing a separate heap allocation for each block's 430/446-byte `lengths` vector, plus unreserved geometric reallocations of `out.blocks` and `out.descriptions`.
* **Remediation:** Replace `std::vector<core::byte> lengths;` in `PrescanDescription` with `std::array<core::byte, 446> lengths{};` (eliminating all per-block heap allocations) and pre-reserve `out.blocks` and `out.descriptions` using `src_size / 32768`.

---

#### 6. `Inflate::window_copy()` Executes $O(\text{len} / \text{dist})$ Small `std::memcpy` Calls & Double-Writes Every Output Byte
* **Location:** [`src/compress/inflate.cpp:404-449, 490-502`](file:///c:/Users/Matt/dev/openrar/src/compress/inflate.cpp#L413-L449)
* **Finding:**
  1. **No span doubling on overlapping matches:** In `Inflate::window_copy()`, when `dist` is small (e.g., `dist = 2`, `len = 258`), `chunk` is clamped to `gap = 2`, and the loop executes 129 iterations of two 2-byte `std::memcpy` calls (258 `memcpy` calls for a single match) instead of doubling the copied prefix in `window_` or specializing `dist == 2, 3, 4, 8`.
  2. **Redundant double-buffering (`out_buf_` + `window_`):** Every literal (`emit_literal`) and match (`window_copy`) writes every byte to both `out_buf_` (64 KiB) and `window_` (32 KiB). Flushing directly from `window_` whenever `window_pos_` wraps (every 32 KiB) eliminates `out_buf_` and halves memory write bandwidth.
  3. **Bit-by-bit stored block copy:** In `Inflate::decode()` (`btype == 0`, lines 490–502), stored blocks read byte-by-byte through `read_byte()` into a 4 KiB `scratch` buffer instead of draining the byte-aligned `bit_acc_` and slicing directly from `in_buf_`.
* **Remediation:** Flush directly from `window_` at 32 KiB wrap boundaries, expand short overlapping matches (`dist < 16`) using power-of-two doubling or SIMD broadcast, and bulk-copy stored blocks from `in_buf_`.

---

#### 7. Scalar 1-Way Execution of 8-Way Parallel `Blake2sp` Tree Hash
* **Location:** [`src/crypto/blake2sp.cpp:60-94, 163-190`](file:///c:/Users/Matt/dev/openrar/src/crypto/blake2sp.cpp#L163-L190)
* **Finding:** BLAKE2sp is designed for 8-way SIMD parallelism (8 independent 32-bit BLAKE2s leaf instances operating in lockstep across 512-byte chunks). However, `Blake2sp::update()` executes the 8 leaves sequentially in a scalar loop (`for (size_t i = 0; i < PARALLELISM; ++i) states_[i].update(...)`), and each leaf copies input through its own 128-byte `buf_` (`std::memcpy`) before compressing.
* **Remediation:** Add an AVX2 8-way vertical SIMD kernel (where each `v[0..15]` state word across the 8 leaves occupies one `__m256i` register) and a NEON/SSE2 4-way kernel, compressing directly from `in` whenever `size >= 512`.

---

### `[MEDIUM]`

#### 1. `match_tokens_` Vector Under-Reserved Relative to `need_flush()` Threshold (`16384` vs `32768`)
* **Location:** [`src/compress/compressor50.cpp:421-423, 1271-1273`](file:///c:/Users/Matt/dev/openrar/src/compress/compressor50.cpp#L421-L423)
* **Finding:** `Compressor50::begin_archive()` reserves `32768` entries for `token_seq_` and `lit_bytes_`, but only `16384` entries for `match_tokens_`. Because `need_flush()` flushes at `token_seq_.size() >= 32768`, any block containing $> 16,384$ match/rep tokens triggers a mid-loop heap reallocation and 256 KiB copy of `match_tokens_`.
* **Remediation:** Reserve `32768` entries for `match_tokens_` (and `32768 + 8` for `token_seq_`) in `begin_archive()`.

---

#### 2. Unconditional Input Buffer Copy & Potential `std::function` Heap Allocation in `StreamDecoder`
* **Location:** [`src/compress/stream_decoder.cpp:51-53, 100-114`](file:///c:/Users/Matt/dev/openrar/src/compress/stream_decoder.cpp#L51-L53)
* **Finding:**
  1. `StreamDecoder::feed()` unconditionally appends all incoming bytes to `in_queue_` (`in_queue_.insert(in_queue_.end(), src, src + n)`), even when `in_queue_` is empty (`in_head_ == in_queue_.size()`) and `src` contains complete RAR5 blocks that can be decoded zero-copy in place.
  2. Inside `StreamDecoder::process_blocks()` (lines 100–113), the `sink` lambda is converted to `Decompressor50::OutputCallback` (`std::function`) on every block iteration.
* **Remediation:** When `in_head_ == in_queue_.size()`, parse and decode complete blocks directly from `src` and only append the unconsumed tail `< total_block_bytes` to `in_queue_`. Hoist the `OutputCallback` `std::function` construction outside the `while` loop (or store it as a member).

---

#### 3. Per-Header Heap Allocations in `HeaderReader::read_block_raw()`
* **Location:** [`src/format/header_reader.cpp:100, 112, 142, 145, 179`](file:///c:/Users/Matt/dev/openrar/src/format/header_reader.cpp#L100)
* **Finding:** `HeaderReader::read_block_raw()` allocates a heap `std::vector<core::byte> size_vint_bytes` on every header read to store at most 10 bytes (`MAX_VINT_SIZE`), and in encrypted-header mode (`crypt && crypt->active`), allocates 4 separate `std::vector<core::byte>` buffers per header (`plain_first(16)`, `size_vint_bytes`, `plain`, `cipher_rest`).
* **Remediation:** Replace `plain_first(16)` and `size_vint_bytes` with fixed stack arrays (`core::byte plain_first[16]`, `core::byte size_vint_bytes[10]`), and reuse `out_body` directly for decrypting the remaining header bytes.

---

#### 4. Per-Byte `cand_crc.update(&b, 1)` & Redundant GF(2^16) Multiplies in `RecoveryWriter::repair()`
* **Location:** [`src/recovery/recovery_writer.cpp:2401, 2468-2495`](file:///c:/Users/Matt/dev/openrar/src/recovery/recovery_writer.cpp#L2468-L2495)
* **Finding:** During Tier-2 syndrome candidate verification in `RecoveryWriter::repair()`, lines 2472–2494 iterate byte-by-byte over the entire entry (`entry.data_size`), recomputing `rs_enc.gf_mul(rs_enc.gf_add(j0 + d, s), s0)` on every byte and calling `cand_crc.update(&b, 1)` 1 byte at a time—bypassing hardware PCLMULQDQ / ARMv8 CRC32 / slicing-by-8 acceleration. Additionally, `std::vector<core::byte> body` at line 2401 is allocated inside the header scan loop instead of hoisted outside.
* **Remediation:** Hoist `body` outside the loop at line 2398, feed untouched prefix/suffix ranges before/after `[s_byte_start, s_byte_end)` to `cand_crc.update()` in bulk, and transform the `[s_byte_start, s_byte_end)` slice in a stack chunk buffer using `rs_enc.update_ecc()` before hashing.

---

### `[LOW]`

#### 1. Unvectorized 8-Way Slicing CRC-64/XZ (`crc64.cpp`)
* **Location:** [`src/crypto/crc64.cpp:36-53`](file:///c:/Users/Matt/dev/openrar/src/crypto/crc64.cpp#L36-L53)
* **Finding:** `crc64_update_slice8()` uses scalar slicing-by-8 table lookups, whereas `crc32.cpp` dispatches to PCLMULQDQ (`_mm_clmulepi64_si128`) on x86-64. Adding a PCLMULQDQ / PMULL folding path for CRC-64/XZ would accelerate inline recovery-record shard generation and verification on large archives.
* **Remediation:** Optionally add a carry-less multiplication (`_mm_clmulepi64_si128` / `vmull_p64`) folding path for buffers $\ge 64$ bytes, falling back to `crc64_update_slice8()` for the tail.