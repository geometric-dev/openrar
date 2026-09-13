# RAR5 Specification — Compressed Methods 1..5

Methods `1..5` share the same wire format and differ only in encoder heuristics (chain limit, nice length, lazy evaluation). Any decoder that implements one implements all — the method is a hint, not a format variant. Method `0` is store (see `02-compression-store.md`).

---

## Overview

A compressed file payload is a concatenation of **blocks**. Each block has a tiny header, an optional Huffman table, and a bitstream of tokens (literals, matches, repetitions, filters). The final block in the file sets `LastBlock`.

The decoder maintains a circular *window* (`MaxWinSize` bytes, see “Dictionary” below) and four *OldDist* registers (`OldDist[0..3]`). It also buffers decoded bytes to an output vector (or file) that is linear; filters are applied at the end.

```
File header (HFL_DATA, PackSize)
  [Block 0: header, [tables], tokens] ... [Block N: header, [tables], tokens, LastBlock]
```

`PackSize` is the exact byte length of the concatenated block headers + payloads.

---

## Dictionary

| Method | Default MaxWinSize | Range via Compression Information `N` / `F` |
|--------|-------------------|---------------------------------------------|
| `1` | 128 KiB | `128 KiB << N` × `(32+F)/32`, `N=0..15` (version 0) |
| `2` | 1 MiB | |
| `3` | 1 MiB | |
| `4` | 4 MiB | |
| `5` | 32 MiB | version `1` extends to `64 GiB` (`N=19`) / `1 TB` (`N=23`) |

The writer selects a power-of-two `MaxWinSize` between `128 KiB` and `64 MiB` (version 0) / `1 TB` (version 1) based on file size and method. The reader must allocate exactly `MaxWinSize` bytes; if it cannot, it must fail (do not truncate). The window wraps: `UnpPtr = (UnpPtr+1) % MaxWinSize`. `FirstWinDone` tracks whether the window has wrapped at least once — matches that would read from unwritten prefix emit zeros.

`MaxDist` is the maximum match distance encoded: `min(MaxWinSize, 64 MiB)` for version 0 (older profiles) and up to `MaxWinSize` for version 1. The encoder never emits a distance larger than `MaxDist`; the decoder should treat larger distances as 0 bytes.

Solid archives set `FCI_SOLID` on every file after the first; `UnpPtr`, `OldDist`, `Window` contents, and Huffman table delta persist. A non-solid file resets `UnpPtr=0`, `OldDist=[-1,-1,-1,-1]` (i.e. `0xFFFFFFFF` / `size_t(-1)` — not `0`), `LastLength=0`, `Window=0`. Implementations that reset `OldDist` to `0` will spuriously honor rep-distance `0` at file start; decoders must init to `-1` and emit zeros via `FirstWinDone` when distance exceeds written prefix.

---

## Block Header

Each block is byte-aligned before its header. The header is MSB-first at the bit level for the stream, but the header itself is byte-oriented:

| Field | Size | Description |
|-------|------|-------------|
| Flags | `1` | `0x80` = tables present, `0x40` = last block in file, `0x00..0x07` = `(BlockBitSize-1)` (1..8), bits `3..4` = `(ByteCount-1)` (1..3, `4` invalid). |
| CheckSum | `1` | `0x5A ^ Flags ^ BlockSize ^ (BlockSize>>8) ^ (BlockSize>>16)` (low byte). |
| BlockSize | `1..3` | Byte count of following compressed payload (excluding header). `1` byte if `<0x100`, `2` if `<0x10000`, `3` otherwise (max `0xFFFFFF`). Little-endian per byte. `0` is valid only as a terminator block (`LastBlock` set, no tables, no payload) — decoders must accept it and treat it as end-of-file, not an error. |
| Payload | `BlockSize` bytes | Huffman tables (if `0x80` set) followed by token bitstream. Only `BlockBitSize` bits of the last byte are valid; the remaining low bits are zero padding. |

`BlockSize==0` with `LastBlock` set and no tables is a legal empty terminator (decoder: `break`).

Byte alignment: after a block’s `BlockBitSize` valid bits, the next block header starts at the next byte boundary (skip `8-BlockBitSize` padding bits).

---

## Huffman Tables

Four literal/match tables plus a fifth for table-length coding are transmitted per block when `tables present`:

| Table | Symbols | Meaning |
|-------|---------|---------|
| `LD` | `NC=306` | `0..255` literals, `256` filter, `257` last-length repeat, `258..261` rep-distance slot, `262..305` match length slot. |
| `DD` | `DCB=64` (RAR5) / `DCX=80` (RAR7, ExtraDist) | Distance slot. |
| `LDD` | `LDC=16` | Low 4 bits of distance when `DBits>=4`. |
| `RD` | `RC=44` | Rep-match length slot (when `MainSlot 258..261`). |
| `BD` | `BC=20` | Bit-lengths of the four tables above (`LD+DD+LDD+RD`). |

### Table transmission

1. **BC bit-lengths (20 entries):** 20 nibbles (`4` bits each), MSB-first. Value `15` is an escape: it is followed by a 4-bit `ZeroCount`; if `ZeroCount==0` it means literal `15`, otherwise it means `ZeroCount+2` zero bit-lengths (`0`). This run-length compresses runs of zeros in the BC alphabet. The 20 lengths define the `BD` Huffman tree (`max_bits 15`).

2. **Main bit-lengths (430 RAR5, 446 RAR7):** Concatenation `LD (306)` + `DD (64/80)` + `LDD (16)` + `RD (44)` = 430 / 446 entries, each `0..15` (`0` = unused). They are encoded with `BD`:

   * `num < 16`: absolute value `num` (not delta). The specification requires `Table[pos]=num`. An encoder that uses delta (`(Table[pos]+num)&15`) will self-roundtrip but fail cross-decode — encoders must emit absolute. Vintage archives may use the delta variant; compliant decoders should accept absolute per spec and may optionally handle delta for backward compatibility, but encoders must emit absolute.
   * `16`: repeat previous bit-length `3+a` times, `a` = 3-bit value (3..10).
   * `17`: repeat previous bit-length `11+a` times, `a` = 7-bit value (11..138).
   * `18`: run of zeros `3+a` times, `a` = 3-bit (3..10).
   * `19`: run of zeros `11+a` times, `a` = 7-bit (11..138).

    `pos==0` with `16/17` (repeat previous at start) is invalid — decoders must reject.

    After decoding, the 430/446 bit-lengths define four Huffman trees (`max_bits 15`). `LDD` and `RD` are small alphabets; their unused symbols are zero. `TABLE_SIZE=430` (RAR5, `DC=64`) and `TABLE_SIZEX=446` (RAR7, `DC=80` when `Win>4 GiB` / `EnableExtraDist`) share the same wire; decoders should support both via table-size selection.

### Huffman code assignment

Canonical, MSB-first, increasing symbol order within equal length:

* Count `BlCount[n]` of symbols with length `n`.
* `Code=0; NextCode[n] = (Code+BlCount[n-1])<<1` for `n=1..15`.
* In symbol order (`0..Size-1`), if `Len!=0` assign `Codes[i]=NextCode[Len]++`.

Decoder builds `DecodeLen[1..15]` and `DecodePos` from `BlCount` and uses quick table (`QUICK_BITS 10`, `QUICK_SIZE 1024`) plus slow path for codes longer than 10 bits. Kraft sum may be incomplete (sparse alphabets) — decoder must accept `upper != 1<<16`.

`BC` lengths use `max_bits 15` as well, but the *encoder* must ensure no `BC` length equals `15` without escaping: an `LDD` length of `15` in the `BD` stream must be encoded as `15` (escape) + `0` (literal). Practically this means if any `LenBD[i]==15`, emit `15,0` (8 bits) not `15` alone.

---

## Token Stream

Tokens are Huffman-coded via `LD` (and `RD`/`DD`/`LDD` as secondary):

| `LD` MainSlot | Token | Bits |
|---------------|-------|------|
| `<256` | Literal `byte` | `LD` Huffman only |
| `256` | Filter | See `04-filters.md` (block start/length/type/channels) |
| `257` | Repeat last length | Re-emit `LastLength` with `OldDist[0]` (no Huffman) |
| `258..261` | Rep-match | `distNum = slot-258`, `dist=OldDist[distNum]` (rotate `OldDist`), then length slot via `RD` |
| `262..305` | Match | Length slot → length, distance slot via `DD` (+ `LDD` if needed) |

### Length encoding

`LengthSlot` → base length `2` plus slot-dependent offset:

* `slot < 8`: `length = 2+slot`.
* Otherwise `LBits = slot/4 -1`, `length = (4 | (slot&3)) << LBits` plus `LBits` extra bits (from stream). Final length `+2`. So `slot 8` with `LBits=1` and extra `0` → `8`; with extra `1` → `9`, etc. Maximum `MAX_LZ_MATCH = 0x1001` (4097). If `length>MAX_LZ_MATCH`, clamp. For matches with `distance>0x100`, `distance>0x2000`, `distance>0x40000`, the decoder adds `1` per threshold to the base length (distance-dependent increment) — encoder must subtract the same increment before slot encoding (`base = len - inc`, clamp to `2..MAX`).

### Distance encoding

`DistSlot` → distance:

* `<4`: `distance = 1+DistSlot` (`1..4`).
* Otherwise `DBits = DistSlot/2 -1`, `distance = 1 + (2 | (DistSlot&1)) << DBits` plus `DBits` extra bits, split: if `DBits>=4`, low `4` bits are Huffman-coded via `LDD` (symbol `0..15`), high bits (`DBits-4`) are raw bits; otherwise all `DBits` bits are raw. Then `OldDist` is rotated (`OldDist[3]=OldDist[2]`,…, `OldDist[0]=distance`) and `LastLength=length`. The raw `DistExtra = distance - (base+1)` is `DistSlot`-dependent.

For `DistSlot>=62` (`DBits>=30`) on 32-bit builds the distance would overflow 4 GiB — decoders on 32-bit must treat it as `0` (emit zeros) 64-bit decoders keep full `size_t`.

### Rep-match length

Same `LengthSlot` via `RD` → `length` as above, but distance comes from `OldDist[distNum]` rotated. `LastLength` is updated.

### Encoder heuristics (method-dependent, wire-compatible)

The wire format is identical for `m1..m5`; encoders differ only in effort:

| Method | `MaxChain` | `NiceLen` | `Lazy` | Notes |
|--------|-----------|-----------|--------|-------|
| `1` | `4` | `256` | `0` | Fast, shallow hash chain. |
| `2` | `8` | `512` | `0` | |
| `3` | `32` | `1024` | `1` | Lazy at `len<NiceLen` and next position better by `>1`. |
| `4` | `128` | `2048` | `1` | |
| `5` | `512` | `4096` | `1` | Deep, exhaustive. |

Additional encoder rule (integrity-preserving, not wire-visible): `FailCount` heuristic — after `0x100` consecutive positions where `FindMatch` yields `<MIN_MATCH (3)`, skip `3/4` of subsequent positions (`FailCount>0x100 → skip 3/4`, `>0x400 → 7/8`, `>0x800 → 15/16`, `>0x1000 → 31/32`). Reset `FailCount` when a match `len>=8` is found. This does not affect decodability; it trades ~0.0002 ratio for 2–3× speed.

---

## Block Framing and Flush

* Tokens are appended until `tokens≥32768` or `inputSinceBlock≥0x80000` (512 KiB), then the block is closed: `MakeTables` (build `LD/DD/LDD/RD` + `BD`), `EmitTable` (BC lengths + `BD` Huffman + `LD/DD/LDD/RD` lengths via `BD`), `EmitTokens` (Huffman-coded tokens with extra bits). `NeedFlush` is wire-transparent — the decoder only sees block boundaries via `BlockSize`/`BlockBitSize`.

* `BlockBitSize` is `TotalBits - (BlockSize-1)*8`, clamped `1..8`. The last byte of the payload contributes only `BlockBitSize` MSB bits.

* After each block, if `LastBlock` (`0x40`) is set, the file ends; otherwise the next byte is the next block header (byte-aligned).

---

## Checksums

The compressed stream itself is not checksummed per block beyond the block header `CheckSum` (`0x5A ^ Flags ^ BlockSize ^ (BlockSize>>8) ^ (BlockSize>>16)`). End-to-end integrity is the file header `Data CRC32` / `FHEXTRA_HASH` BLAKE2sp over the unpacked bytes, not over the stream.

---

## Clean-Room Checklist

* Implement absolute `Table[pos]=num` (not delta) in table decode.
* Reject `pos==0` with `16/17`.
* Handle `BC` escape `15` correctly on both encode and decode.
* Support `TABLE_SIZE=430` (RAR5) and `TABLE_SIZEX=446` (RAR7) based on `ExtraDist` (dict `>4 GiB`).
* Honor `FCI_SOLID` persistence and `LastLength`/`OldDist`.
* Implement distance `DBits` split (`LDD` for low 4 bits when `DBits>=4`).
* Encoder must respect `MaxDist` and `AvailableAt` bounds.
