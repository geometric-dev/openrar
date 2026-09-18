# OpenRAR Extraction Contract

This document is the **authoritative specification** of observable extraction behaviour for
`ArchiveReader`'s streaming API. All extraction entry points in the DLL, WASM, and native
C++ layers are bound by this contract.

---

## 1. Terminology

| Term | Definition |
|---|---|
| **member** | A single logical file, directory, or link entry in a RAR5 archive. |
| **chunk** | A unit of output data processed atomically. For stored/decrypted: <= 64 KiB (`kStreamChunk`). For compressed: one decompressor output flush (typically one dictionary-window boundary). |
| **solid chain** | A sequence of compressed entries that share LZ dictionary and Huffman state across entry boundaries. |
| **catch-up** | Decoding and discarding solid chain entries before the target entry, to restore the decompressor to the correct window state. |

---

## 2. API Entry Points Covered

| Function | Surface |
|---|---|
| `ArchiveReader::extract_entry_stream` | C++ / DLL file-mode handles |
| `ArchiveReader::extract_entry_to_memory` | C++ / DLL preview / WASM |
| `ArchiveReader::test_entry_stream` | C++ / DLL |
| `ArchiveReader::open_ex` | C++ / DLL (header scan phase) |
| DLL `openrar_archive_extract_to_path` | C ABI |
| DLL `openrar_archive_extract` | C ABI |
| DLL `openrar_archive_test` | C ABI |

---

## 3. Cancel and Progress Callback Granularity

### Progress (`ReaderHooks::progress`)

- Called with `(done, total)` where `done` is cumulative and monotonically non-decreasing.
- `total` is the announced uncompressed size of the current member from the file header.
- When `FHFL_UNPUNKNOWN` is set (`total == 0` from the header), `total` is passed as 0 for the
  duration of the member; `done` still advances per chunk.
- Emitted **at least once per chunk** during payload streaming.
- Emitted **between header blocks** during `open_ex` (byte semantics: offset in the stream).
- **Not emitted** during catch-up decoding of earlier solid entries.

### Cancel (`ReaderHooks::cancel`)

Polled:
- Once per chunk during stored/decrypt payload streaming (<= 64 KiB granularity).
- Once per decompressor output flush during compressed payload streaming.
- Once per solid catch-up entry (after each fully decoded discard entry).
- Once per header block during `open_ex`.

**Never polled while a handle-table lock is held** (safe to re-enter DLL entry points from
the cancel callback, as long as the re-entering call does not close the same handle).

A non-zero return from `cancel` causes the operation to return `RAR_ERR_ABORTED`.

---

## 4. Error Code Semantics Per Exit Path

### `RAR_OK` (0)
Full success. The member's BLAKE2sp or CRC32 checksum has been verified and matched.
`last_decoded_index_` advances to the current entry (solid chain position is valid).

### `RAR_ERR_CRC_MISMATCH` (-4)
The member's checksum (BLAKE2sp or CRC32) failed verification after the full payload was
processed. **Partial output may exist at the destination** (see Section 6). The solid chain
position is **not advanced** (the window state is invalid; catch-up will replay from the
run head on the next call).

### `RAR_ERR_ABORTED` (-11)
The cancel callback returned non-zero. Output is partial. The solid chain position is not
advanced.

### `RAR_ERR_BAD_PASSWORD` (-7)
PBKDF2 key derivation succeeded but the PswCheck field did not match. Returned before any
decryption attempt. No output bytes are produced.

### `RAR_ERR_ENCRYPTED` (-12)
The entry is encrypted and no password was set. No output bytes are produced.

### `RAR_ERR_TRUNCATED` (-3)
The payload data ended before the declared size (or before the compressor end-of-stream
marker for FHFL_UNPUNKNOWN entries). Partial output may exist.

### `RAR_ERR_MISSING_VOLUME` (-13)
A required volume in a multi-volume set could not be opened. No output for the current
member. The scan position is at the point of the missing volume.

### `RAR_ERR_IO` (-6)
A read failure from the archive stream or a write failure to the output sink (disk full,
permission denied, etc.). Partial output may exist.

### `RAR_ERR_NOMEM` (-5)
Returned by `extract_entry_to_memory` when the declared uncompressed size exceeds
`max_bytes`. Returned **before any work** -- no output bytes produced, no solid chain
movement. Also returned when the in-flight accumulation exceeds `max_bytes` for entries
with `FHFL_UNPUNKNOWN`.

### `RAR_ERR_LIMIT_EXCEEDED` (-15)
A resource limit imposed by `ExtractionLimits` was breached (see Section 7). Returned
immediately upon breach; no retry. Partial output may exist for disk extractions.

### `RAR_ERR_UNSUPPORTED_FEATURE` (-2)
The entry uses a feature the implementation does not support (e.g. an unknown encryption
version, or a compression algorithm version > 1).

---

## 5. Solid Chain Guarantee

`ArchiveReader` maintains `last_decoded_index_` (signed, -1 = none) to track the solid
chain position:

- Advances to `entry_index` only on **complete, verified success** (checksum matches).
- Any failure or cancel leaves it unchanged so a retry can re-run catch-up cleanly.
- **Catch-up**: if the next entry to extract is not the immediate successor of the current
  solid position, `ensure_solid_position` decodes and discards intervening entries until
  the window state is correct. Each discarded entry is fully verified before proceeding.
- **Random access in a solid archive** is O(N) in the number of intervening entries.
  Callers should extract in forward order where possible.

---

## 6. Partial Output and Disk Safety

### Checksum Verification Ordering

BLAKE2sp/CRC32 verification occurs **after the full payload has been written** to the
output sink (in `stream_payload`). This means:

> **Corrupted or maliciously modified data may be written to the output sink
> before `RAR_ERR_CRC_MISMATCH` is returned.**

#### DLL file extraction path (`extract_to_path`)

Protected by `FileUnlinker` RAII: output is written to a temporary file. On any non-OK
return, the temporary file is deleted before the function returns. The destination path
is never left in a partially-written state. This protection is armed after the output file
is successfully opened.

#### Direct streaming path (`extract_entry_stream`)

**The caller is responsible for disk safety.** The recommended pattern:

```
1. Open a temporary file in the same filesystem volume as the destination.
2. Call extract_entry_stream -> temp file.
3. On RAR_OK: atomically rename temp -> destination.
4. On any error: delete the temp file.
```

#### Memory extraction (`extract_entry_to_memory`)

No durable write occurs. On any non-OK return, the `out` vector contains partial or corrupt
bytes. The caller must discard it entirely on non-OK returns. Because the vector is the
caller's, no cleanup is required by the library.

---

## 7. Resource Limits

`ExtractionLimits` imposes hard caps. All limits use `uint64_t` with `UINT64_MAX` as the
"unlimited" sentinel (the zero-overhead default).

### Per-Member Output Cap (`max_member_output_bytes`)

Checked **in-flight, per chunk**. Fires even when `FHFL_UNPUNKNOWN` is set (the header's
`unp_size` is zero but output bytes accumulate). Returns `RAR_ERR_LIMIT_EXCEEDED` as soon
as the cap is exceeded. The member accumulator is reset at the start of each member.

### Total Output Cap (`max_total_output_bytes`)

**Cumulative across all members in the entire extraction call**, including across volumes.
The running total is owned by the caller's `LimitState` and is never reset. Returns
`RAR_ERR_LIMIT_EXCEEDED` as soon as the cumulative total is exceeded.

### Header Count / Header Bytes (`max_header_count`, `max_header_bytes`)

Applied during the `scan_archive` phase (not during payload extraction). Counters are
**cumulative across all volumes in a multi-volume set** -- they do not reset between
volumes. Returns `RAR_ERR_LIMIT_EXCEEDED` when either cap is exceeded mid-scan.

### Limit Failure State

On `RAR_ERR_LIMIT_EXCEEDED`:
- The thread-local error message contains the limit name, current value, and cap value.
- Partial output may exist for disk extractions (see Section 6).
- The solid chain position is not advanced.
- No retry is implied -- the caller must decide whether to continue with remaining entries.

---

## 8. Multi-Volume Archives

- `open_ex` with `strict_volumes = true` fails at open time if any required volume is
  missing. With `strict_volumes = false` (the CLI's default), scanning stops at the
  first missing volume without error.
- `extract_entry_stream` fails with `RAR_ERR_MISSING_VOLUME` if a payload extent resides
  in a volume that cannot be opened.
- Volume path tracking is available via `missing_volume_path()` after a failure.

---

## 9. Filename Encoding

RAR5 mandates UTF-8 for all `file_name` fields (spec Section "File header and service header":
"Name -- UTF-8 without trailing zero"). The `file_name` bytes are stored directly into
`FileBlock::file_name` without re-encoding. The parser validates UTF-8 well-formedness (RFC 3629); archives with malformed
UTF-8 filenames are rejected as invalid headers. Valid UTF-8 filenames pass through directly.

### DLL/WASM Boundary

The DLL and WASM path buffers transmit valid UTF-8 filename bytes. Because validation occurs
at header parse time, callers are guaranteed to receive well-formed UTF-8.

### Symlink Targets

The `redir_target` field (FHEXTRA_REDIR) is stored and transmitted as raw bytes.
No encoding transformation is applied to symlink targets.

---

## 10. Thread Safety

- Each `ArchiveReader` instance is **not thread-safe** -- calls must be externally serialized.
- Multiple `ArchiveReader` instances on separate threads are independent (no shared mutable state).
- DLL handle-table operations (`pin`, `insert`, `erase`) are mutex-protected and safe to call
  concurrently from multiple threads. Callbacks are invoked with the table lock released.
