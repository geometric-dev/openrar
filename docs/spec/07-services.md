# RAR5 Specification — Services: ACL, Streams, Redirection, Owner

Service headers (`type 3`, `HFL_CHILD`) store out-of-band file metadata. They are **not** required for basic file extraction — a decoder that skips unknown service headers remains compliant — but a complete implementation must handle them to round-trip permissions, alternate streams, and links exactly as reference archivers do.

All service payloads are `HFL_DATA` child blocks of the preceding file header. Their wire format reuses the file header framing (`01-headers.md`) with `Name` = service identifier (`ACL`, `STM`, etc.) and `Host OS` = `0` Windows or `1` Unix as appropriate. `Method` is `0` (store) for `ACL`/`STM`/`RR`; `CompInfo` still carries a valid dictionary hint.

---

## 1. Unix Owner and Permissions (`chmod` / `chown`)

### File Attributes (`Attributes` field, `vint`)

* **Windows (`Host OS 0`):** `FILE_ATTRIBUTE_*` bits (`0x01` read-only, `0x02` hidden, `0x04` system, `0x10` directory, `0x20` archive, `0x80` normal, etc.). Preserved verbatim; decoders call `SetFileAttributesW`.
* **Unix (`Host OS 1`):** low 16 bits are `st_mode` (`0xF000` type, `0x0FFF` permissions). `0x8000` regular file, `0x4000` directory, `0xA000` symlink (see §4), `0x1FF` = `rwxrwxrwx`. `chmod 644` → `0x81A4`, `755` → `0x81ED`.

### Unix Owner Extra Record (`0x06`)

Carries names and numeric IDs. Present when `-ow` or default owner preservation is active.

| Field | Size | Description |
|-------|------|-------------|
| Size | `vint` | From `Type` onward. |
| Type | `vint` | `0x06`. |
| Flags | `vint` | `0x0001` user name present, `0x0002` group name, `0x0004` numeric UID, `0x0008` numeric GID. |
| User name length | `vint` | If `0x0001` set. |
| User name | bytes | Native encoding (not UTF-8), `length` bytes, not NUL-terminated. |
| Group name length | `vint` | If `0x0002` set. |
| Group name | bytes | As above. |
| UID | `vint` | If `0x0004` set. `uint32` user ID. |
| GID | `vint` | If `0x0008` set. `uint32` group ID. |

If both name and numeric ID are present, decoders should try numeric `chown` first, then fall back to name lookup. Extraction requires privilege (`CAP_CHOWN`); failure is non-fatal — warn and continue.

#### Encode (Write) — Unix Owner

```
stat(path, &st)  // or lstat for symlink itself
uid = st.st_uid; gid = st.st_gid
user = getpwuid(uid) → pw_name (native encoding); group = getgrgid(gid) → gr_name
Flags = 0
if user: Flags|=0x0001; store User name length + bytes
if group: Flags|=0x0002; store Group name length + bytes
if uid valid: Flags|=0x0004; store UID vint
if gid valid: Flags|=0x0008; store GID vint
if Flags==0: omit record entirely
Attributes (low 12 bits) already carry st_mode & 0xFFF (permissions) + type bits; stored in file header Attributes vint
```

Only emitted when archive is created on Unix with owner preservation (default) or `-ow`. On Windows, this extra is never emitted.

#### Decode (Extract) — Unix Owner

```
Flags = vint; parse fields in order per Flags
if 0x0001: User name length + bytes → native → Wide (for logging)
if 0x0002: Group name length + bytes
if 0x0004: UID vint
if 0x0008: GID vint
// apply
if 0x0004 || 0x0008: chown(path, UID if 0x0004 else -1, GID if 0x0008 else -1) // numeric first; -1 means unchanged
  if chown fails with EPERM: warn, try name lookup: getpwnam/getgrnam → chown by name
if Attributes present: chmod(path, Attributes & 0x1FF) // low 9 bits; use fchmodat with AT_SYMLINK_NOFOLLOW for symlinks
```

`chmod` is applied after `chown`; on symlinks use `lchmod`/`fchmodat` to avoid following. Failures are non-fatal (warn). Windows decoders ignore `0x06` entirely.

### High-Precision Times

`FHEXTRA_HTIME` (`0x03`) carries `ctime`/`atime` and sub-second `mtime` refinement (see `01-headers.md`). `mtime` base is in the file header `mtime` (`FHFL_UTIME`); refinement is in the extra. `lutimes` with `AT_SYMLINK_NOFOLLOW` is used for symlinks.

---

## 2. NTFS ACL (`ACL` service, `-ow`)

### Wire

```
File header N (HEAD_FILE)
  └─ Service header "ACL" (HEAD_SERVICE, HFL_CHILD|HFL_DATA, HFL_INHERITED, NameLen=3, Name="ACL", Host OS 0)
       Extra area: none (no FHEXTRA_SUBDATA)
       Data area: self-relative SECURITY_DESCRIPTOR blob, little-endian (see below), PackSize==UnpSize==blob size, Method 0, CRC32 of blob
File header N+1
```

Extra flags `HFL_INHERITED` ensures the `ACL` moves with its host on archive update.

### SECURITY_DESCRIPTOR Layout (data area, verbatim)

As returned by `GetFileSecurityW` with `OWNER|GROUP|DACL` and optionally `SACL` (if `SE_SECURITY_NAME` privilege held):

```
Revision:1, Sbz1:1, Control:2, OwnerOff:4, GroupOff:4, SaclOff:4, DaclOff:4  // 20 bytes LE
+ Owner SID at OwnerOff (variable, SID structure)
+ Group SID at GroupOff
+ SACL at SaclOff (if present)
+ DACL at DaclOff
```

All offsets are from start of descriptor, little-endian `uint32`. `Control` bits indicate which parts are present. SACL is included only when the writer held `SE_SECURITY_NAME`; otherwise the descriptor is `OWNER|GROUP|DACL` only (3-part). Decoders must accept either; writers that lack privilege simply omit `SACL`.

### Semantics

* **Probe:** `GetFileSecurityW(path, OWNER|GROUP|DACL[|SACL], NULL, 0)` returns `ERROR_INSUFFICIENT_BUFFER` and `need`; second call fills `need` bytes. Long paths are retried with `\\?\` prefix.
* **Size limits:** reject `need==0` or `>1 MiB`.
* **Store:** `Method 0`, `PackSize==UnpSize==need`, no compression, no filter.
* **Restore:** `SetFileSecurityW(dest, OWNER|GROUP|DACL[|SACL], sd)` — same `si` used on save (include `SACL` only if `SACL` was present in the blob). `\\?\` long-path retry on failure. Missing privilege: warn `ACCESS_DENIED` → `UIERROR_NEEDADMIN` if not admin; otherwise non-fatal.
* **Listing:** `rar l -v` does not expand ACL content; use `rar vta` to see extra sizes.
* **Security:** `SE_SECURITY_NAME`, `SE_RESTORE_NAME`, `SE_BACKUP_NAME` privileges are acquired once per process and cached.

A minimal decoder may skip `ACL` entirely — files extract without ACL and remain usable, but `rar t` will still verify its CRC.

---

## 3. NTFS Alternate Data Streams (`STM` service, `-os`)

NTFS stores per-file alternate streams as `filename:stream:$DATA`. The default stream (`:$DATA` with empty name) is the file body; each `BACKUP_ALTERNATE_DATA (4)` with non-empty name is an ADS.

### Wire

```
File header N
  ├─ Service header "STM" × K (each HFL_CHILD|HFL_INHERITED|HFL_DATA, NameLen=3, Name="STM")
  │    Extra area: FHEXTRA_SUBDATA (0x07) with UTF-8 stream name including leading ':', e.g. ":Zone.Identifier" (trailing ":$DATA" stripped case-insensitively)
  │    Data area: PackSize==UnpSize==stream size, Method 0, CRC32 of stream bytes
  └─ ...
File header N+1
```

Order follows `BackupRead` enumeration.

### Stream Name (extra `0x07`)

`FHEXTRA_SUBDATA` record: `Size`, `Type 0x07`, then UTF-8 bytes of the stream name (e.g., `:Zone.Identifier`). Writers strip the trailing `:$DATA` suffix case-insensitively — `:BranchCache:$DATA` is stored as `:BranchCache`. The `5.21` historical bug (field size off by one) requires decoders to accept a trailing `1`-byte remainder as part of the extra.

### Size Limits and Safety

* **Save:** streams `>1 GiB` (`0x40000000`) are skipped to avoid buffer explosion. Prohibited names (`ColonCount>1`, empty, or path separators `/ \` in the stream part) are skipped. `BACKUP_DATA (1)` (main file data) is skipped via `BackupSeek`.
* **Extract:** per-stream `UnpSize` is capped at `16 MiB` (`0x1000000`) when reading into memory (e.g., for MOTW evaluation) to prevent DoS. Larger streams are still extracted via streaming to file, not buffered.
* **Enumeration** requires `CreateFileW` with `FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_SEQUENTIAL_SCAN` and `BackupRead`/`BackupSeek` with `\\?\` long-path fallback.

### Extraction

```
stream_name = UtfToWide(SubData)  // e.g. ":Zone.Identifier"
if stream_name[0] != ':' or '/' in stream_name or '\\' in stream_name: skip (BROKEN)
if ColonCount>1: skip (IsNtfsProhibitedStream)
if test_mode: ReadSubData(NULL, discard); return
if Motw.IsNameConflicting(stream_name):
    fileMotw = ReadSubData() (≤16 MiB)
    if !Motw.IsFileStreamMoreSecure(fileMotw): skip (already has MOTW)
full = fileName + stream_name  // "file.txt" + ":Zone.Identifier" = "file.txt:Zone.Identifier"
if host file is read-only: clear read-only, remember to restore
CreateFileW(full); if Motw, write parsedMotw else stream bytes via ReadSubData
restore timestamps/attributes of host file
```

Streams are created only after the host file exists; `READONLY` is cleared before and restored after. Long paths use `\\?\` prefix. `HFL_CHILD|HFL_INHERITED` ensures streams move with host on update.

A decoder that does not support ADS may skip all `STM` blocks — the host file remains correct and `rar t` validates their CRCs; `rar l -vta` annotates `Type: NTFS alternate data stream`.

### OpenRAR v1.27 zone-stream policy (SECURITY_ARCHITECTURE §4.3)

`Zone.Identifier` is transport provenance, not file content. OpenRAR therefore
departs from WinRAR here, deliberately:

* **Capture:** OpenRAR's `-os` excludes `:Zone.Identifier` entirely (any legal
  spelling, case-insensitive, with or without the `:$DATA` suffix). WinRAR
  `-os` archives may still carry zone streams — OpenRAR re-emits them
  verbatim on mutation but never restores them.
* **Restore:** extraction skips any zone-named STM child *before* reading its
  payload, regardless of producer, and reports the skip (`W:` line +
  `zone_stream_skipped` JSON flag).
* **Propagation:** the `-oz` policy instead keys on the ARCHIVE FILE's own
  Zone.Identifier ADS / `com.apple.quarantine` xattr (transport context) and
  writes freshly generated mark content — `[ZoneTransfer]
ZoneId=N
`,
  nothing else — per extracted file. WinRAR's "restore archive-provided zone
  if more secure than host" algorithm is not implemented: attacker-chosen
  zone content never reaches disk in either direction.

---

## 4. Redirection — Symlinks, Junctions, Hard Links, File Copy (`-ol`, `-oi`, `-oh`)

Redirections are stored as file headers **with no data area** (`HFL_DATA` clear, `PackSize=-1`, `UnpSize=0`, `Method 0`, `FHFL_CRC32` clear). The link target is the extra record.

### Types

| Value | Name | Platform | Stored Target |
|-------|------|----------|---------------|
| `0` | `NONE` | — | regular file/dir |
| `1` | `UNIXSYMLINK` | Unix | `readlink` target |
| `2` | `WINSYMLINK` | Windows | `IO_REPARSE_TAG_SYMLINK` PrintName |
| `3` | `JUNCTION` | Windows | `IO_REPARSE_TAG_MOUNT_POINT` PrintName (always dir) |
| `4` | `HARDLINK` | any | archived name of hardlink target file |
| `5` | `FILECOPY` | any | same as `HARDLINK` but `FILE_ATTRIBUTE_REPARSE_POINT` not set or dedup fallback |

`DirTarget` flag (`FHEXTRA_REDIR_DIR 0x0001`) indicates the target is a directory.

### Extra Record `0x05` (`FHEXTRA_REDIR`)

Inside the file header extra area, one record:

```
Size: vint (from Type onward)
Type: vint 0x05
Content:
  RedirType: vint 1..5
  Flags: vint (bit0 = DirTarget)
  NameSize: vint (<2048 else ignore record)
  Utf8Target: NameSize bytes, forward slashes, no NUL
Size includes the Type byte.
```

Legacy RAR 1.5 Unix symlink: `HostOS==UNIX` and `(FileAttr & 0xF000)==0xA000` and **no** `0x05` record; the symlink target was the *compressed* file data (hashed, not extra). Modern decoders must still handle both.

### Write Path

*Scan*: `lstat` (Unix, not `stat`) or `GetFileAttributesW` + `FILE_FLAG_OPEN_REPARSE_POINT` (Windows) to detect `IsLink` without following. For Windows, `FSCTL_GET_REPARSE_POINT` yields `REPARSE_DATA_BUFFER`; `PrintName` (or `SubstituteName` if `PrintName` empty) is the target. For junctions the target is always a directory.

*Hardlink deduplication* (`-oh`, `-oi[0-4]`): a `hardlinks` table maps inode/`FileIndex` to first archived name; later links emit `HARDLINK` (`4`) or `FILECOPY` (`5`) with `RedirName` = first file’s archived UTF-8 name. File copies use stream copy; hard links use `CreateHardLinkW`/`link()`.

*Header*: `PackSize=-1`, `UnpSize=0`, `Method 0`, `FileFlags&=~FHFL_CRC32`, `Attributes` preserves directory bit for dir-targets, `RedirType`/`RedirName`/`DirTarget` stored as above, serialized via `MakeFileBody` → `WriteFileHeader50`. No `DataCRC`/`BLAKE2`.

### Extraction and Safety

Parsing:

```
RedirType = vint; Flags = vint; NameSize = vint; Utf8 = bytes → Wide, Windows: UnixSlashToDos()
DirTarget = Flags&1; RedirName = Utf8; Max NameSize 2047 else ignore
```

Decision:

```
isLink = RedirType != NONE
if isLink && type != FILECOPY:
  if SkipSymLinks (-ol-): skip
  if !AbsoluteLinks && (IsFullPath(RedirName) || !IsRelativeSymlinkSafe(FileName, DestPath, RedirName)): warn UIERROR_SKIPUNSAFELINK, skip
  PrepareToDelete(dest)
if HARDLINK or FILECOPY:
  src = DestRoot + RedirName (SlashToNative, ConvertPath)
  hardlink: CreateHardLink(new, src)
  filecopy: copy src→new (stream, or move temp ref via RefList)
else // UNIXSYMLINK / WINSYMLINK / JUNCTION:
  CreateReparsePoint(dest)  // see below
```

`CreateReparsePoint` (Windows):

```
Require SE_RESTORE_NAME + SE_CREATE_SYMBOLIC_LINK_NAME
Subst = RedirName; Print = strip "\??\" prefix (4 chars) if present, fix UNC \\?\→\\
Abs = IsPrefix("\??\")
if !AbsoluteLinks && (Abs || IsFullPath || !IsRelativeSymlinkSafe): skip
CreatePath(dest, skipLast=true); Delete existing
if Dir || DirTarget: CreateDirectory(dest) else CreateFile(CREATE_NEW)
Build REPARSE_DATA_BUFFER:
  junction: tag=MOUNT_POINT, lengths/offsets for Subst/Print
  symlink: tag=SYMLINK, Flags = Abs?0:SYMLINK_FLAG_RELATIVE(1)
FSCTL_SET_REPARSE_POINT on handle opened with FILE_FLAG_OPEN_REPARSE_POINT
Set times/attrs; on failure delete placeholder
```

Unix symlink:

```
WideToChar(dest) + Utf8ToChar(target) → symlink(target, link); CreatePath; DelFile first
lutimes with AT_SYMLINK_NOFOLLOW for atime/mtime when available
```

Safety flags:

* `FHEXTRA_REDIR_DIR` is advisory; `FHFL_DIRECTORY` is separate.
* `IsRelativeSymlinkSafe`: counts `..` components in target vs `CalcAllowedDepth(srcName)`; also scans `DestPath` for existing symlink parents (`LinksToDirs` — any destination component that is itself a link denies). After each `..` extraction, `LinksToDirs` is re-scanned and any symlink prefix is replaced with a real directory.
* Hardlink/filecopy via `RefList` avoids double extraction.

A decoder with `SkipSymLinks` or without reparse privilege must skip `1..3` and warn; `4..5` may still be extracted as regular files (copy).

---

## Clean-Room Checklist

* Unix owner/attributes: decode `Attributes` + `0x06` extra; `chmod`/`chown` on Unix, `SetFileAttributesW` on Windows.
* ACL: skip `ACL` if not on Windows or without privilege; otherwise restore via descriptor.
* Streams: skip `STM` if not on NTFS; otherwise `CreateFileW(full)` per stream.
* Redirection: enforce `NameSize<2048`, `IsRelativeSymlinkSafe`, `LinksToDirs`, long-path `\\?\` retry, and `HARDLINK` vs `FILECOPY` distinction.
