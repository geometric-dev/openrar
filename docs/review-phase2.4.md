## Phase 2.4: Malformed Inputs, Truncated Streams & Bounds Recovery

---

### `[CRITICAL]`

#### 1. Integer Overflow & Unbounded Header Expansion in `RecoveryWriter::repair()` Multi-Physical Shard Reassembly
* **Location:** [`src/recovery/recovery_writer.cpp:1784-2000, 2031`](../src/recovery/recovery_writer.cpp#L1865-L1999)
* **Finding:** In `RecoveryWriter::repair()`, the multi-physical recovery record reassembly block (lines 1784–2000) executes **before** the geometry sanity cap `if (d > 200 || nr > 10u * 200) return false;` at line 2031:
  ```cpp
  core::uint32 d0 =
      core::read_le16(rr.raw_data.data() + std::get<0>(pieces[0]) + SHARD_D_FIELD);
  core::uint32 unscaled_header = d0 * 8 + 0x48;
  core::uint64 scale = (group0 + 0xFFFF) / 0x10000;
  ...
  core::uint64 scaled_header = unscaled_header * scale;
  core::uint64 canonical_total = scaled_header + group0;
  std::vector<core::byte> out_data(
      static_cast<size_t>(nr_logical) * static_cast<size_t>(canonical_total), 0);
  ```
  1. **Unchecked `size_t` multiplication overflow (Heap Buffer Overflow on 32-bit):** Unlike lines 1243, 2215, 2334, and 2562 (which use `calculate_parity_buffer_size()`), line 1966 multiplies `static_cast<size_t>(nr_logical) * static_cast<size_t>(canonical_total)` without overflow checks. Because `scaled_header = unscaled_header * scale` multiplies the on-disk piece header by `scale = ceil(group0 / 64 KiB)` and `d0` can be up to `65535` (`unscaled_header = 524,352` bytes), a crafted ~32 MiB archive with `d0 = 65535` and `scale = 512` expands `scaled_header` to ~268 MiB per shard. With `nr_logical = 16`, `nr_logical * canonical_total` wraps 32-bit `size_t` at line 1966, allocating a small `out_data` buffer that the loop at lines 1967–1996 overwrites out of bounds.
  2. **Uncaught `std::bad_alloc` & DoS on 64-bit:** On 64-bit targets, the same crafted input attempts a multi-gigabyte allocation inside `RecoveryWriter::repair()` (which has no `try/catch` block), terminating the CLI or throwing across internal callers before line 2031 (`d > 200`) is ever reached.
* **Remediation:** Enforce `d0 > 0 && d0 <= 200 && nr_logical <= 2000` before entering the `multiphysical` reassembly block, and validate `nr_logical * canonical_total` through `calculate_parity_buffer_size(nr_logical, canonical_total)` before allocating `out_data`.

---

#### 2. Unsigned 64-Bit Addition Wrap in `ZipReader` Bypasses Pre-Flight Extent Bounds Checks & Causes Mis-Parsed ZIP64 Headers
* **Location:** [`src/archive/foreign_zip.cpp:121, 151-183, 236-239, 409-410, 429-433, 480-481`](../src/archive/foreign_zip.cpp#L151-L183)
* **Finding:**
  1. **Addition wrap on `e.data_offset + e.packed_size` (line 430) and `eocd64_off + EOCD64_FIXED` (line 121):** In `ZipReader::preflight_cd_vs_lfh()`, `e.packed_size` can be any 64-bit value from a ZIP64 extra record. Line 430 checks:
     ```cpp
     e.data_offset = e.lfh_offset + LFH_FIXED + lfh_name_len + lfh_extra_len;
     if (e.data_offset + e.packed_size > file_.size())
     ```
     If a crafted ZIP64 entry sets `e.packed_size >= UINT64_MAX - e.data_offset + 1`, the sum `e.data_offset + e.packed_size` wraps modulo $2^{64}$ to a value $\le \text{file\_.size()}$, bypassing the pre-flight gate. In `decode()`, `remaining = e.packed_size - (file_.tell() - e.data_offset)` is then `~UINT64_MAX`, reading past the member into subsequent archive structures. Similarly, `eocd64_off + EOCD64_FIXED > fsize` at line 121 wraps when `eocd64_off` is near `UINT64_MAX`.
  2. **Broken ZIP64 extra-field parser (`parse_zip64_extra`, lines 151–183):**
     * Line 175 returns `true` unconditionally as soon as tag `0x0001` is found, **even if `left` was too small to satisfy `need_sizes` or `need_offset`** (leaving `0xFFFFFFFF` placeholder values in `e.unpacked_size`, `e.packed_size`, or `e.lfh_offset`).
     * Per PKWARE APPNOTE §4.5.3, the ZIP64 `0x0001` record contains the 8-byte uncompressed size *iff* the 32-bit uncompressed field is `0xFFFFFFFF`, and the 8-byte compressed size *iff* the 32-bit compressed field is `0xFFFFFFFF`. `parse_zip64_extra()` conflates both into a single `bool need_sizes = (unp32 == 0xFFFFFFFFu)` requiring `left >= 16`. For any valid file $> 4\text{ GiB}$ uncompressed that compresses to $< 4\text{ GiB}$ (`unp32 == 0xFFFFFFFF`, `pack32 < 0xFFFFFFFF`), `parse_zip64_extra` either fails (if no 64-bit offset follows) or **misreads the 8-byte `lfh_offset` as `packed_size`**!
     * At line 409 (`preflight_cd_vs_lfh`), the return value of `parse_zip64_extra()` is ignored.
* **Remediation:** Express all extent bounds checks in subtraction form (`e.data_offset > file_.size() || e.packed_size > file_.size() - e.data_offset`), pass separate `bool need_unp, bool need_pack, bool need_offset` flags into `parse_zip64_extra()`, and return `!need_unp && !need_pack && !need_offset`.

---

#### 3. PAX `"size"` Override in `TarReader::open()` Fails to Update `padded`, Mis-Framing the Stream Into Member Data
* **Location:** [`src/archive/foreign_tar.cpp:222, 299-322, 348, 439`](../src/archive/foreign_tar.cpp#L222)
* **Finding:** In `TarReader::open()`, line 222 computes the 512-byte block-aligned payload advance from the ustar header's 12-byte octal `size` field:
  ```cpp
  const core::uint64 padded = (size + BLOCK - 1) / BLOCK * BLOCK;
  ```
  Later, at lines 299–322, if a PAX extended header (`typeflag == 'x'`) provided a `"size"` record (standard for files $> 8\text{ GiB}$ or PAX archives that zero the ustar octal size field per POSIX.1-2001), `m.size` is updated to `v`, but **`padded` is never recomputed from `m.size`**. At line 439:
  ```cpp
  offset = data_offset + padded;
  ```
  `offset` advances by the stale `padded` (`0`), causing the next iteration of `TarReader::open()` to read the member's raw file payload as the next 512-byte TAR header and abort with `ForeignStatus::Unparseable`. Additionally, line 318 (`if (data_offset + v > fsize)`) uses addition instead of subtraction (`v > fsize - data_offset`), which can wrap on large `v`.
* **Remediation:** Compute `padded` *after* `m.size` is finalized at line 348 using subtraction-bounded arithmetic (`if (m.size > fsize - data_offset) return ForeignStatus::Truncated; const core::uint64 rem = m.size % BLOCK; const core::uint64 padded = rem ? (m.size + (BLOCK - rem)) : m.size;`).

---

### `[HIGH]`

#### 1. Truncated or Malformed `FHEXTRA_CRYPT` File Extra Record Silently Uses Zero Salt/IV & Bypasses `crypt_version` Validation
* **Location:** [`src/format/header_reader.cpp:600-634`](../src/format/header_reader.cpp#L600-L634), [`src/archive/archive_reader.cpp:1538-1550, 3071-3094`](../src/archive/archive_reader.cpp#L1538-L1550)
* **Finding:**
  1. In `HeaderReader::parse_file_header()` (lines 600–634), when `rec_type == FHEXTRA_CRYPT`:
     * The return values of `core::read_vint()` for `cver` and `cflags` (lines 603, 605) are ignored.
     * If the record is truncated before `lg2_count` (1B), `salt` (16B), or `init_v` (16B) (or before `psw_check` (8B) + `psw_check_csum` (4B) when `cflags & 0x01` is set), `parse_file_header()` **does not fail**. Instead, it leaves the missing fields zero-initialized, and if `psw_check` was truncated, lines 628–630 (`if (all_zero) out_block.has_psw_check = false;`) silently disable password verification.
  2. In `ArchiveReader::derive_entry_keys()` (`archive_reader.cpp:1538-1550`) and `ArchiveReader::extract_entry_impl()` (`archive_reader.cpp:3071-3094`), `entry.header.crypt_version != 0` is **never checked** (unlike `HeaderCryptReader::init()` at `header_reader.cpp:49`), nor is `entry.header.psw_check_csum` verified against `Sha256(entry.header.psw_check)` before trusting `psw_check`.
* **Remediation:** In `parse_file_header()`, fail closed (`return false`) if `FHEXTRA_CRYPT` cannot read `cver`, `cflags`, `lg2_count`, 16-byte `salt`, 16-byte `init_v`, and (when `cflags & 0x01`) 8-byte `psw_check` + 4-byte `psw_check_csum`. In `derive_entry_keys()` and `extract_entry_impl()`, reject `entry.header.crypt_version != 0` with `RAR_ERR_UNSUPPORTED_FEATURE` and gate `psw_check` on its SHA-256 `psw_check_csum`.

---

#### 2. `RecoveryWriter::repair_rev_volumes()` Destructively Renames Corrupt Volumes to `.bad` Before Validating `.rev` Set Sufficiency
* **Location:** [`src/recovery/recovery_writer.cpp:1548-1664`](../src/recovery/recovery_writer.cpp#L1548-L1664)
* **Finding:** In `RecoveryWriter::repair_rev_volumes()`:
  1. Line 1550 counts `valid_revs` without deduplicating `r.rec_num`. If two `.rev` files in the directory carry the same `rec_num` (e.g., a backup copy of a `.rev` file), `missing <= valid_revs` passes at line 1551, and lines 1565–1572 **delete any existing `.bad` file and rename the user's corrupt volume(s) to `.bad`**.
  2. Immediately afterward, `rs.init(nd, nr, valid.data())` at line 1580 sees fewer distinct `valid[r.rec_num]` slots than `missing` and returns `false` (or opening a source/output volume at lines 1630–1663 fails and returns `false`), leaving the user's original volume renamed away with no replacement volume written.
* **Remediation:** Move the `.bad` rename step to *after* `rs.init()` succeeds and all source streams have been opened, or use a two-phase temp-file reconstruction and only rename the corrupt volume to `.bad` immediately before atomically replacing it.

---

#### 3. Unbounded Output Accumulation in `StreamDecoder` When `flush_cb_ == nullptr` (Decompression Bomb Vector on WASM/DLL)
* **Location:** [`src/compress/stream_decoder.cpp:36-49, 100-110`](../src/compress/stream_decoder.cpp#L100-L110)
* **Finding:** While `Decompressor50::decompress_to_vector()` enforces `MAX_STREAM_OUTPUT` (`2 GiB` on WASM/32-bit, `64 GiB` on 64-bit), `StreamDecoder` (exposed via `openrar_stream_decompress_feed` in `wasm_api.cpp` with `flush_cb_ == nullptr`) appends decoded chunks into `output_` without any upper bound check on `output_.size()` or `total_out_`. A small decompression bomb fed through the streaming decoder API can exhaust the WASM or host heap.
* **Remediation:** Enforce `if (output_.size() + size > Decompressor50::MAX_STREAM_OUTPUT) { aborted_ = true; return false; }` inside `StreamDecoder::feed()` (method 0) and `StreamDecoder::process_blocks()`'s `sink` lambda when `flush_cb_ == nullptr`.

---

### `[MEDIUM]`

#### 1. Unvalidated `HFL_EXTRA` `extra_size` Overrun in `HeaderReader::parse_main_header()` & `parse_file_header()`
* **Location:** [`src/format/header_reader.cpp:28-33, 273-279, 325-331, 565-571`](../src/format/header_reader.cpp#L325-L331)
* **Finding:** When `flags & HFL_EXTRA` is set, `parse_main_header()` and `parse_file_header()` compute:
  ```cpp
  if (flags & HFL_EXTRA && extra_size > 0 && offset < body_size) {
      size_t extra_start = offset;
      if (extra_size <= body_size) {
          size_t from_end = body_size - static_cast<size_t>(extra_size);
          if (from_end > offset) extra_start = from_end;
      }
      offset = extra_start;
  ```
  If a malformed header declares `extra_size > body_size - offset` (i.e., the declared extra area overlaps the fixed header fields or exceeds `body_size`), the parser silently falls back to `extra_start = offset` instead of rejecting the malformed header with `return false`.
* **Remediation:** Reject headers where `(flags & HFL_EXTRA) != 0` and `extra_size > body_size - offset` by returning `false`.

---

#### 2. `ArchiveReader::scan_archive()` Silently Admits Truncated File Extents on Non-Final / Single Volumes Without Failing the Header Walk
* **Location:** [`src/archive/archive_reader.cpp:1186-1195, 1238-1244`](../src/archive/archive_reader.cpp#L1238-L1244)
* **Finding:** During `ArchiveReader::scan_archive()`, when a `HEAD_FILE` or `HEAD_SERVICE` block declares a `data_size` that extends past the end of the volume (`data_size > avail`), lines 1239–1243 clamp `admit = avail`, push the truncated `ArchiveEntry` into `raw_out` (`entries_`), and `break` out of the volume scan loop, returning `true` from `process_volume()` and `scan_archive()`. As a result, `open()` / `open_ex()` succeeds on a truncated archive and lists the truncated entry with `packed_size = admit < header.pack_size`, deferring failure until extraction time (whereas `BufferArchive::list()` at [`buffer_archive.cpp:262, 507`](../src/archive/buffer_archive.cpp#L262) immediately returns `RAR_ERR_TRUNCATED`).
* **Remediation:** Record a truncation flag when `data_size > avail` (or when `HEAD_ENDARC` is missing under `strict_volumes`), so `open_ex()` can fail fast on truncated archives consistently with `BufferArchive::list()`.

---

#### 3. Silent Coercion of Invalid Octal / Base-256 `mode`, `uid`, `gid`, and `mtime` Fields in `TarReader::open()`
* **Location:** [`src/archive/foreign_tar.cpp:180-194, 294-345, 394-398`](../src/archive/foreign_tar.cpp#L180-L194)
* **Finding:**
  1. In `TarReader::open()`, `parse_tar_num()` returns `false` on empty or all-NUL/space fields (`return any;`). While empty `uid`/`gid`/`mtime` fields legitimately default to `0`, a field containing non-octal garbage characters also returns `false` and is silently coerced to `0`.
  2. In the PAX `uid`/`gid` parser (lines 323–335), non-digit characters `break` out of the loop without error and without an overflow guard on `pax_uid = pax_uid * 10 + (c - '0')`.
* **Remediation:** In `parse_tar_num()`, distinguish between an all-space/NUL empty field (which is valid and evaluates to `0`) and a field containing invalid characters (which should fail), and add an overflow guard (`pax_uid > UINT64_MAX / 10`) in the PAX `uid`/`gid` loops.

---

### `[LOW]`

#### 1. Misleading Comment on `SHARD_MAGIC` in `RecoveryWriter::find_rr()`
* **Location:** [`src/recovery/recovery_writer.cpp:392`](../src/recovery/recovery_writer.cpp#L392)
* **Finding:** Line 392 comments `// Fallback 2: Search backwards for SHARD_MAGIC ("RAR_RR\0\0")`, whereas `SHARD_MAGIC` is actually the 4-byte sequence `{'{', 'R', 'B', '}'}` (`0x7B 0x52 0x42 0x7D`, line 131).
* **Remediation:** Update the comment to match `SHARD_MAGIC` (`"{RB}"`).
