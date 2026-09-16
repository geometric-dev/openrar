# RAR 5.0 archive format

Basic data structures of the archive format introduced in RAR 5.0.
For algorithms and format details see the RAR5 format specification documents in `docs/spec/`.

## Contents

- Data types
  - vint
  - byte, uint16, uint32, uint64
- Variable length data
- Hexadecimal values
- General archive structure
  - General archive block format
  - General extra area format
  - General archive layout
- Archive blocks
  - Self-extracting module (SFX)
  - RAR 5.0 signature
  - Archive encryption header
  - Main archive header
    - Extra record types: Locator record, Metadata record
  - File header and service header
    - Extra record types: File encryption, File hash, File time, File version,
      File system redirection, Unix owner, Service data
  - End of archive header
- Service headers
  - Archive comment header
  - Quick open header

## Data types

### vint

Variable length integer. One or more bytes; lower 7 bits of every byte contain
integer data, highest bit of every byte is the continuation flag. If the highest
bit is 0, this is the last byte in the sequence. The first byte contains the 7
least significant bits of the integer and the continuation flag; the second
byte, if present, contains the next 7 bits, and so on.

Currently RAR format uses vint to store up to 64-bit integers, resulting in
10 bytes maximum. This value may be increased in the future.

Sometimes RAR needs to pre-allocate space for a vint before knowing its exact
value. In such situations it may allocate more space than really necessary and
fill several leading bytes with 0x80 hexadecimal, which means 0 with the
continuation flag set.

### byte, uint16, uint32, uint64

Byte, 16-, 32-, 64-bit unsigned integer in little endian format.

## Variable length data

Ellipsis `...` denotes variable length data areas.

## Hexadecimal values

`0x` prefix defines hexadecimal values, such as `0xf000`.

## General archive structure

### General archive block format

| Field        | Size  | Description |
|--------------|-------|-------------|
| Header CRC32 | uint32 | CRC32 of header data starting from Header size field and up to and including the optional extra area. |
| Header size  | vint  | Size of header data starting from Header type field and up to and including the optional extra area. Must not be longer than 3 bytes in current implementation (2 MB maximum header size). |
| Header type  | vint  | Type of archive header:<br>1 — Main archive header<br>2 — File header<br>3 — Service header<br>4 — Archive encryption header<br>5 — End of archive header |
| Header flags | vint  | Flags common for all headers:<br>0x0001 — Extra area is present in the end of header<br>0x0002 — Data area is present in the end of header<br>0x0004 — Blocks with unknown type and this flag must be skipped when updating an archive<br>0x0008 — Data area continues from previous volume<br>0x0010 — Data area continues in next volume<br>0x0020 — Block depends on preceding file block<br>0x0040 — Preserve a child block if host block is modified |
| Extra area size | vint | Size of extra area. Optional, present only if 0x0001 header flag is set. |
| Data size    | vint  | Size of data area. Optional, present only if 0x0002 header flag is set. |
| ...          | ...   | Fields specific for current block type. |
| Extra area   | ...   | Optional area containing additional header fields, present only if 0x0001 header flag is set. |
| Data area    | ...   | Optional data area, present only if 0x0002 header flag is set. Used to store large data amounts, such as compressed file data. Not counted in Header CRC and Header size fields. |

### General extra area format

Extra area can include one or more records having the following format:

| Field | Size | Description |
|-------|------|-------------|
| Size  | vint | Size of record data starting from Type. |
| Type  | vint | Record type. Different archive blocks have different associated extra area record types. Unknown record types need to be skipped without interrupting an operation. |
| Data  | ...  | Record dependent data. May be missing if record consists only of size and type. |

### General archive layout

```
Self-extracting module (optional)
RAR 5.0 signature
Archive encryption header (optional)
Main archive header
Archive comment service header (optional)

File header 1
Service headers (NTFS ACL, streams, etc.) for preceding file (optional)
...
File header N
Service headers (NTFS ACL, streams, etc.) for preceding file (optional)

Recovery record (optional)
End of archive header
```

## Archive blocks

### Self-extracting module (SFX)

Any data preceding the archive signature. SFX module size and contents are not
defined. RAR assumes the maximum SFX module size does not exceed 1 MB, but this
value can increase in the future.

### RAR 5.0 signature

8 bytes: `52 61 72 21 1A 07 01 00`. Search for this signature from the
beginning up to maximum SFX module size. For comparison, the RAR 4.x signature
is 7 bytes: `52 61 72 21 1A 07 00`.

### Archive encryption header

| Field              | Size    | Description |
|--------------------|---------|-------------|
| Header CRC32       | uint32  | |
| Header size        | vint    | |
| Header type        | vint    | 4 |
| Header flags       | vint    | Flags common for all headers |
| Encryption version | vint    | Version of encryption algorithm. Currently only 0 (AES-256). |
| Encryption flags   | vint    | 0x0001 — Password check data is present. |
| KDF count          | 1 byte  | Binary logarithm of iteration number for PBKDF2. RAR can refuse to process KDF count exceeding a version-dependent threshold. |
| Salt               | 16 bytes | Salt value used globally for all encrypted archive headers. |
| Check value        | 12 bytes | Present only if 0x0001 encryption flag is set. Verifies password validity. First 8 bytes calculated using additional PBKDF2 rounds, last 4 bytes are an additional checksum. See `docs/spec/08-encryption.md` for algorithm details. |

Present only in archives with encrypted headers. Every next header after this
one starts from a 16-byte AES-256 initialization vector followed by encrypted
header data. Encrypted header data block size is aligned to a 16-byte boundary.

### Main archive header

| Field         | Size | Description |
|---------------|------|-------------|
| Header CRC32  | uint32 | |
| Header size   | vint  | |
| Header type   | vint  | 1 |
| Header flags  | vint  | Flags common for all headers |
| Extra area size | vint | Optional, present only if 0x0001 header flag is set. |
| Archive flags | vint | 0x0001 — Volume. Archive is part of a multivolume set.<br>0x0002 — Volume number field is present. Present in all volumes except first.<br>0x0004 — Solid archive.<br>0x0008 — Recovery record is present.<br>0x0010 — Locked archive. |
| Volume number | vint | Optional, present only if 0x0002 archive flag is set. Not present for first volume, 1 for second volume, 2 for third and so on. |
| Extra area    | ...  | Optional, present only if 0x0001 header flag is set. |

#### Extra area of main archive header

| Type | Name     | Description |
|------|----------|-------------|
| 0x01 | Locator  | Positions of different service blocks for quick access without scanning the entire archive. Optional; if missing the whole archive must be scanned to verify presence of service blocks. |
| 0x02 | Metadata | Optional record storing archive metadata: original archive name and time. |

##### Locator record

| Field                  | Size | Description |
|------------------------|------|-------------|
| Size                   | vint | |
| Type                   | vint | 1 |
| Flags                  | vint | 0x0001 — Quick open record offset is present<br>0x0002 — Recovery record offset is present |
| Quick open offset      | vint | Distance from beginning of quick open service block to beginning of main archive header. Present only if 0x0001 flag is set. 0 means ignore (preallocated space was not enough). |
| Recovery record offset | vint | Distance from beginning of recovery record service block to beginning of main archive header. Present only if 0x0002 flag is set. 0 means ignore. |

##### Metadata record

| Field       | Size      | Description |
|-------------|-----------|-------------|
| Size        | vint      | |
| Type        | vint      | 2 |
| Flags       | vint      | 0x0001 — Archive name is present<br>0x0002 — Archive original creation time is present<br>0x0004 — Use Unix time if 1, Windows FILETIME if 0<br>0x0008 — Defines Unix time as nanoseconds since 1970-01-01 if 1, seconds if 0 |
| Name length | vint      | Original archive name length. Present if flag 0x0001 is set. |
| Name        | ? bytes   | UTF-8, length = Name length. Trailing zero normally missing; trailing zeros possible from size overprovisioning — truncate at first zero. If initially reserved buffer was insufficient, first byte is 0 meaning no name stored even if length non-zero. |
| Time        | 4 or 8 bytes | 8-byte Windows FILETIME if flag 0x0004 is 0, else Unix format: 4-byte seconds if 0x0008 is 0 or 8-byte nanoseconds if 0x0008 is 1. |

### File header and service header

Both header types share the same structure.

| Field            | Size | Description |
|------------------|------|-------------|
| Header CRC32     | uint32 | |
| Header size      | vint  | |
| Header type      | vint  | 2 for file header, 3 for service header |
| Header flags     | vint  | Flags common for all headers |
| Extra area size  | vint  | Optional, present only if 0x0001 header flag is set. |
| Data size        | vint  | Optional, present only if 0x0002 header flag is set. For file header: packed file size. |
| File flags       | vint | 0x0001 — Directory file system object (file header only)<br>0x0002 — Time field in Unix format is present<br>0x0004 — CRC32 field is present<br>0x0008 — Unpacked size is unknown |
| Unpacked size    | vint | Unpacked file or service data size. |
| Attributes       | vint | OS-specific file attributes for file header; may be data-specific or reserved 0 for service header. |
| mtime            | uint32 | File modification time, Unix time. Optional, present if 0x0002 file flag is set. |
| Data CRC32       | uint32 | CRC32 of unpacked file or service data. For files split between volumes: CRC32 of packed data in current volume for all parts except the last. Optional, present if 0x0004 file flag is set. |
| Compression information | vint | See below. |
| Host OS          | vint | 0x0000 — Windows, 0x0001 — Unix. |
| Name length      | vint | File or service header name length. |
| Name             | ? bytes | UTF-8 without trailing zero. Forward slash is the path separator for both Unix and Windows names. Backslashes are part of the name for Unix names, invalid for Windows names. |
| Extra area       | ...  | Optional, present only if 0x0001 header flag is set. |
| Data area        | ...  | Optional, present only if 0x0002 header flag is set. Store file data (file header) or service data (service header). Uncompressed (method 0) or compressed depending on Compression information. |

If file flag 0x0008 (unpacked size unknown) is set, the unpacked size field is
still present but must be ignored; extraction runs until end of compression
stream. Used when actual file size exceeds reported OS size or is unknown
(e.g. stdin to multivolume archive, all volumes except last).

#### Compression information

- Lower 6 bits (mask `0x003f`): version of compression algorithm (0–63).
  Currently 0 and 1 are possible. Version 0 archives unpack with RAR 5.0+;
  version 1 requires RAR 7.0+.
- Bit 7 (`0x0040`): solid flag. RAR continues using the compression dictionary
  left after processing preceding files. Only for file headers, never service.
- Bits 8–10 (mask `0x0380`): compression method. Values 0–5 used. 0 = no
  compression (store).
- Bits 11–15 (mask `0x7c00`): minimum dictionary size required to extract.
  N means dictionary size is 128 KB * 2^N: 0 = 128 KB, 1 = 256 KB, ..., 15 =
  4096 MB, ..., 19 = 64 GB, 23 = 1 TB (field maximum). Actual implementations
  may have lower limits. Values above 15 only used with algorithm version 1.
- Bits 16–20 (mask `0xf8000`, version 1 only): multiplier applied to dictionary
  size in bits 11–15, divided by 32, added to dictionary size. Allows up to 31
  intermediate sizes between neighbouring powers of 2.
- Bit 21 (`0x100000`, version 1 only): dictionary size flags are in version 1
  format but actual compression algorithm is version 0. Useful when appending
  version 1 files to an existing version 0 solid stream needing a larger
  dictionary without touching version 0 compressed data.

Host OS values: `0x0000` Windows, `0x0001` Unix.

Service header names currently used:

| Name | Meaning |
|------|---------|
| CMT  | Archive comment |
| QO   | Archive quick open data |
| ACL  | NTFS file permissions |
| STM  | NTFS alternate data stream |
| RR   | Recovery record |

#### File and service extra area records

| Type | Name            | Description |
|------|-----------------|-------------|
| 0x01 | File encryption | File encryption information. |
| 0x02 | File hash       | File data hash. |
| 0x03 | File time       | High precision file time. |
| 0x04 | File version    | File version number. |
| 0x05 | Redirection     | File system redirection. |
| 0x06 | Unix owner      | Unix owner and group information. |
| 0x07 | Service data    | Service header data array. |

##### File encryption record

| Field     | Size     | Description |
|-----------|----------|-------------|
| Size      | vint     | |
| Type      | vint     | 0x01 |
| Version   | vint     | Encryption algorithm version. Currently only 0 (AES-256). |
| Flags     | vint     | 0x0001 — Password check data is present<br>0x0002 — Use tweaked checksums instead of plain checksums (checksum becomes dependent on encryption key so file contents cannot be guessed from checksums; affects data CRC32 in file header and file hash record checksums) |
| KDF count | 1 byte   | Binary logarithm of PBKDF2 iteration number. Threshold version dependent. |
| Salt      | 16 bytes | Salt for file decryption key. |
| IV        | 16 bytes | AES-256 initialization vector. |
| Check value | 12 bytes | Present only if 0x0001 flag set. First 8 bytes via additional PBKDF2 rounds, last 4 an additional checksum; combined with header CRC32 gives 64-bit integrity/password check. See `docs/spec/08-encryption.md`. |

##### File hash record

Only standard CRC32 can be stored directly in the file header; other hashes go here.

| Field    | Size    | Description |
|----------|---------|-------------|
| Size     | vint    | |
| Type     | vint    | 0x02 |
| Hash type| vint    | 0x00 — BLAKE2sp hash function. |
| Hash data| ? bytes | 32 bytes of BLAKE2sp for hash type 0x00. |

For split files: hash of packed data in current volume for all parts except the
last; otherwise hash of unpacked data.

##### File time record

Used when ctime/atime are needed or 1-second mtime precision is insufficient.

| Field                | Size            | Description |
|----------------------|-----------------|-------------|
| Size                 | vint            | |
| Type                 | vint            | 0x03 |
| Flags                | vint | 0x0001 — Unix time_t format if set, Windows FILETIME otherwise<br>0x0002 — Modification time present<br>0x0004 — Creation time present<br>0x0008 — Last access time present<br>0x0010 — Unix time format with nanosecond precision |
| mtime                | uint32 or uint64 | Present if 0x0002 set. Format depends on 0x0001. |
| ctime                | uint32 or uint64 | Present if 0x0004 set. Format depends on 0x0001. |
| atime                | uint32 or uint64 | Present if 0x0008 set. Format depends on 0x0001. |
| mtime nanoseconds    | uint32 | Present if 0x0001, 0x0002 and 0x0010 are all set. Added to mtime value. |
| ctime nanoseconds    | uint32 | Present if 0x0001, 0x0004 and 0x0010 are all set. |
| atime nanoseconds    | uint32 | Present if 0x0001, 0x0008 and 0x0010 are all set. |

##### File version record

Used in archives created with `-ver`.

| Field          | Size | Description |
|----------------|------|-------------|
| Size           | vint | |
| Type           | vint | 0x04 |
| Flags          | vint | No flags defined yet; set to 0. |
| Version number | vint | File version number. |

##### File system redirection record

| Field           | Size | Description |
|-----------------|------|-------------|
| Size            | vint | |
| Type            | vint | 0x05 |
| Redirection type| vint | 0x0001 — Unix symlink<br>0x0002 — Windows symlink<br>0x0003 — Windows junction<br>0x0004 — Hard link<br>0x0005 — File copy |
| Flags           | vint | 0x0001 — Link target is directory |
| Name length     | vint | Length of link target name |
| Name            | ? bytes | Link target name, UTF-8, no trailing zero |

(Note: original spec text lists Name as "vint"; it is a length-prefixed UTF-8
byte string.)

##### Unix owner record

| Field             | Size    | Description |
|-------------------|---------|-------------|
| Size              | vint    | |
| Type              | vint    | 0x06 |
| Flags             | vint    | 0x0001 — User name string present<br>0x0002 — Group name string present<br>0x0004 — Numeric user ID present<br>0x0008 — Numeric group ID present |
| User name length  | vint    | Present if 0x0001 flag set. |
| User name         | ? bytes | Owner user name, native encoding, not zero terminated. Present if 0x0001 set. |
| Group name length | vint    | Present if 0x0002 flag set. |
| Group name        | ? bytes | Owner group name, native encoding, not zero terminated. Present if 0x0002 set. |
| User ID           | vint    | Present if 0x0004 flag set. |
| Group ID          | vint    | Present if 0x0008 flag set. |

##### Service data record

| Field | Size    | Description |
|-------|---------|-------------|
| Size  | vint    | |
| Type  | vint    | 0x07 |
| Data  | ? bytes | Contents depend on service header type. |

### End of archive header

RAR does not read anything after this header, allowing third-party tools to
append extra information such as digital signatures.

| Field               | Size | Description |
|---------------------|------|-------------|
| Header CRC32        | uint32 | |
| Header size         | vint  | |
| Header type         | vint  | 5 |
| Header flags        | vint  | Flags common for all headers |
| End of archive flags| vint  | 0x0001 — Archive is a volume and it is not the last volume in the set |

## Service headers

Service headers are based on the file header structure and store supplementary
information.

### Archive comment header

Optional header storing the main archive comment. Contains `CMT` identifier in
the file name field. Placed before any file headers and after the main archive
header. Comment data is stored in UTF-8 immediately after the header. RAR does
not compress archive comments: packed and unpacked data sizes are equal and both
define the comment data size. Compression method is 0.

### Quick open header

Optional header storing the quick open record. Contains `QO` identifier in the
file name field. Placed after all file headers, but before the recovery record
and end of archive header. Locatable via the locator record in the main archive
header.

Quick open record data is stored immediately after the header. Not compressed:
packed and unpacked sizes equal the quick open data size. Compression method 0.

Quick open data is an array of data cache structures, each storing a portion of
archived data:

| Field          | Size    | Description |
|----------------|---------|-------------|
| Structure CRC32| uint32  | CRC32 of structure data starting from Structure size field. |
| Structure size | vint    | Size of structure data starting from Flags field. Max 3 bytes (2 MB) in current implementation. |
| Flags          | vint    | Currently 0. |
| Offset         | vint    | Distance from beginning of quick open header to beginning of archived data cached in this structure. Absolute positions referred to by the structure array are always growing. |
| Data size      | vint    | Size of archive data stored in this structure. |
| Data           | ? bytes | Archived data stored in this structure. |

Normally quick open data stores copies of file and service headers — either all
of them or a subset. If a required header is missing from quick open data or its
structure CRC32 is invalid, it is read from the original archive position.

Using quick open data is optional; you can skip it entirely and read only
standard archive headers. But use the same access pattern when listing names and
when extracting — otherwise one name could be displayed while another is
extracted when quick open data and real archive data intentionally differ
(security threat).
