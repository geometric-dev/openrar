# OpenRAR Test Fixtures & Golden Conformance

This directory houses deterministic RAR5 test fixtures, feature coverage archives, and locked golden outputs for verifying writer, mutator, and reader stability across OpenRAR releases.

---

## Directory Structure

```
tests/fixtures/
├── README.md                      ← This specification and convention guide
├── golden/
│   ├── writer/                    ← Bit-locked outputs produced by `openrar a`
│   │   ├── writer_stored=comp=m0.rar
│   │   ├── writer_stored=comp=m0.rar.sha256
│   │   ├── writer_compressed=comp=m3.rar
│   │   ├── writer_compressed=comp=m3.rar.sha256
│   │   ├── writer_solid=solid=1=comp=m3.rar
│   │   └── writer_solid=solid=1=comp=m3.rar.sha256
│   └── mutator/                   ← Bit-locked outputs produced by mutator actions
│       ├── mutator_add_file=action=add.rar
│       ├── mutator_add_file=action=add.rar.sha256
│       ├── mutator_delete_file=action=del.rar
│       ├── mutator_delete_file=action=del.rar.sha256
│       ├── mutator_lock=action=lock.rar
│       └── mutator_lock=action=lock.rar.sha256
└── rar5/                          ← Synthetic and oracle RAR5 feature coverage archives
    ├── hello5.rar
    ├── hello5.rar.sha256
    ├── hello5_p.rar
    ├── hello5_p.rar.sha256
    ├── hello5_hp.rar
    └── hello5_hp.rar.sha256
```

---

## Naming Convention

Fixture archives follow a structured `=key=value` syntax encoding key structural properties directly in the filename. This syntax is fully portable across Windows NTFS and POSIX filesystems (unlike colons `:` which are reserved NTFS stream separators):

`[prefix]_[descriptor][=key1=value1][=key2=value2]...rar`

### Standard Keys

| Key | Values | Meaning |
|---|---|---|
| `comp` | `m0`, `m1`, `m2`, `m3`, `m4`, `m5` | Compression method (`m0` = store, `m3` = normal, `m5` = best) |
| `solid` | `0`, `1` | Solid archive mode (shared LZ dictionary across entries) |
| `dict` | `128k`, `256k`, `1m`, `4m`, `16m`, `64m` | LZ window size |
| `enc` | `none`, `aes` | Payload encryption |
| `hp` | `0`, `1` | Header encryption (`-hp`) |
| `rec` | `none`, `3pct`, `5pct`, `10pct` | Reed-Solomon recovery record size |
| `vol` | `0`, `1` | Multi-volume split archive |
| `action` | `add`, `del`, `lock`, `freshen` | Mutator operation that transformed the baseline |

---

## Provenance Tracking & Determinism

All golden fixtures must be 100% reproducible. Non-deterministic attributes (such as variable local wall clock timestamps, varying filesystem creation times, or unseeded random payloads) are strictly forbidden:

1. **Fixed Timestamp**: Source files are normalized to `2026-01-01T00:00:00.000Z` before archive creation.
2. **Fixed Pseudorandom Seed**: Synthetic binary payloads are generated using fixed-seed PRNG (Seed `42`).
3. **No External Machine State**: Archives are generated without local username/machine path leaks (`-ep` exclude paths).
4. **Companion `.sha256`**: Every `.rar` file has an adjacent `.sha256` checksum file generated at creation time.

---

## Tooling

### 1. Generating Golden Fixtures
To regenerate the golden fixtures deterministically:
```powershell
powershell -ExecutionPolicy Bypass -File tools/generate_golden.ps1
```

### 2. Verifying Golden Fixtures
To verify SHA256 integrity and test extraction across all golden archives:
```powershell
powershell -ExecutionPolicy Bypass -File tools/check_golden.ps1
```

### 3. CI Gate Integration
`tools/check_golden.ps1` runs in automated CI to immediately detect any unintended bit-level writer or mutator divergence.
