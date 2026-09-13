# RAR5 Specification — Encryption

RAR5 provides two independent encryption layers, both AES-256-CBC with PBKDF2-HMAC-SHA256. They share primitives but use different salts and scopes.

---

## Primitives

* **Cipher:** AES-256-CBC, 14 rounds, 16-byte block. Zero-padding to 16 bytes (`(n+15)&~15`). Key schedule is encrypt or decrypt depending on direction.
* **KDF:** PBKDF2-HMAC-SHA256, 32-byte output, with two supplementary 32-byte outputs derived by continuing the same HMAC chain (`+16` rounds each). All three outputs are used.
* **Constants:**
  * `SIZE_SALT50 16`, `SIZE_INITV 16`, `SIZE_PSWCHECK 8`, `SIZE_PSWCHECK_CSUM 4`
  * Default `Lg2Count 15` → `32768` iterations (`1<<15`), max `24` → `16M` iterations (higher is rejected)
  * Supported `CryptVersion 0` (AES-256); higher versions are rejected as `Unknown`

### Password Handling

Password is a Unicode string, converted to UTF-8, truncated to `MAXPASSWORD-1` bytes (warning `TruncPsw`). Comparison uses the truncated UTF-8 form.

### PBKDF2-HMAC-SHA256 Derivation

```
SaltData = Salt[16] || 0x00 0x00 0x00 0x01
U1 = HMAC(pwd, SaltData); Fn = U1
For i = 1 .. (1<<Lg2Count)-1:
    U = HMAC(pwd, Uprev); Fn ^= U
Key = Fn  (32 bytes, after Count rounds)

For +16 rounds:  HashKey = Fn  (32 bytes)
For +16 rounds:  PswSeed = Fn  (32 bytes)
PswCheck[8]: PswCheck[j] = XOR over PswSeed[i] where i%8==j (fold 32→8)
PswCheckCsum = SHA256(PswCheck)[0..3] (4 bytes)
```

The three outputs (`Key`, `HashKey`, `PswCheck`) are derived sequentially from the same chain; `Key` is the encryption key, `HashKey` tweaks file checksums, `PswCheck` is the password verifier.

A 4-entry LRU cache of `(pwd, Salt, Lg2Count) → (Key, HashKey, PswCheck)` is allowed but wire-invisible.

---

## Key Scopes

| Scope | When | Salt / IV Scope | Data Protected |
|-------|------|-----------------|----------------|
| **Header encryption (`-hp`)** | Archive created with header encryption flag | Archive-wide `HeaderSalt` (16) + per-block 16-byte clear IV prepended to each encrypted header | All headers after the encryption header: `Main`, `File`/`Service`, `End`, and their extra/data areas |
| **File data encryption (`-p`)** | Per-file when password is set | Per-file `FileSalt` (16) + `FileInitV` (16) in `FHEXTRA_CRYPT` | File payload (compressed or stored bytes), zero-padded to 16 |

Both may be active simultaneously; header encryption always implies file encryption for files written after it.

When header encryption is active, recovery record (`RR`) and quick-open (`QO`) are **not** created (locator offsets are `0`).

---

## Header Formats

### Archive Encryption Header (`type 4`, never encrypted)

| Field | Size | Description |
|-------|------|-------------|
| Encryption version | `vint` | `0` (reject `>0`) |
| Encryption flags | `vint` | `0x0001` = `PswCheck` present |
| KDF count | `byte` | `log2` iterations (`1<<Lg2Count`); reject `>24` |
| Salt | `16` | Archive-wide header salt |
| Check value | `12` | If `0x0001` set: `PswCheck[8]` + `PswCheckCsum[4]` (`SHA256(PswCheck)[0..3]`) |

Block framing: standard block prefix (`[Header CRC32|Header size|Header type 4|Header flags]`) + `Body` above as `[BodySize vint][Body bytes]`. This block itself is never encrypted; the 16-byte IV for the *next* header is stored in clear before that header’s bytes.

**Checksum gate:** On read, compute `SHA256(PswCheck)[0..3]` and compare to stored `PswCheckCsum`. Mismatch does **not** fail immediately — it disables `UsePswCheck`, letting the header CRC decide (damaged `PswCheck` is treated as absent).

### File Encryption Extra Record (`0x01`, in file extra `HFL_EXTRA`)

| Field | Size | Description |
|-------|------|-------------|
| Size | `vint` | From `Type` onward |
| Type | `vint` | `0x01` |
| Version | `vint` | `0` (reject `>0` as `CRYPT_UNKNOWN`) |
| Flags | `vint` | `0x0001` `PswCheck` present, `0x0002` `HASHMAC` (tweaked checksums) |
| KDF count | `byte` | `log2` (`>24` → `CRYPT_UNKNOWN`) |
| Salt | `16` | File salt |
| IV | `16` | File IV (`InitV`) |
| Check value | `12` | If `0x0001` set: `PswCheck[8]` + `PswCheckCsum[4]` |

If the stored `PswCheck` is `8` zero bytes in a service header (historical RAR 5.21 bug), `UsePswCheck` is forced `false`.

---

## Integrity With Encryption

### Header Integrity

* After decrypting a header block (AES-256-CBC with `Key` and the clear IV prepended to the block), verify the block’s `Header CRC32`. Failure after a correct `PswCheck` means corrupt archive; failure without `UsePswCheck` may be wrong password.
* Early password check: if `UsePswCheck` is true, derive `PswCheck` from the supplied password and compare to stored `PswCheck`. Mismatch → `BADPSW` before attempting header CRC. If `UsePswCheck` is false, skip early check and rely on header CRC (which still requires the correct key).

### File Integrity (`HASHMAC`)

When `Flags & 0x0002` (`HASHMAC`) is set, the stored file checksums are *key-dependent*:

* `HASH_CRC32`: `rawCRC = IEEE CRC32(unpacked)` as `LE32` bytes; `digest = HMAC-SHA256(HashKey, rawCRC[4])`; `storedCRC = XOR-fold 32→4` (`for i 0..31: stored ^= digest[i] << ((i&3)*8)`).
* `HASH_BLAKE2`: `digest = HMAC-SHA256(HashKey, BLAKE2sp[32])` (full 32 bytes, replaces the digest).

Writers compute `raw` hash, then tweak via `HashKey` before storing; readers re-derive `HashKey` and verify the tweaked value. Without `HASHMAC`, the stored hashes are plain.

Service headers (`STM`, `ACL`) and recovery data are not HASHMAC-tweaked.

---

## Encryption Flow

### Writer (create)

```
if EncryptHeaders:
    HeaderSalt = random(16); HeaderLg2 = 15
    Key, HashKey, PswCheck = derive(pwd, HeaderSalt, 15)
    PswCheckCsum = SHA256(PswCheck)[0..3]
    emit HEAD_CRYPT with HeaderSalt, 15, PswCheck+PswCheckCsum
    # RR and QO suppressed
for each file:
    if EncryptFiles:
        FileSalt, FileInitV = random(16), random(16)
        Key, HashKey, PswCheck = derive(pwd, FileSalt, 15)
        rawCRC = CRC32(unpacked); rawBLAKE = BLAKE2sp(unpacked)
        if HASHMAC: storedCRC/BLAKE = HMAC(HashKey, raw)
        padded = pad(payload, 16); encrypted = AES-CBC(Key, FileInitV, padded)
        emit FHEXTRA_CRYPT with FileSalt, FileInitV, 15, PswCheck+PswCheckCsum, HASHMAC
        payload = encrypted
```

### Reader (extract/test)

```
if block type == HEAD_CRYPT:
    if CryptVersion>0 or Lg2Count>24: UnknownVersion → FailedHeaderDecryption
    else UsePswCheck = (Flags & 0x0001) && SHA256(PswCheck)[0..3]==PswCheckCsum
    next headers: IV = next 16 clear bytes; decrypt block with Key derived from pwd+Salt
    if UsePswCheck and derived PswCheck != stored: BADPSW (or prompt again if manual)
    else if Header CRC mismatch: BADPSW or corrupt

if file extra 0x01:
    if CryptVersion>0 or Lg2Count>24: CRYPT_UNKNOWN → skip file, warn
    else if UsePswCheck and SHA256(PswCheck)[0..3]!=stored: UsePswCheck=false
         (and if 8 zero bytes in service header: UsePswCheck=false)
    Key, HashKey, PswCheck = derive(pwd, FileSalt, Lg2Count)
    if UsePswCheck and PswCheck != stored: BADPSW
    else decrypt payload (AES-CBC, FileInitV), unpack, verify HASHMAC tweak
```

All key material must be wiped after use.

---

## Limits and Compatibility

* `CryptVersion>0` or `Lg2Count>24` is a hard unknown-version error (do not attempt decrypt).
* RAR 1.5–4 filename encryption (`EncName`, `LHD_UNICODE`) does not exist in RAR5; names are in `HEAD_FILE` which is encrypted wholesale under `-hp`.
* Password length `MAXPASSWORD` truncation is identical for header and file KDFs; excess is silently cut with warning.
