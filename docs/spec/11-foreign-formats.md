# Foreign Format Specification — ZIP / TAR / GZIP Read Side (v1.29.0)

Normative reference for the v1.29.0 foreign-format readers and the `cv`
transcoder. Written from public knowledge of the published standards:
PKWARE APPNOTE.TXT (ZIP), RFC 1951 (DEFLATE), RFC 1952 (GZIP), POSIX
ustar + GNU/pax extensions (TAR). Security constraints inherit
`SECURITY_ARCHITECTURE.md` §2.1/§3.1/§4.1/§4.4/§5.3/§5.4 with ZERO
relaxation; the mapping table in §8 is normative. Numbers below are
little-endian unless marked big-endian (network order).

---

## 1. Dispatch (one detection function)

`detect_foreign_format(path)` runs in this order; first match wins:

1. `52 61 72 21 1A 07 01 00` → RAR5 — NOT a `cv` source (usage error 7;
   re-packing existing RAR5 is `a`/`u`/`f`).
2. `52 61 72 21 1A 07 00` (7 bytes) → legacy RAR 1.5–4 — **not supported in
   v1.29 (scope decision)**: refused with an explicit error naming the
   reason (the legacy RAR reader, including header parsing and store-entry
   extraction, was dropped from the arc; SECURITY_ARCHITECTURE §5.3's VM
   boundary is provable by absence — no legacy decoder or parser exists).
   A future migration arc would need its own Gate 0.
3. `50 4B 03 04` or `50 4B 05 06` (EOCD-only empty archive) → ZIP.
4. `1F 8B` → GZIP.
5. Otherwise: TAR if the 512-byte block at offset 0 validates per §4.1
   (checksum test; v7 archives carry no `ustar` magic). TAR is tried LAST.
6. None of the above → unparseable (exit 13).

A gzip'd tar is GZIP (one entry; no recursion — §7). A TAR named `.zip`
is ZIP (signatures outrank names). Ambiguity tests are pinned
(`cv_format_detection_precedence`).

---

## 2. ZIP (APPNOTE.TXT read subset)

### 2.1 Structures

- **Local file header (LFH)** at `PK\x03\x04` (0x04034b50): version
  needed (2), GP flags (2), method (2), DOS time/date (4), crc32 (4),
  compressed size (4), uncompressed size (4), name len (2), extra len
  (2), name, extra. With GP bit 3 (data descriptor) the crc/size fields
  here are ZERO placeholders and the real values follow the payload in
  an optional descriptor (`PK\x07\x08` or legacy short form) — the
  reader does not need the descriptor: the central directory is the
  compared authority (§2.3).
- **Central directory file header (CDH)** at `PK\x01\x02` (0x02014b50):
  version made by (2), version needed (2), GP flags (2), method (2),
  DOS time/date (4), crc32 (4), compressed size (4), uncompressed size
  (4), name len (2), extra len (2), comment len (2), disk start (2),
  internal attrs (2), external attrs (4), LFH offset (4), name, extra,
  comment.
- **EOCD** at `PK\x05\x06` (0x06054b50): disk numbers (4), entries on
  disk / total (2+2), CD size (4), CD offset (4), comment len (2),
  comment. **EOCD64** (`PK\x06\x06`) + **locator** (`PK\x06\x07`)
  support ZIP64: total-entries and CD offset/size fields that are
  0xFFFF/0xFFFFFFFF in EOCD are resolved from EOCD64; per-entry, a CDH
  field that is 0xFFFFFFFF is resolved from the entry's ZIP64 extra
  field (header id 0x0001, fields in order: uncompressed size,
  compressed size, LFH offset, disk start — present fields only, as
  many as were 0xFFFFFFFF).
- **EOCD discovery:** backward scan bounded by the comment field (max
  64 KiB) with an absolute window cap of 4 MiB (prepended SFX stubs
  tolerated). A located signature must field-validate (CD offset +
  CD size ≤ file size; entry counts consistent) else the archive is
  unparseable (exit 13).

### 2.2 Methods

| Method | Meaning | Reader behavior |
| :--- | :--- | :--- |
| 0 | store | decode |
| 8 | DEFLATE (RFC 1951) | decode via `Inflate` (in-flight cap, §8) |
| 12 / 14 / 98 / 99 | bzip2 / LZMA / PPMd / AES | per-entry refusal (method-named) |
| other | unknown | per-entry refusal (method-named) |

### 2.3 The CD-vs-LFH pre-flight gate (§2.1 hardening — release gate)

After the CD walk completes (and BEFORE any staging file is created),
each entry's LFH is read at its CDH-recorded offset and compared:

| Field | Rule |
| :--- | :--- |
| name bytes | strict — any difference → structural abort (exit 2) |
| method | strict |
| LFH offset | strict; continuity validated during the CD walk |
| crc32, sizes | compared UNLESS GP bit 3 (LFH fields are placeholders) or the field is 0xFFFFFFFF/0xFFFF (ZIP64 resolution — compare the resolved ZIP64 extra values) |
| GP flags | compared subset = {bit 0 (encrypted), bit 3 (descriptor), bit 11 (UTF-8)}; remaining bits tolerated |

ANY mismatch outside this table → structural abort (exit 2). The gate
is integrity-failure semantics (§3.1 *abort run*), never best-effort
reconciliation. The source file is opened ONCE and read sequentially
from the pinned handle through pre-flight and decode (no re-open; a
raced file fails CRC, exit 3).

### 2.4 Names and encodings

- GP bit 11 set → name bytes are UTF-8; invalid sequences are
  percent-encoded (§4.4; the escaped name IS the name).
- GP bit 11 clear → name bytes are CP437; the full 256-entry CP437 →
  Unicode map (in-tree table) decodes losslessly, then the standard
  composition applies (§6).
- `\` bytes in names are normalized to `/` BEFORE composition (APPNOTE
  defines `/` as the separator; a literal backslash in a stored Unix
  name is indistinguishable from a weaponized one — documented lossy
  rule). The composed name (§6) is the emitted RAR5 name.
- Per-entry comments have no RAR5 representation → skipped with the
  `comment_skipped` flag. The archive comment (EOCD) migrates to the
  RAR5 `CMT` service after display-safety review (valid UTF-8 enforced;
  sanitized line output downstream per §7.1).

### 2.5 Attributes and timestamps

- External attrs: DOS portion (byte 0) → RAR5 file_attr (readonly
  0x01, hidden 0x02, system 0x04, directory 0x10, archive 0x20);
  UNIX portion (high 16 bits when made-by host is Unix(3)/macOS) →
  POSIX mode honored ONLY when real st_mode type bits (0170000) are
  present (SECURITY_ARCHITECTURE §4.3 rule).
- Timestamps, preferred when present (newest resolution wins):
  NTFS extra (0x000a, tag 1: mtime/atime/ctime FILETIME) — timestamps
  ONLY; any stream sub-blocks in the NTFS extra are NEVER restored or
  emitted (the §4.3 MotW/ADS forgery rule, closed by construction).
  Unix extra (0x5455, UT: mtime, atime, ctime). Old Unix extra
  (0x5450). Fallback: MS-DOS date/time (local-time interpretation,
  documented divergence class; every other zip tool shares it).
  All times clamp through `MtimeBounds` (`timestamp_clamped` flag).

### 2.6 Encryption

GP bit 0 set → per-entry refusal. The AES extra (0x9901) → per-entry
refusal ("AES-ZIP unsupported in Phase 1"). ZipCrypto is additionally
flagged as a legacy weak cipher refused by policy. No foreign
decryption exists in Phase 1; `-p` on a foreign source is a usage
refusal (exit 7).

---

## 3. DEFLATE (RFC 1951)

`src/compress/inflate.{hpp,cpp}` — clean-room decoder, no zlib/puff
code. Bit reader: LSB-first. Block types: 00 stored (LEN/NLEN
complement check), 01 fixed Huffman, 10 dynamic Huffman (HLIT+257 /
HDIST+1 / HCLEN+4, code-length alphabet with the 16/17/18 repeat
codes), 11 → error. Length/distance tables per RFC 1951 §3.2.5.
Distance-too-far, over-subscribed/impossible Huffman sets, truncated
streams, and bad stored-block complements are all hard decode errors.
The decoder carries a MANDATORY in-flight output cap + cumulative
debit (LimitState) wired by the caller; NO declared size is ever a
bound (plan D5 trust model). Known-answer vectors: RFC 1951-derived
streams plus oracle-produced (python zipfile/gzip) streams.

## 4. TAR (ustar + documented GNU/pax subset)

### 4.1 Framing

512-byte blocks. Header: name(100) mode(8) uid(8) gid(8) size(12)
mtime(12) chksum(8) typeflag(1) linkname(100) magic(6 "ustar\0")/
version(2) uname(32) gname(32) devmajor(8) devminor(8) prefix(155).
Numeric fields are octal ASCII (spaces/NUL padded) or GNU base-256
(high bit of first byte set — accepted). Checksum: sum of header bytes
with the chksum field as spaces, stored octal; a header whose checksum
does not validate at dispatch time means "not a TAR"; mid-stream
checksum failure → unparseable (13). End of archive: two zero blocks;
EOF before the padding is ACCEPTED (streamed tapes), EOF mid-header or
mid-payload → exit 3 truncation. Declared size vs remaining input is
enforced.

### 4.2 Typeflags

| Typeflag | Meaning | Reader behavior |
| :--- | :--- | :--- |
| `0`/`\0` | regular file | decode |
| `5` | directory | dir record (trailing slash stripped before composition) |
| `1` | hardlink | FHEXTRA_REDIR type 4 → `linkpath` (target composed through §6) |
| `2` | symlink | FHEXTRA_REDIR type 1/2 (platform) → `linkpath`; `link_migrated` flag |
| `3`/`4`/`6`/`7` | char/block/fifo/contiguous | skip-with-report |
| `L`/`K` | GNU long name/linkname | payload IS the next header's name/linkpath (bounded) |
| `S` | GNU sparse | per-entry refusal (documented subset boundary) |
| `x`/`g` | pax extended header/global | per §4.3 |
| `D`, `M`, `V`, other | GNU dumpdir/multivolume/volume | skip-with-report |

### 4.3 pax subset

Records `len key=value\n`. HANDLED: `path`, `linkpath`, `size`,
`mtime` (float seconds), `atime`, `uid`, `gid`, `uname`, `gname`,
`hdrcharset` (BINARY → name bytes are raw and go through the
percent-encode branch of §6). All other keys are skipped (POSIX
self-describing semantics), record parsing size-bounded. GNU sparse
keys (`GNU.sparse.*`) → per-entry refusal, never silent corruption.

### 4.4 Attributes/timestamps

mode → POSIX mode honored only with type bits (§4.3 rule);
uid/gid/uname/gname → FHEXTRA_UOWNER (restore stays opt-in
downstream); mtime (+pax atime where the staging mechanism can express
it) through `MtimeBounds`.

## 5. GZIP (RFC 1952)

- Header: `1F 8B`, CM=8 (deflate; other → 13), FLG (FTEXT 0x01, FHCRC
  0x02, FEXTRA 0x04, FNAME 0x08, FCOMMENT 0x10), MTIME(4), XFL, OS.
  FEXTRA: bounded skip (XLEN-bounded subfields). FNAME/FCOMMENT:
  zero-terminated Latin-1 → FNAME composes through §6 (treated as
  UTF-8 when well-formed, percent-encoded otherwise); FCOMMENT →
  skip-with-report. FHCRC: header CRC16 verified when set.
- Payload: one RFC 1951 stream per member; **multi-member files are
  ONE logical entry whose payload is the concatenation of member
  payloads** (the interoperable concatenation reading; zcat
  semantics). Trailing garbage shorter than a member header → EOF
  tolerance ends the entry; a malformed member → exit 3.
- Per member: CRC32 + ISIZE (mod 2³²) verified; in-flight output cap +
  cumulative debit mandatory (GZIP declares NO upfront uncompressed
  size — §8). MTIME (header, seconds) through `MtimeBounds`.
- Entry name: FNAME of the FIRST member; absent → the source stem
  convention (strip `.gz`/`.tgz`→`.tar` documented).

## 6. Name composition (ONE pipeline, all formats — plan D14)

`translate_foreign_name(raw_bytes, encoding_hint)`:

1. Decode to a byte string per the source's encoding rule (ZIP §2.4,
   TAR raw/pax/hdrcharset, GZIP FNAME).
2. `\` → `/` normalization (ZIP/TAR; documented lossy rule).
3. Percent-encode invalid UTF-8 sequences (§4.4 — the escaped name IS
   the name).
4. `io::sanitize_archive_path`: resolve `.`/`..` lexically, strip
   drive letters and leading slashes, harden components
   (`make_safe_component`).
5. Empty result → the entry is skipped with report (never emitted
   nameless).

Traversal-shaped names never reach an emitted archive unchanged: cv
emits the already-sanitized name, so downstream extractors never need
to clean our output (§4.1 defense is syscall-level at extraction; cv
refuses to forward weapons regardless).

## 7. Limits (§5.4 — zero relaxation)

Through the new readers: `max_header_count` (entry walk),
`max_header_bytes` (CD/pax/header walk), per-member in-flight output
cap (MANDATORY for inflate/GZIP — declared sizes are never bounds),
cumulative `max_total_output_bytes` debit across decode AND staging
AND re-pack, staging disk-full (ENOSPC) → clean per-entry failure.
Phase-1 cv defaults (documented, non-disableable): member cap 16 GiB,
total cap 1 TiB, entry count 1,000,000, header bytes 256 MiB.
Nested archives are data (no recursion — cv yields entries, never
opens them).

## 8. Security mapping (normative — SECURITY_ARCHITECTURE inheritance)

| Constraint | Foreign-format instance |
| :--- | :--- |
| §2.1 CD-vs-LFH | §2.3 pre-flight gate: mismatch → exit 2 BEFORE any output; integrity failure, never best-effort |
| §4.1 containment | cv materializes ONLY engine-named staging files (`.openrar-cv-*`, OS temp dir); archive names never become local paths; emitted names are pre-sanitized (§6) |
| §4.2 links | symlinks/hardlinks migrate as REDIR records (data in transit); downstream extraction stays default-deny |
| §4.3 metadata trust | modes only with type bits; NTFS-extra stream sub-blocks never restored/emitted; owner records restore opt-in downstream |
| §4.4 names/times | §6 composition; MtimeBounds clamps everywhere |
| §5.3 legacy VM | no legacy decoder exists in Phase 1 — the boundary is provable by absence; per-entry method-named refusals |
| §5.4 limits | §7 |
| §3.1 exit taxonomy | 0 success / 0+report policy skips / 11 encrypted-refused / 2 per-entry failed + structural / 13 unparseable / 3 source CRC-truncation / 7 usage / 10 nothing migrated / 255 cancel — no new codes, no exit-1 mapping (challenge D1) |
| §7.1 render gate | every cv line through `sanitize_for_display`; `--json-summary` valid UTF-8; non-TTY: zero escape bytes, exit parity |
