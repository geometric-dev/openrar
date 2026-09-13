# RAR5 Specification — Recovery Record (RR) and `.rev` Volumes

Recovery protects against volume loss or damage using Reed-Solomon (RS) codes. It is optional: indicated by `MHFL_PROTECT` in the main header and locatable via the locator’s recovery offset. The data is stored as a service header `RR` (and as standalone `.rev` volumes for multivolume sets).

---

## Overview

| Aspect | Details |
|--------|---------|
| Code | Cauchy Reed-Solomon over `GF(2^8)` (`0x11D`) for RAR5 vintage, and over `GF(2^16)` (`0x1100B` — `x^16 + x^12 + x^3 + x + 1`) for RAR5 16-bit (current). Selection is implicit by `RR` version. |
| Layout | `RR` service header data area is the parity. `.rev` volumes are full file copies of `RR` parity split across volumes (one `RR` block per `RR` header). |
| Protection | Typically `1..10%` of archive size, configurable via `-rr[n]` (`-rr` alone = ~3%). `n` is the number of RS sectors (up to `100%` with `-rr100%` or explicit count). |
| Granularity | `512`-byte sectors (`0x200`), aligned. Parity is computed per 512-byte stripe across data volumes. |

---

## Service Header `RR`

```
File header type 3, Name "RR", HFL_DATA set
  Data area: parity bytes (see below)
  File flags: 0 (no CRC32 in header; parity has its own CRCs)
  PackSize = Data size = parity length
```

Locator: main extra `Locator` with `0x0002` flag holds `Recovery offset` (distance from `RR` header to main header). If locator is missing, scanners must scan for `RR` by type.

`.rev` volumes: each `.rev` file is a standalone RAR volume containing a single `RR` service header. The `Archive number` in the main header distinguishes them. `recvol` vs `recvol5` handling: RAR4 `recvol` and RAR5 `recvol5` are distinct; RAR5 uses the `GF(2^16)` codec.

---

## Reed-Solomon — GF(256) `0x11D` (vintage, 8-bit)

* Irreducible polynomial: `x^8 + x^4 + x^3 + x^2 + 1` (`0x11D`).
* Tables: `gfExp[512]` (duplicate wrap), `gfLog[256]`.
* Generator `g(x) = (x - a)(x - a^2)...(x - a^N)` where `a` is primitive (`2`), `N` = number of recovery volumes / sectors.
* Encoding: linear feedback shift register with taps `g(x)`. Input `Data[0..DataSize-1]` (sector bytes), output `Dest[0..N-1]` parity.
* Decoding: syndrome `S[i] = sum_j Data[j] * a^{(i+1)*j}`, error locator via `Erasure` positions, then Vandermonde inversion.

This codec is retained for RAR4 compatibility; RAR5 decoders may encounter it in older archives.

---

## Reed-Solomon — GF(65536) `0x1100B` (current, 16-bit)

* Irreducible: `x^16 + x^12 + x^3 + x + 1` (`0x1100B`, `0x1100B` truncated to 16 bits `0x100B` with high bit implicit).
* Tables: `gf_exp` size `~262144` (`4*GF_SIZE+1`), `gf_log` size `GF_SIZE+1` where `GF_SIZE=65535`.
* Matrix: Cauchy `Mx[i][j] = 1 / ((i + ND) ^ j)` where `ND` = number of data volumes/sectors, `i` in `0..NR-1`, `j` in `0..ND-1` (`^` is XOR, division is `gf` division). `NR` = number of recovery volumes.
* Encoding: per 512-byte sector, `NR` parity sectors are computed as matrix-vector product over `GF(2^16)` (each `uint16` word is an element). Data is treated as `uint16` little-endian words per sector; odd trailing byte is padded.
* Decoding: given `valid_flags` per volume (present/missing), select `NE` available parity rows covering the `NE` missing data positions, build `NE×ND` decoder matrix, invert, then reconstruct missing sectors. Opaque `data_log` caching of current data block’s log values.

Constants from `recvol5`:

* `DATA_SECTOR = 512`
* `RR_BLOCK_SIZE` is a multiple of `DATA_SECTOR`.
* `MaxPar = 255` recovery volumes (parity bytes per RS stripe).

---

## Encoding Flow (Writer)

1. After all file/service headers and end header are written, compute parity over the byte range from end of `Main` header through end of `End` header (excluding `RR` itself and any `QO`/`CMT` that are not protected? RAR protects the main file area; locator tells the protected range. Current writers protect from first file header through end header).

2. For each 512-byte stripe aligned to `DATA_SECTOR`, compute `NR` parity bytes (8-bit) or words (16-bit). For `GF(2^16)`, each sector is `256` `uint16` words.

3. Store parity as `Data area` of the `RR` service header (single volume) or split across `.rev` volumes (multivolume: each `.rev` holds one `RR` block header plus its parity slice).

4. After writing `RR`, patch the locator’s recovery offset (main header CRC is recomputed) and patch the `RR` header’s own CRC and size vint (padded reservation).

---

## Decoding / Repair Flow (Reader, `r` command)

1. Detect missing/damaged volumes: `valid_flags[i] = 0/1` per volume (present and header CRC `OK`).

2. If `number of valid data + valid parity >= ND`, reconstruct:
   * For each damaged byte/sector, use the RS matrix to compute the missing `uint16` word from available data+parity.
   * For `GF(2^16)`, word order is little-endian per sector.

3. Write reconstructed volumes to new files (`*.1`, `*.2` etc.) or in-place if the archive is on a writable filesystem. The reconstructed archive is then `t`/`x` verified via its own header CRCs and file `CRC32`/`BLAKE2sp`.

4. If reconstruction fails (too many erasures `>NR` or header CRC mismatch after correction), return `RARX_REPAIR`.

Multivolume recovery with `.rev` files: the `r` command reads `*.part01.rar` + `*.rev` and rebuilds any missing `*.partNN.rar` as long as `NE <= NR`.

---

## Checksums Within Recovery

* `RR` service header itself has a header CRC32 (like any block).
* Each sector’s parity is not individually checksummed; integrity is verified by re-computing the syndromes and by the file header `Data CRC32` / `BLAKE2sp` after reconstruction.
* `rs16` codec verifies `gf_log`/`gf_exp` consistency and matrix invertibility before decoding; non-invertible (e.g., duplicate valid flags) is a hard error.

---

## Implementation Notes

* RAR4 (`0x11D`) and RAR5 (`0x1100B`) codecs must coexist. Detect by `RR` version field or by archive version (`FCI_ALGO` bits). Writers always emit the current (`0x1100B`) unless compatibility flags force vintage.
* Performance: `GF(2^16)` multiplication is `gf_exp[gf_log[a]+gf_log[b]]`; use `gf_exp` duplication to avoid modulo. Cauchy matrix inversion is `O(NE^3)` per stripe — cache the inverted matrix per `valid_flags` pattern (only one inversion per repair, not per sector).
* Threading: `rs16` update is data-parallel per stripe; writers use thread pool per `NR`.
* Locator patching: `RR` offset is patched after parity is known, just like `QO` offset. Patching must also recompute the main header CRC.

---

## Clean-Room Checklist

* Implement both `GF(256)` `0x11D` and `GF(65536)` `0x1100B` with correct polynomial.
* Support `512`-byte sectoring and `.rev` volume splitting.
* Verify `MHFL_PROTECT` + locator before trusting offsets.
* Return reconstruction failure if `erasures > parity` count.
