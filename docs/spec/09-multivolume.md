# RAR5 Specification — Multivolume

Multivolume splits a single logical archive across several physical files. The split is at the *packed* stream level, not at file boundaries — a single file’s packed payload may be fragmented across volumes.

---

## Concepts

| Term | Meaning |
|------|---------|
| **DataCount** | Number of `.rar` data volumes (`.partNN.rar` / `.r00`) |
| **New numbering** | RAR5 `*.partNN.rar` (default, `MHFL_VOLNUMBER`) vs legacy `*.r00` (RAR3) |
| **VolSize** | Target bytes per volume (`-v<size>[k|m|g|t]`, `VOLSIZE_AUTO` for listing) |
| **Split file** | One logical file whose packed bytes are divided across volumes (`HFL_SPLITBEFORE`/`HFL_SPLITAFTER`) |
| **NextVolume** | `EHFL_NEXTVOLUME` in end header indicates more volumes follow |

---

## Volume Naming

### RAR5 new numbering (`*.partNN.rar`, `MHFL_VOLUME` + `MHFL_VOLNUMBER`)

1. If the name has no extension, append `.rar`. If it ends with `.` or is `.exe`/`.sfx`, replace extension with `.rar`.
2. Locate the right-most digit run in the filename (ignore path). For `vol.part03of05.rar` the run before the dot is used.
3. Increment that run with decimal carry leftwards. Non-digit predecessor inserts `1` (`part9.rar` → `part10.rar`). If no digits, the base becomes `*.part01.rar`.
4. First-volume derivation zeroes all digits then sets the right-most to `1`; old scheme forces `.rar`.

### Legacy RAR3 (`.r00`)

* If the extension suffix at `dot+2`/`+3` are not both digits, suffix becomes `00` (`.rar` → `.r00`).
* Otherwise increment tail `0-9` with rollover; overflow past the dot advances the leading letter (`.r99` → `.s00`).

---

## Header Flags and State

**Block common:** `HFL_SPLITBEFORE 0x0008` (continues from previous volume), `HFL_SPLITAFTER 0x0010` (continues to next).  
**Main:** `MHFL_VOLUME 0x0001`, `MHFL_VOLNUMBER 0x0002` (all volumes except first carry `VolNumber` vint: `1` for second, etc.).  
**End:** `EHFL_NEXTVOLUME 0x0001` (not the last volume).

`FirstVolume = Volume && VolNumber==0`. `VolWrite` and `Splitting` track whether the current write needs to fragment.

File headers for a split file repeat the logical file header per chunk:

* `UnpSize` is the full logical unpacked size (repeated every chunk).
* `Data size` / `PackSize` is the per-volume packed chunk.
* `File flags` and `Data CRC32` are per-chunk: intermediate volumes store CRC32 of the *packed* chunk; the final volume stores the unpacked CRC32.
* Flags: first chunk `SplitAfter`, last chunk `SplitBefore`, middle chunks both.

---

## Creation (`-v`)

```
cur = create(firstVolume)  # .part01.rar if -v set
for each header+payload (header CRC already computed):
    while payload remaining:
        space = VolSize - Tell(cur)
        maxSlice = space - MAX_HEADER_SIZE_MARGIN - ENDARC_SIZE  # ~80 + 7 bytes margin
        if maxSlice <= 0 or (encryptHeaders && maxSlice < 16):
            advanceToNextVolume(cur)  # CloseCurrent, NextVolumeName, WCreate
            continue
        remaining = total - written
        slice = min(remaining, maxSlice)
        if encryptHeaders && slice < remaining: slice &= ~(16-1)  # 16-byte AES align
        final = (written+slice >= total)
        hd.SplitBefore = written>0; hd.SplitAfter = !final
        hd.PackSize = slice  # Data size for this volume’s header
        writeHeader(cur, hd, sliceCRC); write(cur, payload[written:slice])
        written += slice
        if !final: advanceToNextVolume(cur)
```

`AdvanceToNextVolume` closes the current volume, derives `NextVolumeName(curName, !NewNumbering)`, and creates the next file. `VolSize` may be auto (`VOLSIZE_AUTO`) when listing.

Total archive size for progress (`TotalArcSize`) is pre-summed by probing `NextVolumeName` + `FastFind` for each successive volume.

Multivolume archives **do not** store `QO` or `RR` via locator (locator offsets are `0`, `WriteQO` early-returns when `VolSize>0`).

---

## Extraction and Stitching

```
if headerSize==0 && UnpVolume || headerType==ENDARC && end.NextVolume:
    if splitHeader: verify per-slice packed hash before closing
    closeCurrentVolume()
    nextName = nextVolumeName(arc.FileName, !OldNumbering)
    dataIO.ProcessedArcSize += LastArcSize
    while !arc.Open(nextName):
        if !oldSchemeTested: try nextVolumeNameAltScheme; continue
        if isDll: if !DllVolChange(nextName): fail
        elif !recoveryDone:
            if RecVolumesRestore(nextName, isNewArc=true): continue  # single-shot
        if !VolumePause && !IsRemovable(nextName): fail
        if !uiAskNextVolume(nextName): fail
    arc.CheckArc()
    if arc.Encrypted != prevEncrypted: fail
    block = splitHeader ? SearchBlock(prevType) : ReadHeader()
    if file: Seek(nextBlockPos - PackSize)
    dataIO.UnpVolume = splitAfter; dataIO.SetPackedSizeToRead(PackSize)
else:
    Seek(nextBlockPos - PackSize)  // resume within volume
    unpack/unstore; Seek(nextBlockPos)
```

* Missing volume UX: non-removable + no `VolumePause` → immediate fail; otherwise `uiAskNextVolume` prompts (forced pause with `-vp` even on fixed media).
* DLL path delegates to `UCM_CHANGEVOLUME`.
* Solid + split: if the archive starts with `SplitBefore` and the first file is solid, the decoder must scan back to the first volume (`GetFirstVolIfFullSet`) or fail `NEEDPREVVOL`.
* `IsArchive` must be re-validated after volume switch (`CheckArc`).

### Switch `-v`

* `-v<size>[k|m|g|t]` → `VolSize = GetModSize(arg,1)` with `b/k/m/g/t` multipliers (`k=1024`), float allowed.
* `-v-` clears, bare `-v` → `VOLSIZE_AUTO` (listing).
* `-vp` → `VolumePause` (pause even on fixed media).
* Multivolume disables `QO`/`RR` as above; `-ver` (file versioning) is independent of volume numbers.

---

## Implementation Checklist

* Fragment only on packed bytes; never split a header across volumes without `HFL_SPLITAFTER`/`HFL_SPLITBEFORE`.
* Per-chunk `PackSize` and per-chunk `Data CRC32` (packed) vs final unpacked.
* `VolNumber` vint in main header of every volume except first.
* `NextVolumeName` carry logic and `IsArchive` re-check.
* Respect `16`-byte alignment for header-encrypted slices.
