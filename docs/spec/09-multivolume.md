# RAR5 Specification — Multivolume

Multivolume splits a single logical archive across several physical files. The split is at the *packed* stream level, not at file boundaries — a single file’s packed payload may be fragmented across volumes.

---

## Concepts

| Term | Meaning |
|------|---------|
| **Data volume count** | Number of `.rar` data volumes (`.partNN.rar` / `.r00`) |
| **New numbering** | RAR5 `*.partNN.rar` (default, volume-number flag `0x0002`) vs legacy `*.r00` (RAR3) |
| **Volume size** | Byte limit for each volume (`-v<size>[k|m|g|t]`, or auto-sizing for listing) |
| **Split file** | One logical file whose packed bytes are divided across volumes (split-before `0x0008` / split-after `0x0010`) |
| **Next volume** | next-volume flag `0x0001` in the end block indicates more volumes follow |

---

## Volume Naming

### RAR5 new numbering (`*.partNN.rar`, volume flag `0x0001` + volume-number flag `0x0002`)

1. If the name has no extension, append `.rar`. If it ends with `.` or is `.exe`/`.sfx`, replace extension with `.rar`.
2. Locate the right-most digit run in the filename (ignore path). For `vol.part03of05.rar` the run before the dot is used.
3. Increment that run with decimal carry leftwards. Non-digit predecessor inserts `1` (`part9.rar` → `part10.rar`). If no digits, the base becomes `*.part01.rar`.
4. First-volume derivation zeroes all digits then sets the right-most to `1`; old scheme forces `.rar`.

### Legacy RAR3 (`.r00`)

* If the extension suffix at `dot+2`/`+3` are not both digits, suffix becomes `00` (`.rar` → `.r00`).
* Otherwise increment tail `0-9` with rollover; overflow past the dot advances the leading letter (`.r99` → `.s00`).

---

## Header Flags and State

**Block common:** Split-before `0x0008` (continues from previous volume), split-after `0x0010` (continues to next).  
**Main:** Volume flag `0x0001`, volume-number flag `0x0002` (every volume after the first carries a volume-number vint: `1` for the second, and so on).  
**End:** Next-volume flag `0x0001` (not the last volume).

A volume is the first one when the main header sets the volume flag `0x0001` and its volume-number field is `0`. Writers track whether the current output is mid-fragment when deciding whether to start a new volume.

File headers for a split file repeat the logical file header per chunk:

* `Unpacked size` is the full logical file size (repeated every chunk).
* `Data size` is the per-volume packed chunk.
* `File flags` and `Data CRC32` are per-chunk: intermediate volumes store CRC32 of the *packed* chunk; the final volume stores the unpacked CRC32.
* Flags: the first chunk sets split-after (`0x0010`), the last sets split-before (`0x0008`), and middle chunks set both.

---

## Creation (`-v`)

```
open the first volume                       # .part01.rar when -v is set
for each header+payload (header CRC already computed):
    while payload remaining:
        space = volume size limit - bytes already written to this volume
        maxSlice = space - header/end-block reserve    # keeps the tail block + margin
        if maxSlice <= 0 or (header encryption and maxSlice < 16):
            close this volume, derive the next name, open it
            continue
        remaining = total - written
        slice = min(remaining, maxSlice)
        if header encryption and slice < remaining:
            round slice down to a 16-byte multiple
        final = (written + slice >= total)
        header.split_before = written > 0
        header.split_after = !final
        header.data_size = slice               # this volume's chunk
        write header, then payload[written : written+slice]
        written += slice
        if !final:
            close this volume, derive the next name, open it
```

Closing a volume flushes it and derives the next name from the current one; the
next file is then created. When listing, the size limit may be auto.

For progress reporting, the total is obtained up front by probing successive
volume names until a file is missing; no additional metadata is stored for it.

Multivolume archives **do not** store the quick-open or recovery-record offset
through the locator: both locator offsets are `0`, and the writer skips the
quick-open block while the size limit is non-zero.

---

## Extraction and Stitching

The reader stitches volumes at three kinds of boundary: an entry whose data
continues into the next volume, a header split across volumes, and a plain
volume-to-volume transition (an end-of-archive block carrying the "next volume"
flag).

```
on volume boundary:
    verify the slice that ends here (a split header is checked per slice
        against its packed hash before the volume is closed)
    close the current volume
    next = derive the next volume name from the current one
    progress += bytes consumed by the volume just closed
    while the next volume cannot be opened:
        try the alternate numbering scheme once, then stop trying it
        if embedded in an application: report the change to the host
        else if recovery has not been attempted:
            if .rev parity reconstructs the volume: continue
        if the medium cannot be swapped: fail
        else ask for the next volume (forced pause with -vp)
    re-validate the archive signature
    encryption state must match the previous volume
    resume at the boundary: continue the split header, or seek to the
        packed data offset of the continuation
```

* Missing volume: non-removable media fails immediately; removable media asks
  for the next volume, and `-vp` forces that prompt even on fixed media.
* Embedders receive a volume-change callback instead of a prompt.
* A solid archive that starts with a split entry must rewind to the first volume
  before decoding, or fail with "previous volume required".
* The archive signature must be re-validated after every volume switch.

### Switch `-v`

* `-v<size>[k|m|g|t]` sets the volume size; `k=1024` and the larger suffixes
  follow, decimals allowed.
* `-v-` clears the setting; a bare `-v` selects auto-sizing for listing.
* `-vp` keeps asking for the next volume even on fixed media.
* Multi-volume sets store neither `QO` nor `RR` through the locator; `-ver`
  (file versioning) is independent of volume numbering.

---

## Implementation Checklist

* Fragment only on packed bytes; never split a header across volumes without split-after / split-before flags set.
* Per-chunk `Data size` and per-chunk `Data CRC32` (packed) vs final unpacked.
* Volume-number vint in the main header of every volume except the first.
* Name carry logic across volumes (digits and letter suffixes) and signature re-validation.
* Respect `16`-byte alignment for header-encrypted slices.
