# RAR5 Specification — Filters

Filters are reversible transforms applied to the *unpacked* byte stream after LZ decoding but before writing. They improve compression for executable and audio data. Filters are signalled in-band as a token (`LD` MainSlot `256`) and applied after the entire file is decoded (or windowed, with correct ordering).

---

## Filter Signalling

A filter token appears in the token stream like any `LD` symbol:

```
MainSlot == 256
  BlockStart  : variable-length integer, bytes = GetBits(2)+1 (1..4), little-endian via GetBits(8) per byte
  BlockLength : same encoding
  Type        : 3 bits (GetBits(3))
  Channels    : if Type==0 (DELTA) then 5 bits (GetBits(5)+1) else 1
```

* `BlockStart` is an offset relative to the current unpack position. The linear start in this file's output is `total_written_so_far + BlockStart`. The address fixup base for E8/E8E9/ARM (`file_offset`) is the linear file position including prior solid bytes: `solid_base + total_written_so_far + BlockStart`.
* `BlockLength` is clamped: if `>0x400000` (4 MiB) it is forced to `0` (no-op filter).
* `Type` values: `0=DELTA`, `1=E8`, `2=E8E9`, `3=ARM`, `4=AUDIO`, `5=RGB`, `6=ITANIUM`, `7=TEXT`. Only `0..3` are used by RAR5 encoders today; decoders must skip unknown types.
* At most `8192` filters per file; excess must flush the write buffer (`UnpWriteBuf`) and reset.

`BlockStart` distance to the filter token is bounded: the encoder guarantees `BlockStart <= MaxWinSize`, so wrap-around at most once.

### NextWindow

In windowed (solid) mode, a filter whose `block_start` lies in the not-yet-written part of the circular window must be deferred:

```
Filter.NextWindow = (WrPtr != UnpPtr && WrapDown(WrPtr - UnpPtr) <= BlockStart)
Filter.BlockStart = (BlockStart + UnpPtr) % MaxWinSize
```

`NextWindow` filters are skipped during the current write pass; when `BlockStart` enters the current write range, `NextWindow` is cleared. Single-file decoders that produce the whole output before applying filters apply every filter whose linear range lies within the output.

Filter state is per-file: the pending queue is cleared at every file boundary even when the LZ window, distances, and tables carry over. A filter region never spans two files.

Maximum filter block is `4 MiB`; filters that would cross the write border are adjusted to defer the tail.

---

## Filter Application

All filters are applied **out-of-place**: copy the source block to a temporary buffer, transform in place, then copy back. This preserves window bytes for future LZ matches (which must see the pre-filter bytes).

Filters are applied in insertion order after the file’s LZ bytes are fully decoded (or incrementally after each `UnpWriteBuf` in windowed mode). Overlapping filters are applied sequentially.

### 0 — DELTA

*Purpose:* audio / PCM interleaving.

```
Channels = GetBits(5)+1  (1..32)
DataSize = BlockLength
SrcPos = 0
for Channel in 0..Channels-1:
    Prev = 0
    for DestPos = Channel; DestPos < DataSize; DestPos += Channels:
        Dst[DestPos] = Prev - Src[SrcPos++]
        Prev = Dst[DestPos]
```

Bytes within each channel are delta-decoded (subtraction modulo 256); channels are de-interleaved (encoder interleaves them). Groups are contiguous per channel. Single-channel delta is just `Prev -= Src[SrcPos++]` (`Prev - Src`). The encoder stores `Src = Prev - Dst` (also subtraction, same modulo) so the decoder above round-trips. Implementations must use subtraction in both directions.

### 1 — E8

*Purpose:* x86 `CALL` (`E8`) address translation.

```
FileOffset = WrittenFileSize  (low 32 bits, the file offset of the block start)
FileSize   = 0x1000000 (16 MiB window)
CmpByte2   = 0xE8 (for E8) / 0xE9 (for E8E9)
for CurPos = 0; CurPos+4 < DataSize; CurPos++:
    CurByte = Data[CurPos]
    if CurByte==0xE8 || CurByte==CmpByte2:
        Offset = (CurPos + FileOffset) % FileSize
        Addr   = LE32(Data[CurPos+1])  (raw 32-bit target)
        if (Addr & 0x80000000) { // Addr < 0 (high bit set)
            if ((Addr + Offset) & 0x80000000) == 0) // Addr+Offset >=0
                LE32(Data[CurPos+1]) = Addr + FileSize
        } else {
            if ((Addr - FileSize) & 0x80000000) // Addr < FileSize
                LE32(Data[CurPos+1]) = Addr - Offset
        }
        CurPos += 4  // skip operand
```

For `E8` only `0xE8` bytes are transformed; for `E8E9` both `0xE8` and `0xE9` (`JMP`) are. The `FileSize` window is `16 MiB`; transformation is reversible and position-dependent. `DataSize<4` is skipped (needs `CurPos+4<DataSize`).

Encoders may emit either `E8` or `E8E9`; decoders must handle both. The transform is applied with `linear_start` as the file offset base (for windowed decoders, the linear file offset at `BlockStart`, not the window offset).

### 2 — E8E9

Same as `E8` but `CmpByte2 = 0xE9`. See above.

### 3 — ARM

*Purpose:* ARM `BL` (`0xEB`) branch with condition `Always` (`0xE` prefix in ARM encoding). Disabled by default since RAR 5.80b3 for modern 64-bit ARM, but decoders must still support it.

```
FileOffset = WrittenFileSize
for CurPos = 0; CurPos+3 < DataSize; CurPos += 4:
    if Data[CurPos+3] == 0xEB: // BL Always
        Offset = (Data[CurPos] | Data[CurPos+1]<<8 | Data[CurPos+2]<<16)
        Offset -= (FileOffset + CurPos)/4
        Data[CurPos]   = Offset & 0xFF
        Data[CurPos+1] = (Offset>>8) & 0xFF
        Data[CurPos+2] = (Offset>>16) & 0xFF
```

Operates on 4-byte ARM instruction groups, little-endian. `FileOffset` truncated to 32 bits.

### 4..7 — Reserved

`AUDIO` (4), `RGB` (5), `ITANIUM` (6), `TEXT` (7) — not emitted by current RAR5 `m1..m5` encoders for `LD` filter type dispatch. Decoders for the LZ `LD=256` path must skip unknown types as `FILTER_NONE`. VM standard filters for audio/ITANIUM are defined separately in the VM specification and are not part of this LZ filter dispatch.

---

## Encoder Constraints

* Filter blocks are `≤4 MiB`; larger blocks are clamped to `0` length (no-op).
* Encoders choose filter placement after LZ analysis; the `BlockStart` must be within `MaxWinSize` of the filter token position.
* Solid archives must respect `NextWindow` — a filter referencing not-yet-flushed window bytes must be deferred correctly.

---

## Decoder Checklist

* Implement `BlockStart`/`BlockLength` variable-length `GetBits(2)+1` bytes, little-endian per byte.
* Handle `NextWindow` for windowed decoders; linear decoders may ignore it (their out vector never wraps).
* Apply filters out-of-place, in insertion order, after `UnpPtr` advancement and before `WrPtr` flush.
* Support all four types (`DELTA`, `E8`, `E8E9`, `ARM`); skip unknown types.
* Verify `BlockLength` bounds and `8192` filter limit (flush and reset on overflow).
