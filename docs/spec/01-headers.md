# RAR5 Specification — Headers

All header layouts build on the common framing in `00-overview.md`. All multi-byte integers are little-endian; `vint` where noted.

---

## Main Archive Header (type `1`)

| Field | Size | Description |
|-------|------|-------------|
| Archive flags | `vint` | `MHFL_*` |
| Volume number | `vint` | If `MHFL_VOLNUMBER` set. Omitted for first volume. |
| Extra area | `...` | If `HFL_EXTRA` set. |

`MHFL_*`:

| Bit | Name | Meaning |
|-----|------|---------|
| `0x0001` | `MHFL_VOLUME` | Part of a multivolume set. |
| `0x0002` | `MHFL_VOLNUMBER` | Volume number field present (all volumes except first). |
| `0x0004` | `MHFL_SOLID` | Solid archive — dictionary persists across files. |
| `0x0008` | `MHFL_PROTECT` | Recovery record present. |
| `0x0010` | `MHFL_LOCK` | Locked (no further modifications). |

### Main extra area

| Type | Name | Description |
|------|------|-------------|
| `0x01` | Locator | Offsets of service blocks for random access. |
| `0x02` | Metadata | Original archive name + creation time. |

**Locator (`0x01`):**

| Field | Size | Description |
|-------|------|-------------|
| Flags | `vint` | `0x0001` quick-open offset present, `0x0002` recovery offset present. |
| Quick-open offset | `vint` | If `0x0001` set. Distance from the start of the main archive header to the start of the quick-open service header. `0` = ignore (preallocation insufficient). |
| Recovery offset | `vint` | If `0x0002` set. Distance from the start of the main archive header to the recovery record service header. `0` = ignore. |

**Metadata (`0x02`):**

| Field | Size | Description |
|-------|------|-------------|
| Flags | `vint` | `0x0001` name present, `0x0002` creation time present, `0x0004` Unix time if set else Windows `FILETIME`, `0x0008` Unix nanoseconds (`1`) vs seconds (`0`). |
| Name length | `vint` | If `0x0001` set. |
| Name | bytes | UTF-8, `Name length` bytes. Trailing zeros are padding — truncate at first `0x00`. If first byte is `0x00`, no name (buffer too small). |
| Time | 4 or 8 bytes | If `0x0002` set. `FILETIME` (8) or Unix `uint32` / `uint64` nanoseconds per `0x0004`/`0x0008`. |

---

## File Header (type `2`) and Service Header (type `3`)

Shared structure; `2`=file, `3`=service. Service headers that belong to the preceding file set `HFL_CHILD`.

| Field | Size | Description |
|-------|------|-------------|
| File flags | `vint` | `FHFL_*` |
| Unpacked size | `vint` | Unpacked size of file/service data. Ignored if `FHFL_UNPUNKNOWN` set — decompress until stream end (used for stdin → multivolume). |
| Attributes | `vint` | OS-specific file attributes (file only; service may be `0`). |
| mtime (Unix) | `uint32` | If `FHFL_UTIME` set. Seconds since epoch. Also the `mtime` base for high-precision extra. |
| Data CRC32 | `uint32` | If `FHFL_CRC32` set. See “CRC semantics” below. |
| Compression information | `vint` | See below. |
| Host OS | `vint` | `0` Windows, `1` Unix. Affects attribute interpretation and path separator rules. |
| Name length | `vint` | |
| Name | bytes | UTF-8, no NUL. `/` is the separator on both OSes; `\` is part of name on Unix. Service names are identifiers: `CMT`, `QO`, `ACL`, `STM`, `RR`. |
| Extra area | `...` | If `HFL_EXTRA`. |
| Data area | `...` | If `HFL_DATA`. Packed size is `Data size` from common framing. |

`FHFL_*`:

| Bit | Meaning |
|-----|---------|
| `0x0001` | Directory (file only). |
| `0x0002` | `mtime` field present (Unix `uint32`). |
| `0x0004` | `Data CRC32` present. |
| `0x0008` | Unpacked size unknown (ignore `Unpacked size` field). |

**CRC semantics:** For non-split files, `Data CRC32` is the IEEE CRC32 of the unpacked data (`0xFFFFFFFF` init, final xor). For split files, all volumes except the last store the CRC32 of the *packed* volume part; the last volume stores the unpacked CRC32. With `FHEXTRA_CRYPT` flag `0x0002` (`HASHMAC`), the CRC is tweaked with the file key — see `docs/encryption-spec.md`.

### Compression Information (`vint`)

Bit-packed:

| Bits | Mask | Meaning |
|------|------|---------|
| 0–5 | `0x003F` | Algorithm version `0..63`. `0`=RAR5 (RAR 5.0+), `1`=RAR7 (RAR 7.0+, 1 TB dict). Decoders must reject unknown versions. |
| 6 | `0x0040` | `FCI_SOLID` — dictionary continues from previous file. Only for file headers. |
| 7–9 | `0x0380` | Method `0..7`. `0`=store, `1..5`=compressed. `6,7` reserved. |
| 10–14 | `0x7C00` | Dictionary size `N`: `128 KiB << N`. `0=128K`,…, `15=4096 MiB`. For version `0` the max is `N=15`. |
| 15–19 | `0xF8000` | (version `1` only) Fraction `F` in units of 1/32 of the `N` step. Effective dict = `128 KiB << N` × `(32+F)/32`. Allows 31 intermediate sizes per power of two. |
| 20 | `0x100000` | (version `1` only) `FCI_RAR5_COMPAT` — dict is version `1` sized but stream is version `0` algorithm. Used when appending large-dict version `1` files to a solid version `0` stream without recompressing prior data. |

Example: `N=4, F=0` → `128K<<4 = 2 MiB`. `N=4, F=16` → `2 MiB × 48/32 = 3 MiB`.

Decoder required dictionary is `Host` dict size; if the decoder cannot allocate that much, it must fail with “dictionary too large” rather than silently truncating.

### File / Service Extra Records

| Type | Name | Payload |
|------|------|---------|
| `0x01` | File encryption | Version, flags, KDF count (1 byte, log2 iterations; reject `>=25`), 16-byte salt, 16-byte IV, 12-byte check (if `0x0001` flag). Flags: `0x0001` check present, `0x0002` HASHMAC (tweaked CRC). An all-zero 8-byte check field means no explicit password check is stored; verify via the data checksum instead. Passwords longer than 127 bytes are truncated before derivation. See `docs/encryption-spec.md`. |
| `0x02` | File hash | Hash type `vint` (`0x00` = BLAKE2sp) + 32-byte digest. See “CRC semantics”. |
| `0x03` | File time | High-precision times. Flags `vint`: `0x0001` Unix `time_t` else `FILETIME`, `0x0002` mtime, `0x0004` ctime, `0x0008` atime, `0x0010` nanoseconds. Followed by `mtime/ctime/atime` (`uint32` seconds or `uint64` `FILETIME` per `0x0001`) and then, when `0x0001` and `0x0010` are both set, one `uint32` nanosecond remainder per present stamp in mtime/ctime/atime order (not interleaved). Each remainder is masked with `0x3FFFFFFF`; values `>=1000000000` are ignored (kept at one-second resolution, never clamped or carried). Without `0x0001`, `0x0010` is ignored. |
| `0x04` | File version | Flags `vint` (`0`), version number `vint`. From `-ver`. |
| `0x05` | Redirection | Type `vint` (`1` Unix symlink, `2` Windows symlink, `3` junction, `4` hard link, `5` file copy), flags `vint` (`0x0001` target is dir), name length `vint`, UTF-8 target. |
| `0x06` | Unix owner | Flags `vint` (`0x0001` user name, `0x0002` group name, `0x0004` numeric UID, `0x0008` numeric GID), then lengths + native-encoded names and/or `vint` IDs per flag. Name lengths are capped at 255 bytes on read. |
| `0x07` | Service data | Opaque per service type. For `ACL`/`STM` etc., the data area holds the NTFS ACL/stream bytes; this extra is a descriptor array. A single trailing byte left over at the end of the extra area is folded into a final `0x07` record. |

---

## Archive Encryption Header (type `4`)

Present only when headers are encrypted. All subsequent headers are AES-256-CBC encrypted, each starting with a 16-byte IV, padded to 16-byte boundary.

| Field | Size | Description |
|-------|------|-------------|
| Encryption version | `vint` | `0` = AES-256. |
| Encryption flags | `vint` | `0x0001` = check value present. |
| KDF count | `byte` | `log2` of PBKDF2-HMAC-SHA256 iterations. Reject if above version-dependent threshold (≈24). |
| Salt | `16` | Global header salt. |
| Check value | `12` | If `0x0001` set. 8 bytes via extra PBKDF2 rounds + 4-byte checksum (see encryption spec). |

---

## End of Archive Header (type `5`)

| Field | Size | Description |
|-------|------|-------------|
| End flags | `vint` | `0x0001` = not the last volume (`HFL_SPLITAFTER` on prior data block). |

Bytes after this header are ignored (signatures). Writers must not emit data after it except for externally appended signatures.

---

## Service Headers

Service headers are file-header-shaped blocks with `HFL_CHILD` and identifier names.

### Archive Comment (`CMT`)

* Placed after main header, before any file. Name `CMT`. Uncompressed (`method 0`), `PackSize == UnpSize` is the UTF-8 comment length. Data area holds the comment. Not compressed even if archive method is compressed.

### Quick Open (`QO`)

* Placed after all files, before `RR` and end header. Name `QO`. Uncompressed. Data area is an array of cache structures:

| Field | Size | Description |
|-------|------|-------------|
| Structure CRC32 | `uint32` | CRC32 of structure from `Structure size` onward. |
| Structure size | `vint` | Size from `Flags` onward (max 3 bytes, 2 MiB). |
| Flags | `vint` | `0`. |
| Offset | `vint` | Distance from start of `QO` header to start of cached archive bytes. |
| Data size | `vint` | Bytes of archive data cached. |
| Data | bytes | Copy of archive bytes (typically file/service headers). |

The locator’s quick-open offset points to this header. Listing may be satisfied from `QO` alone, but extraction must use the same source for name lookup and data — never mix `QO` names with real data (security).

### ACL / Streams (`ACL`, `STM`, …)

* Each is a child of the preceding file. Name is the service type; data area holds the NTFS ACL (`ACL`) or alternate stream (`STM`) bytes. The file’s extra `0x07` service-data record indexes them.

---

## Implementation Notes

* All `vint`-dimensioned counts must be validated against `Header size` / `Data size` before allocation — a `vint` of `0xFFFFFFFF` does not imply that many bytes are present.
* For solid archives, `FCI_SOLID` is set on every file after the first; the dictionary and `OldDist` (last 4 distances) persist. A non-solid file resets them.
* `Host OS` affects path canonicalisation and attribute bits but not the wire format.
