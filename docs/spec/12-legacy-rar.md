# Legacy RAR Specification — 1.5–4.0 Header Walk & Migration Boundary (v1.29.0)

Normative reference for the v1.29.0 legacy-RAR reader. Written from
public knowledge of the RAR 1.5–4.0 archive format (pre-RAR5). SCOPE
(SECURITY_ARCHITECTURE §5.3, ROADMAP §v1.29.0): **header parsing +
listing + STORED-entry extraction only.** The 2.x/3.x decompressors
(rar15/20/29 LZ, PPMd) are NOT implemented, and the RAR 2.x/3.x Virtual
Machine filter subsystem is NOT implemented and will not be — VM-filtered
entries fail with an explicit per-entry error. No partial VM emulation,
ever; no partial LZ emulation either (Phase 1). Numbers are little-endian.

---

## 1. Signature and dispatch

`52 61 72 21 1A 07 00` (7 bytes: "Rar!\x1a\x07\x00"). The RAR5 signature
differs in the last two bytes (`01 00`), so dispatch is unambiguous
(`detect_foreign_format`, spec 11 §1). SFX modules are legacy archives
with an executable stub prepended — the signature is located by a
bounded forward scan (stub ≤ 4 MiB, the shipped MAX_SFX_SIZE bound);
a stub that hides the signature beyond the window → unparseable (13).

## 2. Block framing

Every block: HEAD_CRC (2, CRC32 of the following HEAD_TYPE..data,
lower 16 bits stored), HEAD_TYPE (2), HEAD_FLAGS (2), HEAD_SIZE (2).
HEAD_FLAGS bit 15 (0x8000) = LONG_BLOCK: a 4-byte DATA_SIZE field
follows the fixed header (added to HEAD_SIZE to get the total block
footprint). Type `0x7B` = ENDARC. Total block size must fit within
the remaining file; a block extending past EOF → structural failure.

Header CRC: IEEE CRC32 (poly 0xEDB88320) over HEAD_TYPE through the
end of the header (and, for file headers, the extra area when
HEAD_FLAGS bit 9 / 0x100 is absent — the exact span per type is pinned
in the parser tests). Mismatch → structural integrity failure
(pre-analysis §2 table): the walk aborts (exit 2) — legacy headers
have no skip-marker semantics we can trust.

## 3. MAIN_HEAD (0x73)

HEAD_FLAGS: 0x0001 volume, 0x0002 volume-annotation-present, 0x0004
locked, 0x0008 solid, 0x0010 auth-info-present (v1.5–2.x), 0x0020
RECOVERY_PRESENT, 0x0040 block-headers-encrypted (v3.9+ / -hp), 0x0080
first-volume. High 12 bits: reserved/extra-size variants in 3.9+.
- 0x0040 (encrypted headers) → the archive is refused up front
  (exit 11 class: encrypted content cannot be processed without
  header decryption, which is out of Phase-1 scope) — explicit, never
  a silent walk into garbage.
- 0x0080 (first volume) / 0x0001 (multi-volume): Phase 1 migrates
  single-volume sets only; a multi-volume set → explicit refusal
  (exit 7 usage-class message) rather than partial migration.
- RECOVERY-PRESENT and solid flags are structural metadata only —
  they do not affect Phase-1 behavior (solidity matters only to
  decompressors, which do not exist here).

## 4. FILE_HEAD (0x74) and SUB_HEAD (0x7A)

Fixed part after the common 8 bytes: PACK_SIZE (4, when LONG_BLOCK),
UNP_SIZE (4), HOST_OS (1), FILE_CRC (4), FTIME (4, DOS packed time),
UNP_VER (1), METHOD (1), NAME_SIZE (2), ATTR (4). HEAD_FLAGS 0x100
(large file, v3.9+): a 64-bit extension block follows the fixed part
(8 bytes: HIGH_PACK_SIZE, HIGH_UNP_SIZE) — sizes are then 64-bit.
HEAD_FLAGS 0x400 (salt, v3.x): 8-byte salt follows the optional extra
area — encryption marker (see §4.3). Name bytes (NAME_SIZE) follow;
v3 archives with HEAD_FLAGS 0x200 carry the unicode-name encoding
(§4.2). For v3 (UNP_VER ≥ 29) an EXTRA AREA may sit between the name
and the data (its size is the high 12 bits of HEAD_FLAGS after the
0x0FFF mask shift — per public documentation; bounded, parsed only for
the time fields we consume, skipped otherwise).

- HOST_OS: 0 MS-DOS, 1 OS/2, 2 Win32, 3 Unix, 4 macOS, 5 BeOS.
  ATTR semantics per OS: MS-DOS/Win32 → DOS attribute byte (map to
  RAR5 file_attr); Unix/macOS → POSIX mode honored only with 0170000
  type bits (§4.3 rule).
- FTIME: DOS packed time (local-time interpretation, documented
  divergence class) → epoch, then MtimeBounds clamp.
- **METHOD:** 0x30 store — extract. 0x31–0x35 → per-entry refusal
  ("legacy compression method 0x3N not implemented in Phase 1;
  migrate via an oracle tool first").
- **UNP_VER:** the decompressor version the entry requires. 29 (2.9,
  the RAR3 codec) and 20/15 map to the refused methods above. Entries
  whose headers reference VM filter sub-blocks (SUB_HEAD 0x7A blocks
  whose content is a VM code record per public documentation) → the
  §5.3 named error ("VM filter entries are not supported —
  SECURITY_ARCHITECTURE §5.3"). Because Phase 1 implements NO legacy
  decompressor, the VM boundary is provable by absence: no filter can
  execute in a binary containing no legacy decoder.

### 4.1 Stored-entry extraction

METHOD 0x30 with UNP_VER any: the data area (PACK_SIZE bytes at the
current read position) IS the payload. Verify FILE_CRC (IEEE CRC32 of
the payload; for split entries — HEAD_FLAGS 0x0001/0x0002 — the
per-volume CRC semantics of the legacy format do NOT apply in Phase 1:
split entries across volumes are refused with the multi-volume
refusal). Then the standard transcode staging pipeline (spec 11 §6-8).

### 4.2 Names

- Plain (0x200 clear): NAME_SIZE raw bytes; encoding is
  platform-historical — treat as UTF-8 when well-formed, else
  percent-encode (§4.4 rule; composition per spec 11 §6).
- 0x200 set (v3 unicode): the name field starts with 1 byte
  `DecompSize`, followed by an interleaved ASCII + UTF-16LE-delta
  scheme per public documentation (ASCII pass-through with flag byte
  0x80-escape for wide runs). The spec doc pins the decode; ANY
  malformed sequence → percent-encoded fallback of the RAW bytes
  (never a mis-decoded name), then composition.

### 4.3 Encrypted entries

HEAD_FLAGS 0x400 (salt present) or METHOD 0x30 with password flag
semantics → per-entry refusal ("legacy encrypted entry — not supported
in Phase 1"). No legacy crypto is implemented in Phase 1 (the rar29
crypt has no in-tree primitive).

## 5. Directory entries

ATTR with DOS directory bit (0x10) or Unix mode with S_IFDIR →
directory record (no data). Names compose through spec 11 §6.

## 6. Refusal taxonomy (all per-entry unless noted)

| Condition | Class | Report |
| :--- | :--- | :--- |
| HEAD_CRC mismatch | structural abort (2) | pre-output |
| block past EOF | structural abort (2) | pre-output |
| header-encrypted main (0x0040) | archive-class (11) | up front |
| multi-volume set | usage refusal (7) | up front |
| VM filter sub-blocks | per-entry fail | §5.3 named error |
| method 0x31–0x35 | per-entry skip | method-named |
| salt/encrypted entry | per-entry skip | encrypted_refused |
| undecodable name | per-entry entry with escaped name | name_escaped |

## 7. What Phase 1 does NOT do (truthful boundary)

- No rar15/20/29 LZ decoding, no PPMd, no VM, no legacy crypto —
  README "Not yet" row states this; a candidate later arc would need
  its own Gate 0 (the codecs are the largest clean-room item in the
  whole migration theme).
- No multi-volume legacy sets, no split entries, no header decryption.
- Everything else (header walk, listing metadata, store extraction,
  name/time/attr translation) behaves like the other foreign readers.
