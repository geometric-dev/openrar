# RAR5 Specification — Self-Extracting (SFX)

SFX archives are regular RAR5 archives with an executable stub prepended. The stub is opaque and not interpreted by the RAR parser except for its length.

---

## Layout

```
[ SFX stub (arbitrary PE/ELF, ≤4 MiB) ][ RAR 5.0 signature 52 61 72 21 1A 07 01 00 ][ Archive encryption header? ][ Main header ][ File/Service headers … ][ RR? ][ End header ]
^                                     ^                                           ^
0                                     SFXSize                                     SFXSize + 8
```

* The stub is *any* data preceding the signature; spec allows up to `MAXSFXSIZE = 0x400000` (4 MiB) to be scanned. Older docs say 1 MiB but implementations scan 4 MiB. The stub size is **not** stored — it is discovered at open by scanning for the signature.
* Extension: `SetSFXExt` → `.exe` (Windows) / `.sfx` (Unix); `arc.rar` with `-sfx` becomes `arc.exe`, `arc.sfx` keeps `.sfx`.
* No padding or gap: headers start at `SFXSize` exactly.
* All stored offsets (quick-open locator, recovery record) are stream-absolute, so they remain valid with a non-zero prefix. Recovery parity covers the whole stream including the stub.

With `-v`, the stub is prepended to the **first volume only** (`vol.part01.exe`); subsequent volumes are regular `*.rar`.

---

## Detection

**Signature test** on bytes `D[0..]`:

```
if D[0]!=0x52: NONE
if D[1]==0x45 && D[2]==0x7E && D[3]==0x5E: RARFMT14 (SFX-only legacy, ignore if SFX_MODULE)
if D[1]==0x61 && D[2]==0x72 && D[3]==0x21 && D[4]==0x1A && D[5]==0x07:
    if D[6]==0: RARFMT15
    if D[6]==1: RARFMT50
    if 1<D[6]<5: RARFMT_FUTURE
else: NONE
```

**Archive open:**

```
if IsSignature(Mark[0..7]) != NONE: SFXSize=0, Format=type
else:
    buf = read up to MAXSFXSIZE-16 from offset 0
    for i in 0..len-8:
        if buf[i]==0x52 && IsSignature(buf[i..]) != NONE:
            SFXSize = CurPos + i
            seek SFXSize; read Mark; break
    if no signature found: not an archive
if RARFMT_FUTURE: warn NEWRARFORMAT, fail
HeadSize = 8 for RAR5 (7 for RAR4)
Loop ReadHeader() to find HEAD_MAIN or HEAD_CRYPT
```

Scan is linear for `0x52`, `O(N)` up to 4 MiB. Headers inside the SFX prefix (i.e., `CurBlockPos <= SFXSize+HeadSize`) are **not** decrypted.

A non-zero `SFXSize` causes `list` to print `SFX`.

---

## SFX Module Runtime

If built with `SFX_MODULE`, `main()` forces `Command="X"` (extract self from `ModuleName = GetModuleFileStr()` / `argv[0]`), honoring only `-t`/`-v`/`-?`. Otherwise the SFX stub is inert.

---

## Creation (`-sfx[name]`)

### Switch and Module Lookup

* `-sfx` alone → `default.sfx`; `-sfx<name>` → `<name>` verbatim (path and extension significant).
* Bare name (no path separator) is resolved against **current directory first**, then **openrar executable directory**. Path-qualified names are used as-is. First existing file wins. If none, report `Cannot open <path>` with exit 6 and create nothing.

### Output Naming

* Extension is forced to `.exe` unless already `.exe` or `.sfx`: `arc.rar`→`arc.exe`, `arc.gz`→`arc.exe`, `arc.sfx` stays. Bare names get `.rar` appended first by normal archive-name processing, then the rule applies.
* Final name is known before opening, so overwrite checks, `-tk` time capture, self-exclusion from file scan, and log lines use the final `.exe` name.

### Size Bound

* Module must be `≤ MAXSFXSIZE` (4 MiB). Larger modules are rejected with exit 7 (user error) before creating output, because the signature would be beyond the scan bound and the archive would be unopenable.

### Layout of Created Archives

* Single-pass: module bytes are copied (64 KiB chunks) then signature and headers. No alignment gap.
* Offsets remain stream-absolute.
* Deterministic: same module + inputs → byte-identical output (module bytes are not re-encoded).
* `-v`: first volume gets the stub, rest are regular.

### Not Implemented

* The `s` command (convert existing archive to SFX) is not implemented; use `a -sfx`.
* No automatic 32/64-bit module selection.

---

## Handling Notes

* `SFXSize` is discovered, not stored — scanners must tolerate arbitrary preambles, not just `MZ`/`ELF`.
* `GetStartPos() = SFXSize + MarkHead.HeadSize (+ CryptHead size if present)` is the base for progress and for `Seek(NextBlockPos)`.
* Security: a valid signature can be hidden inside executable data; bounded scan (`≤4 MiB`) limits search cost.
