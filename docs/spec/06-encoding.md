# RAR5 Specification — Encoding, Identifiers, and Checksums

This file collects the low-level encodings and identifiers referenced by the other spec files. Implementations must match these exactly to pass the `UnRAR` cross-decode gate.

---

## vints (Review)

* 7-bit-per-byte little-endian base-128, continuation in bit 7. See `00-overview.md`.
* Canonical encoding uses the minimal byte count; non-canonical (over-long, e.g. `0x80 0x01` for `1`) must be accepted and decoded to the same value.
* Padded fixed-width vints (`PushVintFixed`) force exactly `Width` bytes by encoding the value LSB-first then appending trailing high-order `0x80` groups (value `0` with continuation) and a final `0x00` terminator. Used for locator’s 10-byte quick-open/recovery offsets (`MHEXTRA_LOCATOR` `0x01 QO / 0x02 RR`, `0` means absent/ignore) and for main/RR header CRC patching.

Example: `300` (`0x12C`) → `0xAC 0x02` (`0x2C` with continue `0x80`, then `0x02`). `0` with `Width=10` → nine `0x80` then `0x00`. `Header size` vint in the current profile must be ≤ 2 MiB (≤3 bytes); readers should enforce this before allocating `body`.

---

## Little-Endian Integers

* `uint16`, `uint32`, `uint64`, and raw vectors are little-endian.
* `RawGet4` / `RawPut4` in filter code (`04-filters.md`) are `LE32` loads/stores.
* `CRC32` bytes in headers are `LE32`; `BLAKE2sp` digest is verbatim (no endian swap).

---

## Header CRC32

* IEEE CRC32 (`poly 0xEDB88320`), init `0xFFFFFFFF`, final xor `0xFFFFFFFF`, reflected input and output.
* Covers header bytes from `Header size` (`vint`) inclusive through end of extra area (if any). Excludes the leading `4`-byte `Header CRC32` field itself and excludes the data area.
* Writers must compute over the exact emitted byte sequence, including padding `0x80` bytes in vints. Readers must recompute identically; mismatch is a hard header error (skip volume if `HFL_SPLITBEFORE`).

---

## Data CRC32 and BLAKE2sp

* **File header `Data CRC32`** (`FHFL_CRC32`): IEEE CRC32 of the unpacked file bytes (full file, not per-volume), except for split-file intermediate volumes where it is the CRC32 of the *packed* volume part (so the last volume carries the true unpacked CRC). For zero-length files the CRC is `0` if present, or the field may be absent.

* **BLAKE2sp** (`FHEXTRA_HASH` type `0x00`): 32-byte BLAKE2sp. Implementation is 8-way parallel BLAKE2s (`blake2s` per 8 lanes, then salted final). For split files, `BLAKE2sp` in the file hash extra of intermediate volumes is the hash of the packed part; the final volume’s extra holds the unpacked hash. Writers store both CRC32 and BLAKE2sp when `HASH_BLAKE2` is selected (default for non-encrypted files); `HASH_CRC32` alone stores only the header CRC field.

* **HASHMAC tweak** (`FHEXTRA_CRYPT` flag `0x0002`): when set, `Data CRC32` and the BLAKE2sp digest are *tweaked* with the file key (derived from password, salt, IV). The tweak makes the checksum key-dependent so the plaintext cannot be guessed from the checksum. Decoders must derive the file key before verifying (the header CRC32 itself is not tweaked).

---

## Block Header Checksum (Compression Stream)

Each compressed block header (`03-compression-m1-m5.md`) carries a `CheckSum` byte:

```
CheckSum = 0x5A ^ Flags ^ BlockSize ^ (BlockSize>>8) ^ (BlockSize>>16)  (low 8 bits)
```

`Flags` is the `1`-byte flags, `BlockSize` is the `1..3`-byte little-endian byte count. A mismatch is a hard block error; the decoder must stop and report `corrupt`. The block’s `BlockBitSize` (`Flags & 0x07` +1) is not checksummed except indirectly via `Flags`.

---

## Block Header Fields (Bit-Level)

* `Flags` bits: `7` (`0x80`) table present, `6` (`0x40`) last block, `3..4` `(ByteCount-1)`, `0..2` `(BlockBitSize-1)`. `ByteCount==4` (`bits 3..4 == 3`) is invalid.
* `BlockSize` is little-endian per byte: `sum GetBits(8) << (8*i)` for `i=0..ByteCount-1`. Max `0xFFFFFF` (16 MiB); larger streams use multiple blocks.
* `BlockBitSize` `1..8` is the count of valid bits in the last payload byte; remaining low bits are zero padding and must be ignored. After `BlockBitSize` bits, the next block header is byte-aligned (skip `8-BlockBitSize` bits).
* Bitstream order: MSB-first. `GetBits(n)` returns the next `n` bits from the byte stream, MSB first. `PeekBits(n)` does not consume. `AlignByte` skips to the next byte boundary after a block. Implementations may buffer up to 64 bits (`Acc`/`AccBits`).

---

## Identifiers

### Block Types

| Value | Name | Where |
|-------|------|-------|
| `1` | Main | Archive start |
| `2` | File | File payload |
| `3` | Service | Service payload (`HFL_CHILD` if attached) |
| `4` | Encryption | Header encryption (AES-256, before main) |
| `5` | End | Archive end |

### Main Flags

`MHFL_VOLUME 0x0001`, `MHFL_VOLNUMBER 0x0002`, `MHFL_SOLID 0x0004`, `MHFL_PROTECT 0x0008`, `MHFL_LOCK 0x0010`.

### File Flags

`FHFL_DIRECTORY 0x0001`, `FHFL_UTIME 0x0002`, `FHFL_CRC32 0x0004`, `FHFL_UNPUNKNOWN 0x0008`.

### Compression Information (see `01-headers.md`)

* `FCI_ALGO 0x003F` (6 bits, version `0`=RAR5, `1`=RAR7)
* `FCI_SOLID 0x0040`
* `FCI_METHOD 0x0380` (`0`=store, `1..5`=methods)
* `FCI_DICT 0x7C00` (`N`, `128K<<N`)
* `FCI_DICT_FRACT 0xF8000` (version `1` fraction `F`, `1/32`)
* `FCI_RAR5_COMPAT 0x100000` (version `1` dict with version `0` stream)

### Service Names

`CMT` (comment), `QO` (quick open), `ACL` (NTFS ACL), `STM` (NTFS stream), `RR` (recovery), plus vendor service names.

### Extra Types

Main extra: `0x01` Locator, `0x02` Metadata.  
File/service extra: `0x01` encryption, `0x02` hash, `0x03` time, `0x04` version, `0x05` redirection, `0x06` owner, `0x07` service data.

### Filter Types

`0=DELTA`, `1=E8`, `2=E8E9`, `3=ARM`, `4=AUDIO`, `5=RGB`, `6=ITANIUM`, `7=TEXT`. Only `0..3` are currently emitted.

### Huffman Alphabets (see `03-compression-m1-m5.md`)

`NC=306` (`LD`), `DCB=64` / `DCX=80` (`DD`), `LDC=16` (`LDD`), `RC=44` (`RD`), `BC=20` (`BD`). `TABLE_SIZE=430` (RAR5) / `TABLE_SIZEX=446` (RAR7). `MAX_LZ_MATCH=0x1001` (4097), `MAX_INC_LZ_MATCH=0x1004` (4100) for pathological distance-increment case.

---

## Dictionary Size Encoding

`N` in `FCI_DICT` maps to `128 KiB << N` (`N=0` 128 KiB … `N=15` 4096 MiB, version `0`). Version `1` adds `N=16..23` up to `1 TB` (`128K<<23 = 1 TB`). Fraction `F` (`0..31`) refines between powers of two: `Effective = (128K<<N) * (32+F)/32`. Writers choose the smallest `N,F` that covers the requested `-md` size (e.g., `-md2m` → `N=4,F=0`; `-md3m` → `N=4,F=16`). Readers reconstruct via `128K<<N` plus fraction and allocate exactly that.

Host `WinSize` in the decoder must be the effective dict size rounded up to a power of two for circular wrapping (`MaxWinSize` is always a power of two).

---

## Checksums Summary

| Layer | Algorithm | Covers | Where |
|-------|-----------|--------|-------|
| Header CRC32 | IEEE CRC32 | `Header size` vint bytes + `Header body` (through extra area), excl. leading 4-byte CRC and excl. data area, including any trailing `0x80` padding in vints | Every block’s first 4 bytes (IEEE, init `0xFFFFFFFF`, final xor) |
| Block header CheckSum | `0x5A` xor | `Flags` + `BlockSize` LE bytes | Per compressed block `0x5A ^ Flags ^ BlockSize ^ (BlockSize>>8) ^ (BlockSize>>16)` |
| File Data CRC32 | IEEE CRC32 | Unpacked file (or packed part for split intermediate; final volume = unpacked) | File header `Data CRC32` + `RR` parity |
| File BLAKE2sp | BLAKE2sp | Unpacked file (or packed part) | `FHEXTRA_HASH` extra type `0x02` (32 bytes) |
| Quick-open structure CRC32 | IEEE CRC32 | Structure from `Structure size` onward | Per `QO` cache structure |
| Recovery parity | RS `GF(256)`/`GF(65536)` | 512-byte sectors from first file through end header | `RR` service header / `.rev` |

Implementations must verify at least header CRC32 before interpreting any `vint` lengths, must cap `Header size` ≤ 2 MiB, and must verify `Data CRC32` or `BLAKE2sp` before declaring extraction `OK`. Unknown block types with `HFL_SKIPIFUNKNOWN (0x0004)` must be skipped.
