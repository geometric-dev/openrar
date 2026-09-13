# RAR5 Specification — Overview

This directory documents the RAR 5.0 / RAR 7 clean-room format: every element needed to write a compliant encoder or decoder without reading the reference implementation. No file names, routines, or project structure are included — only the wire format and algorithms.

See also `RAR5-FORMAT.md` for the original block-layout reference.

## Contents

* `00-overview.md` — this file, data types, block framing, archive layout
* `01-headers.md` — main / file / service / encryption / end headers, extra areas
* `02-compression-store.md` — method `0` (store) stream
* `03-compression-m1-m5.md` — methods `1..5`, LZ, Huffman, tables, dictionary, solid
* `04-filters.md` — executable and byte filters (DELTA, E8/E8E9, ARM)
* `05-recovery.md` — recovery record (RR) and `.rev` volumes, RS codecs
* `06-encoding.md` — identifiers, vints, little-endian integers, CRCs, checksums

All integers are unsigned unless stated. `vint` is the only variable-length integer. Block and stream fields are MSB-first at the bit level unless noted; byte payloads are little-endian.

---

## Data Types

### vint

Variable-length, 7-bit-per-byte, little-endian base-128. Each byte contributes 7 data bits (bits 0–6) and a continuation bit (bit 7): `1` means another byte follows, `0` terminates. The first byte carries the 7 LSBs. Values up to 64 bits need at most 10 bytes (up to 10×7 = 70 bits, high bits zero). Encoders may emit over-long / non-canonical encodings (e.g. `0x80 0x01` for value `1`); decoders must accept them and treat them as the same value.

Padded vints (`PushVintFixed`) are emitted as exactly *Width* bytes: the value is encoded in base-128 (LSB groups first), then the remaining high-order groups are forced to `0x80` (value `0` with continuation bit set) and the final byte has `bit7=0`. On the wire this appears as `value` LSB-first followed by `Width - len(value)` trailing `0x80` bytes and a terminating byte. Example: `300` (`0x12C`) → `0xAC 0x02`; `0` with `Width=10` → nine `0x80` then `0x00`. Decoding stops at the first byte with `bit7=0`; the high-order zero groups contribute `0` to the value.

### Byte integers

`byte`, `uint16`, `uint32`, `uint64` are little-endian. `byte` arrays are verbatim. Strings (`Name`) are UTF-8 without NUL terminator, length given by a preceding `vint`; forward slash `/` is the portable path separator.

### Ellipsis

`...` in tables denotes variable-length or record-dependent data.

## General Block Framing

Every archive block shares a common prefix:

| Field | Size | Description |
|-------|------|-------------|
| Header CRC32 | `uint32` | CRC32 (IEEE, `0xFFFFFFFF` init, final xor) of header bytes from `Header size` through end of extra area (if present). Excludes the CRC field itself and the data area. |
| Header size | `vint` | Size of header from `Header type` through end of extra area. Max 3 bytes in current profile (2 MiB). |
| Header type | `vint` | `1` main, `2` file, `3` service, `4` encryption, `5` end. Unknown types with `HFL_SKIPIFUNKNOWN` must be skipped on update. |
| Header flags | `vint` | Common flags (see below). |
| Extra area size | `vint` | If `HFL_EXTRA` set. |
| Data size | `vint` | If `HFL_DATA` set. For file headers: packed size. |
| Type-specific fields | `...` | Per block type. |
| Extra area | `...` | If `HFL_EXTRA` set. One or more extra records. |
| Data area | `...` | If `HFL_DATA` set. Not covered by header CRC/size. File/service payload, may be split across volumes. |

Common header flags `HFL_*`:

| Bit | Name | Meaning |
|-----|------|---------|
| `0x0001` | `HFL_EXTRA` | Extra area present. |
| `0x0002` | `HFL_DATA` | Data area present. |
| `0x0004` | `HFL_SKIPIFUNKNOWN` | Skip unknown block types on update. |
| `0x0008` | `HFL_SPLITBEFORE` | Data continues from previous volume. |
| `0x0010` | `HFL_SPLITAFTER` | Data continues in next volume. |
| `0x0020` | `HFL_CHILD` | Block depends on preceding file block. |
| `0x0040` | `HFL_INHERITED` | Preserve child if host is modified. |

### Extra Area Framing

The extra area ends where the header ends. Locate it from the end
(`extra_start = header_end - Extra Area Size`), not by parsing forward
from the last fixed field, so future fields are skipped rather than
misparsed.

Extra area is a concatenation of records:

| Field | Size | Description |
|-------|------|-------------|
| Size | `vint` | Size from `Type` through end of record data. |
| Type | `vint` | Record type; unknown types are skipped. |
| Data | `...` | Type-dependent payload, may be empty. |

### Archive Layout

```
[SFX module, ≤4 MiB, opaque] (optional)
RAR 5.0 signature: 52 61 72 21 1A 07 01 00  (8 bytes)
[Archive encryption header] (if headers encrypted)
Main archive header
[Archive comment service header "CMT"] (optional)
File header 1
[Service headers for file 1: ACL/STM/... ] (optional, each HFL_CHILD)
...
File header N
[Service headers for file N]
[Recovery record service header "RR"] (optional, locatable via main extra locator)
End of archive header
[Trailing bytes after end header are ignored — signatures, etc.]
```

Volumes: first volume omits `MHFL_VOLNUMBER`; subsequent volumes set `HFL_SPLITBEFORE` on the first data block and `HFL_SPLITAFTER` on the last data block of the volume. Volume number (`vint`) is the 0-based index of the volume (first = 0, second = 1). `Header size` vint is at most 3 bytes in the current profile (2 MiB); readers should reject headers declaring larger sizes to avoid OOM.

Search for the signature forward from offset 0 up to `4 MiB` (`MAXSFXSIZE = 0x400000`); older docs say 1 MiB but implementations scan 4 MiB. RAR 4.x signature is 7 bytes `52 61 72 21 1A 07 00` for discrimination. Unknown block types with `HFL_SKIPIFUNKNOWN (0x0004)` must be skipped (skip `Data size` bytes) on update, not failed.

## Checksums and Hashes

*Header CRC32* — IEEE CRC32 over header bytes as above. Used to reject corrupt headers before interpreting `vint` lengths.

*Data CRC32* — IEEE CRC32 of unpacked file data (or packed volume-part for split files, except last part). Stored in file header `Data CRC32` field when `FHFL_CRC32` set, and in recovery. For encrypted files with `FHEXTRA_CRYPT_HASHMAC`, the CRC is tweaked (key-dependent) — see `01-headers.md`.

*BLAKE2sp* — 32-byte BLAKE2sp (parallel 8-way BLAKE2s) of unpacked data, stored in `FHEXTRA_HASH` (type `0x00`). For split files, intermediate volumes store the hash of the packed part.

Implementations must verify at least one of CRC32 / BLAKE2sp on extraction; recovery uses the same.

## Design Principles for Clean-Room

* Never trust `Header size` / `Data size` without CRC check. Validate `Header size` ≤ 2 MiB before allocating `body` (see `06-encoding.md`).
* Skip unknown block types and unknown extra record types — do not fail. For unknown block types, honor `HFL_SKIPIFUNKNOWN` and skip `Data size` bytes; otherwise treat as corrupt.
* `vint` overlong and padded encodings are legal; they appear in locator pre-reservation (`PushVintFixed` width 10) and quick-open patching. Locator offsets of `0` mean “absent / preallocation insufficient” and must be ignored — do not seek to offset `0`.
* All `vint` counts that dimension arrays are followed by exact byte counts; if the byte count is smaller than the `vint` count implies, the block is truncated — fail gracefully.
* Solid, quick-open, and recovery are optional to decode — ignoring them must still extract non-solid files.
* `Extra area Size` vint includes the `Type` byte: `rec_end = offset + Size`, `Type = body[offset++]`, payload `rec_end - offset`. Unknown extra types are skipped to `rec_end`.
