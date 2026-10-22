## Phase 2.2: Bit-Stream Logic, UB & Algorithmic Invariants

---

### `[CRITICAL]`

#### 1. 4 GiB Modulo Collision in `Compressor50::reconstruct_pos()` Causes Self-Matching (`distance == 0`) & Silent Stream Corruption
* **Location:** [`src/compress/compressor50.hpp:447-452`](../src/compress/compressor50.hpp#L447-L452), [`src/compress/compressor50.cpp:685, 742, 1155-1156`](../src/compress/compressor50.cpp#L685)
* **Finding:** When compressing an input stream $> 4\text{ GiB}$ with a standard window $\le 4\text{ GiB}$ (`is_large_window_ == false`), `head_` and `prev_` store 32-bit truncated positions (`static_cast<uint32>(pos)`). `reconstruct_pos()` reconstructs the 64-bit candidate position as:
  ```cpp
  inline core::int64 reconstruct_pos(core::uint64 pos, core::uint32 trunc) {
      if (trunc == 0xFFFFFFFFu) return -1;
      core::uint32 dist32 = static_cast<core::uint32>(pos) - trunc;
      if (dist32 > max_dist_ || dist32 > pos) return -1;
      return static_cast<core::int64>(pos - dist32);
  }
  ```
  When a stale hash-table or chain entry from **exactly $2^{32}$ bytes (4 GiB) earlier** is encountered, `trunc == static_cast<uint32>(pos)`, yielding **`dist32 == 0`** and returning **`cand == pos`**.
  * `find_match_impl()` then compares `buf_pos` against **itself** (`buf_cand == buf_pos`), which matches up to `limit` (`MAX_LZ_MATCH + 3 = 4100` bytes) and breaks out of the walk immediately with `best.candidate = pos` (`dist_f = 0`).
  * `process_available()` passes `dist = 0` to `add_match(4097, 0)` $\to$ `distance_to_slot(0, ...)`.
  * In Debug builds, `assert(distance >= 1)` aborts; in Release builds, `if (distance == 0) distance = 1;` (`compressor50.cpp:1156`) **silently rewrites `distance = 0` to `distance = 1`**, emitting a 4,097-byte RLE run of the previous byte and corrupting the compressed stream.
  * Similarly, in `LargeWindow` mode (`compressor50.cpp:685, 742`), `pos >= hpos` and `pos >= next64` allow `dist == 0` instead of requiring strict inequality (`pos > hpos` and `cand > next64`).
* **Remediation:** Reject `dist32 == 0` in `reconstruct_pos()` (`if (dist32 == 0 || dist32 > max_dist_ || dist32 > pos) return -1;`), and enforce strict inequality `pos > hpos` and ` next64 < static_cast<core::uint64>(cand)` in `find_match_impl<true>()`.

---

#### 2. 430-vs-446 Huffman Table-Size Desynchronization When Shrinking `Compressor50` Window Across the 4 GiB Boundary
* **Location:** [`src/compress/compressor50.cpp:1281, 2081-2088, 2254-2256`](../src/compress/compressor50.cpp#L1281), [`src/compress/parallel_compressor.cpp:293-295`](../src/compress/parallel_compressor.cpp#L293-L295), [`src/archive/archive_mutator.cpp:1264, 1312-1315, 1347`](../src/archive/archive_mutator.cpp#L1347)
* **Finding:** Both `Compressor50::make_tables()` (`compressor50.cpp:1281`) and `Decompressor50` (`decompressor50.cpp:295-296`) select between the 430-symbol table (`TABLE_SIZE`, `DCB = 64`) and the 446-symbol RAR7 table (`TABLE_SIZEX`, `DCX = 80`) based strictly on whether `win_size_ > 4 GiB`. However:
  1. **Single-buffer clamp:** `Compressor50::compress_buffer()` (`compressor50.cpp:2081-2088`) clamps `comp_win_size` down to `pow2(src_size)` while `ArchiveMutator::prepare_add_file()` (`archive_mutator.cpp:1347`) records the unclamped `fb.win_size = win_size` in the file header when `dict_size` was explicitly specified (`> 4 GiB`). For any file $\le 4\text{ GiB}$ with an explicit $> 4\text{ GiB}$ dictionary, the encoder emits 430-symbol tables while `Decompressor50` reads 446-symbol tables.
  2. **Parallel chunk clamp:** In `compress_buffer_parallel()` (`compressor50.cpp:2255`) and `ParallelBlockPipeline::compress_stream()` (`parallel_compressor.cpp:294`), each chunk worker clamps `worker_win = std::min(win_size, seed_len + len)`. When `win_size > 4 GiB`, chunks with `off + len <= 4 GiB` run with `worker_win <= 4 GiB` and emit **430-symbol** tables, whereas chunks with `off > 4 GiB` (and the file header itself) use `> 4 GiB` and **446-symbol** tables—producing a hybrid stream that no RAR5/RAR7 decoder can unpack.
* **Remediation:** Preserve the session's `use_extra_dist` (446-entry table mode) whenever the member's declared `win_size > 4 GiB`, even when clamping the internal LZ allocation (`comp_win_size` / `worker_win`) to the input/chunk span.

---

#### 3. Shift-by-$\ge 32$ Undefined Behavior & 32-Bit Distance Wrap in `Decompressor50` on ILP32 / WASM32 Targets
* **Location:** [`src/compress/decompressor50.cpp:961-989, 1130, 1351-1379`](../src/compress/decompressor50.cpp#L961-L989)
* **Finding:** In both `Decompressor50::decompress_internal()` (lines 961–989) and `Decompressor50::decode_span()` (lines 1351–1379), match distances are accumulated in `size_t`:
  ```cpp
  size_t distance = 1;
  ...
  d_bits = dist_slot / 2 - 1;
  distance += static_cast<size_t>(2 | (dist_slot & 1)) << d_bits;
  ...
  distance += static_cast<size_t>(extra) << 4;
  ```
  On 32-bit and WASM32 targets (`sizeof(size_t) == 4`):
  1. `Decompressor50::initialize_from_description()` (line 1130) accepts `desc.extra_dist = true` (`DCX = 80`, `dist_slot` up to 79, `d_bits` up to 38) without gating on 64-bit `size_t`. For `dist_slot >= 66` (`d_bits >= 32`), `static_cast<size_t>(2 | (dist_slot & 1)) << d_bits` shifts a 32-bit `size_t` by `32..38` bits—**Undefined Behavior (`[expr.shift]`)**.
  2. Even with `use_extra_dist_ == false` (`dist_slot` in `0..63`, `d_bits` up to 30) or `dist_slot in 64..65` (`d_bits == 31`), `distance` wraps modulo $2^{32}$. For `dist_slot == 64` or `65`, a distance $> 4\text{ GiB}$ wraps into `[1, win_size_]`, bypassing `if (distance == 0 || distance > engine_.win_size_) return false;` and copying from a bogus window offset.
* **Remediation:** Compute `distance` in `core::uint64` (`static_cast<core::uint64>(2 | (dist_slot & 1)) << d_bits` and `extra << 4`), validate `if (distance == 0 || distance > engine_.win_size_) return false;` in 64-bit, and only then cast to `size_t`. Also reject `desc.extra_dist` on 32-bit targets in `initialize_from_description()`.

---

#### 4. Big-Endian Byte-Order Miscomputation in `match_length_scalar()` & `Compressor50::hash_range()`
* **Location:** [`src/compress/arch/match_simd.hpp:40-56, 126-140, 242-257, 282-290`](../src/compress/arch/match_simd.hpp#L40-L56), [`src/compress/compressor50.cpp:620-661`](../src/compress/compressor50.cpp#L620-L661)
* **Finding:**
  1. In `match_length_scalar()` (and the 8-byte tails in `match_length_avx2`, `match_length_neon`, `match_length_wasm`), 8 bytes are loaded via raw `std::memcpy(&p_val, p + i, 8)` and the first mismatching byte index is computed via `__builtin_ctzll(p_val ^ q_val) / 8` (trailing zeros). Trailing-zero count only identifies the lowest-address byte on **little-endian** hosts; on big-endian hosts, the first byte is in the most-significant bits, so `__builtin_ctzll` returns the distance from the *end* of the 8-byte word—reporting a match length longer than the actual match and **corrupting the compressed output**.
  2. In `Compressor50::hash_range()` (`compressor50.cpp:644-651`), `std::memcpy(&q, ..., 8)` is shifted right (`q >> 8`, `q >> 16`, `q >> 24`) to extract the 4-byte words at offsets `+0, +1, +2, +3`. On big-endian hosts, `static_cast<uint32>(q)` extracts offset `+4` instead of `+0`, breaking the invariant that `hash_range()` inserts the same hashes as `calc_hash()`.
* **Remediation:** Load `p_val`, `q_val`, and `q` via `core::read_le64()` (and `core::read_le32()` in `calc_hash()`), which compiles to a single unaligned load instruction on all little-endian targets while preserving exact semantics on big-endian targets.

---

#### 5. Signed 64-Bit Integer Overflow UB in Timestamp Conversions (`foreign_transcode.cpp`, `foreign_zip.cpp`)
* **Location:**
  * [`src/archive/foreign_transcode.cpp:67`](../src/archive/foreign_transcode.cpp#L67)
  * [`src/archive/foreign_zip.cpp:287`](../src/archive/foreign_zip.cpp#L287)
* **Finding:**
  1. In `foreign_transcode.cpp:67`, `set_staging_mtime()` computes:
     ```cpp
     const auto target = std::chrono::seconds(epoch_sec) + std::chrono::nanoseconds(nsec);
     ```
     `epoch_sec` is clamped to `MtimeBounds` whose default maximum is `32,503,680,000` (year 3000, [`extraction_limits.hpp:81`](../src/archive/extraction_limits.hpp#L81)). Converting `std::chrono::seconds(epoch_sec)` to `std::chrono::nanoseconds` multiplies `int64_t epoch_sec` by $10^9$. For any `epoch_sec > 9,223,372,036` (any date after 2262-04-11, including every timestamp clamped to `MtimeBounds::max`), `epoch_sec * 1,000,000,000LL` **overflows signed `int64_t` (Undefined Behavior)**.
  2. In `foreign_zip.cpp:287`, an untrusted 64-bit NTFS FILETIME `ft` is converted via:
     ```cpp
     const int64_t epoch_100ns = static_cast<int64_t>(ft) - 116444736000000000LL;
     ```
     When `ft` has bit 63 set (`0x8000000000000000ULL <= ft < 0x8000000000000000ULL + 116444736000000000ULL`), `static_cast<int64_t>(ft)` is near `INT64_MIN` and subtracting `116444736000000000LL` **overflows signed `int64_t` (Undefined Behavior)**.
* **Remediation:** In `foreign_transcode.cpp`, compute the delta in `std::chrono::seconds` first or clamp `epoch_sec` to `<= 9000000000LL` before converting to nanoseconds. In `foreign_zip.cpp`, divide `ft` in unsigned 64-bit (`ft / 10000000ULL`) and clamp before converting to signed `int64_t` (matching `archive_reader.cpp:115-120`).

---

### `[HIGH]`

#### 1. `BitReader::peek_bits64()` Falsely Zero-Pads Non-EOF Streams for `count` in `58..64`
* **Location:** [`src/compress/decompressor50.cpp:25, 102-110`](../src/compress/decompressor50.cpp#L102-L110)
* **Finding:** `BitReader::refill()` loads whole bytes into the 64-bit accumulator `acc_`, stopping when `n_ > 56` (`n_` in `57..64`). When `n_ % 8 != 0` (e.g., `n_ == 57` after `refill()`), calling `peek_bits64(count)` with `count` in `58..64` executes:
  ```cpp
  unsigned int take = std::min(count, n_); // take = 57 < count
  if (take == count) return acc_ >> (n_ - count);
  if (take == 0) return 0;
  return (acc_ >> (n_ - take)) << (count - take);
  ```
  Even when megabytes of unread data remain in the stream (`p_ < end_`), `peek_bits64(58..64)` treats `n_ == 57` as EOF and **zero-fills the low `1..7` bits** instead of reading from `p_[0]`.
* **Remediation:** When `count > 57` and `n_ < count` after `refill()`, either pull the remaining `count - n_` bits from `*p_` (if `p_ < end_`) to form the full `count`-bit peek value, or restrict `peek_bits64` to `count <= 57` with an explicit assertion and clamp.

---

#### 2. Sequential vs. Parallel Block-Boundary Desynchronization on Token Over-Read Past `end_bit`
* **Location:** [`src/compress/decompressor50.cpp:626-704, 821-830, 1088-1123, 1226, 1283`](../src/compress/decompressor50.cpp#L821-L830)
* **Finding:**
  1. In `Decompressor50::decompress_internal()` (line 822), a single `BitReader` runs across all blocks in a member. The loop only checks `if (cb >= end_bit)` *between* tokens. If a token starting at `cb < end_bit` (or `read_tables()` itself) consumes bits past `end_bit` into the next block's byte range, `read_block_header()` calls `reader.align_byte()` from the over-read bit position, advancing into the middle of the next block.
  2. By contrast, `prescan_member()` (line 1118) and `decode_span()` (line 1226) construct a fresh `BitReader(src + expect.src_byte, ...)` at the exact byte boundary `payload_start + payload` for every block. Thus, if a block over-reads `end_bit`, the parallel two-phase decoder and sequential decoder diverge on where the next block header starts.
* **Remediation:** Verify `if (reader.bit_pos() > end_bit) return false;` at the end of `read_tables()` and after decoding each token in `decompress_internal()` and `decode_span()`.

---

#### 3. Missing Bit-Availability Checks in `HuffmanDecoder::decode()` and `Decompressor50::slot_to_length()` on Truncated Streams
* **Location:** [`src/compress/decompressor50.cpp:240-264, 584-597, 665-695, 840, 957-959, 983, 1027-1028`](../src/compress/decompressor50.cpp#L240-L264)
* **Finding:**
  1. `HuffmanDecoder::decode(BitReader& reader)` peeks 10 (or 15) zero-padded bits, looks up the code length `len` (`1..15`), and calls `reader.consume_bits(len)` without verifying `reader.bits_remaining() >= len` (unlike `Inflate::next_symbol` at [`inflate.cpp:463`](../src/compress/inflate.cpp#L463)). Callers only check `reader.bits_remaining() < 1` beforehand. At the end of a truncated stream with `1..14` bits remaining, `decode()` zero-pads missing bits and fabricates symbols.
  2. `Decompressor50::slot_to_length()` (line 594) calls `reader.get_bits(l_bits)` (`l_bits` up to 9) without checking `reader.bits_remaining() >= l_bits`. On a repeat match (`slot in 258..261`, lines 1028 and 1416), no subsequent bit check occurs, allowing a truncated stream to fabricate up to 4,097 bytes of output.
* **Remediation:** Check `reader.bits_remaining() >= len` inside `HuffmanDecoder::decode()` (or track a `truncated_` flag in `BitReader` like `Inflate` does) and check `reader.bits_remaining() >= l_bits` in `slot_to_length()`.

---

### `[MEDIUM]`

#### 1. Slot 257 (`LastLen`) Before First Match Silently No-Ops Instead of Failing Stream
* **Location:** [`src/compress/decompressor50.cpp:933-955, 1568-1584`](../src/compress/decompressor50.cpp#L933-L955)
* **Finding:** The encoder documentation ([`compressor50.cpp:1959-1960`](../src/compress/compressor50.cpp#L1959-L1960)) states: *"a 257 before any match is illegal (the decoder fails the stream on the unset rep sentinel)"*. However, in both `decompress_internal()` (line 934) and `apply_span_records()` (line 1571), the sentinel check `if (distance == static_cast<size_t>(-1) ...)` is nested *inside* `if (last_length_ != 0)`. Because `last_length_` is initialized to `0` alongside `old_dist_[0] = -1`, a `slot == 257` token before the first match skips the body of `if (last_length_ != 0)` and is silently ignored as a no-op.
* **Remediation:** Return `false` when `slot == 257` (or `OpRecord::Tag::R257`) is encountered with `last_length_ == 0`.

---

#### 2. Signed Integer Overflow UB & Negative-Offset Clamping in `MappedFile::seek()` and `BufferStream::seek()`
* **Location:** [`src/io/mapped_file.cpp:107-119, 201-213`](../src/io/mapped_file.cpp#L107-L119), [`src/io/buffer_stream.hpp:43-54`](../src/io/buffer_stream.hpp#L43-L54)
* **Finding:**
  1. In `MappedFile::seek()` and `BufferStream::seek()`, `static_cast<int64_t>(cursor_) + offset` and `static_cast<int64_t>(size_) + offset` perform signed 64-bit addition without overflow checks, causing Undefined Behavior if `offset > INT64_MAX - static_cast<int64_t>(cursor_)`.
  2. In `MappedFile::seek(offset, SeekOrigin::Begin)`, a negative `offset < 0` silently clamps `next = 0` and returns `true`, whereas `FileStream::seek()` and `BufferStream::seek()` return `false`.
* **Remediation:** Return `false` on `offset < 0` for `SeekOrigin::Begin`, and perform offset addition in unsigned `uint64_t` with explicit underflow/overflow bounds checks against `size_`.

---

#### 3. Lazy Match Evaluation at `cur_ + 1` Can Over-Estimate Match Length by 1 Byte at Filter Boundaries
* **Location:** [`src/compress/compressor50.cpp:1845-1858, 2004-2017`](../src/compress/compressor50.cpp#L2004-L2017)
* **Finding:** When `active_filter_ != FilterType::None` and `filter_emitted_until_ > cur_`, `cur_max_lz` is clamped to `rem = filter_emitted_until_ - cur_` (the remaining bytes in the current filter region starting at `cur_`). In the lazy match check at `cur_ + 1` (line 2009), `next.length` is clamped to `cur_max_lz` (`rem`) instead of `rem - 1` (`filter_emitted_until_ - (cur_ + 1)`). This allows `next` to appear 1 byte longer than it can legally be emitted at `cur_ + 1`, causing the lazy test `next.length - next_inc > len - curr_inc + 1` to emit a literal at `cur_` for zero net gain.
* **Remediation:** Clamp `next.length` in the lazy evaluation to `active_filter_ != FilterType::None && filter_emitted_until_ > cur_ + 1 ? std::min<size_t>(cur_max_lz, static_cast<size_t>(filter_emitted_until_ - (cur_ + 1))) : cur_max_lz`.

---

### `[LOW]`

#### 1. Non-Defensive 32-Bit Rotate Helpers (`rotr` / `rotr32`) on `n == 0`
* **Location:** [`src/crypto/sha256.cpp:40-42`](../src/crypto/sha256.cpp#L40-L42), [`src/crypto/blake2sp.cpp:9-11`](../src/crypto/blake2sp.cpp#L9-L11)
* **Finding:** `return (x >> n) | (x << (32 - n));` is only safe for `1 <= n <= 31` (which holds for all current call sites, but invokes shift-by-32 UB if ever called with `n == 0`).
* **Remediation:** Use the branchless UB-free rotate idiom `return (x >> (n & 31)) | (x << ((-n) & 31));`.

---

**Phase 2.2 complete.** Halting here—awaiting your review and explicit approval to proceed to **Phase 2.3: Hot Paths, Zero-Allocation & SIMD Optimisations**.