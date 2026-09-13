# RAR5 Specification — Store (Method 0)

Method `0` is the uncompressed path. It is required for incompressible data, empty files, and service headers (`CMT`, `QO`, `ACL`, `STM`, `RR`). Encoders must use it when the compressed payload would be `>=` the raw payload (except for solid streams where recompression would break the chain).

---

## Wire Format

A store block carries **no** compression stream. The file (or service) payload is stored verbatim as the block’s data area:

```
File header (type 2 or 3, HFL_DATA set, Compression information method=0)
  Data area: exactly Unpacked size bytes (or Data size if FHFL_CRC32 split-part)
```

* `Data size` (common framing) equals `Unpacked size` for non-split files.
* For split files, `Data size` is the per-volume chunk; `Unpacked size` in the header is the total file size (ignored until the last volume, where `HFL_SPLITBEFORE` is clear).
* `Data CRC32` (if present) is the IEEE CRC32 of the unpacked data for the final volume, or of the packed chunk for intermediate volumes.

No Huffman tables, no `Block header`, no filters. The decoder copies the data area directly to the window (or to the output for `decompress_to_vector`).

---

## Encoder Rules

1. If `Method==0` in `Compression information`, the encoder must still emit a valid `Compression information` `vint` with `method=0`, `N` = dict size index the decoder would need if it later recompresses (conventionally `0` for store, meaning 128 KiB, or the archive’s `WinSize` for consistency). `FCI_SOLID` may be set only if the archive is solid and this store file is part of the solid chain — otherwise it must be clear.

2. Store files participate in the solid chain only if the archive is solid and the next file continues the chain. A store file with `FCI_SOLID` set must be decoded by feeding its bytes into the window at `UnpPtr` exactly as literals would, so that subsequent compressed files can reference them. A store file **without** `FCI_SOLID` resets the window (`OldDist=[-1,-1,-1,-1]`, `LastLength=0`, `UnpPtr=0` if not solid).

3. For empty files (`Unpacked size==0`), the data area may be absent (`HFL_DATA` clear, `Data size` omitted) or present as `0` bytes. Both are valid. Decoders must handle either.

---

## Decoder Rules

1. After header CRC validation, if `method==0`, verify that `Data size` (if `HFL_DATA` set) equals the expected payload size. If `HFL_DATA` is clear, the file is empty.

2. Copy `Data size` bytes from the data area to the output. In windowed mode (`UnpPtr`/`Window`), copy through the window so that solid successors can reference the bytes:

```
for i in 0..Data size-1:
    Window[UnpPtr++] = Data[i]  (wrap at MaxWinSize)
    Output[i] = Data[i]
    if UnpPtr==0: FirstWinDone = true
```

3. Update `OldDist` only if the file was solid and `Method` of the *next* file expects it — store itself does not push `OldDist`.

4. Filters never apply to store blocks (filter symbol `256` only appears in compressed streams).

---

## Quick-Open and Recovery Interaction

* Store headers are eligible for quick-open caching (`QO` array) — they are cached verbatim like any header.
* Recovery record (`RR`) parity protects store data areas identically to compressed ones (see `05-recovery.md`). The store payload is just another `DataSize` byte range fed to the RS codec.

---

## Test Vectors

* Empty file: `Name="empty.txt"`, `Unpacked size=0`, `Data size` omitted or `0`, `method=0`, no data area.
* 1-byte file: `Data size=1`, data area `0x42`, `Data CRC32 = CRC32(0x42)`.
* Multivoulme store: 10-byte file split `6+4` across two volumes — first header `HFL_SPLITAFTER`, `Data size=6`, `Data CRC32=CRC32(first 6 packed bytes)`; second header `HFL_SPLITBEFORE`, `Data size=4`, final `Data CRC32=CRC32(all 10 unpacked)`.
