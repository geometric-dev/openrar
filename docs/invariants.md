# DLL engine invariants (file-mode handles, v1.3.0+)

Central reference for the non-negotiable behavior contracts introduced by the
file-mode handle surface (`openrar_archive_open_file` and friends). The
integration spec (§6.11) is the host-facing description; this file is the
engineering-facing list every engine change must preserve. Each invariant
names the test that pins it in `tests/unit/file_handle_tests.cpp`.

## 1. Solid block invariant

- A RAR5 solid flag on a compressed entry means "decode continues from the
  previous compressed entry's LZ window, Huffman tables and repeat
  distances". Decoding a mid-chain entry with a fresh window produces **wrong
  data silently** — it must never be reachable as an extraction result.
- Therefore: before decoding entry K of a solid run, the reader guarantees
  chain position. Fast path: the immediate predecessor was the last entry
  fully decoded on this reader (`last_decoded_entry_index_ == K-1` and
  `solid_chain_ok_`). Rewind path: decode the run prefix [H(K), K-1] through
  a discard sink first (H(K) = nearest preceding entry without the solid
  flag). Stored/service entries inside the run are skipped — they do not
  touch the window.
- `last_decoded_entry_index_` advances **only** after a complete, successful
  decode. Any failure or cancel leaves it unchanged, so a retry re-runs
  catch-up cleanly; the handle stays usable after every abort or error.
- Out-of-order and repeated extraction are correct but pay
  O(solid-run-prefix) decode; hosts extract in ascending index order for
  speed. Suffix-only delete: mutation exports (Phase 2) must refuse deleting
  members of a solid run rather than orphan later entries.
- Pinned by: `solid_out_of_order_identity`, `solid_cancel_catchup_reusable`,
  `solid_repeat_extract`.

## 2. RAM ceiling invariant

Extraction/test memory is bounded by O(dictionary window + slice buffers),
strictly independent of uncompressed entry size:

- stored (plain or encrypted): ≤ 64 KiB I/O buffer + ≤ 256 KiB decrypt
  slice ⇒ ≤ ~1 MiB working-set increase;
- compressed (plain or encrypted): dictionary window (≤ 32 MiB RAR5 / ≤ 64
  MiB RAR7) + ≤ 256 KiB slice buffer;
- the only unbounded-by-entry-size surface left is the frozen buffer API and
  the capped `handle_extract` (OPENRAR_MAX_HEAP_EXTRACT_SIZE = 256 MiB).
- Chunked AES-256-CBC carries the CBC IV across slices (each slice's last 16
  ciphertext bytes become the next slice's IV — `Aes256::decrypt_cbc`
  contract) so no slice ever needs the whole cipher.
- Pinned by: `encrypted_stored_ram_ceiling` (peak working set) and
  structurally by every streaming sink in `archive_reader.cpp`.

## 3. Multi-volume lifetime invariant

- The primary volume (first of the set) is held open continuously by the
  handle; secondary volumes are opened on demand per access and closed
  immediately after their extent read completes. No descriptor accumulation,
  and extract-time missing-volume errors are testable on every platform.
- A middle volume passed to `open_file` is rewound to the derived first
  volume; a first volume that cannot be opened fails with
  `RAR_ERR_MISSING_VOLUME` ("cannot open first volume: <path>").
- A volume required by split_after / ENDARC NEXTVOL flags that cannot be
  opened fails the open (strict, DLL-only; the CLI's tolerant stop-at-gap
  scan behavior is unchanged). A volume missing at extract time fails that
  extraction with `RAR_ERR_MISSING_VOLUME` ("missing volume: <path>").
- Split entries are one logical entry: summed extents, whole-file CRC from
  the terminating block, single index in `handle_list`.
- Pinned by: `volume_stitch_roundtrip`, `volume_missing_at_open`,
  `volume_missing_at_extract`, `volume_middle_open`.

## 4. Callback & cancellation contract

- Progress: (done, total) = (uncompressed bytes produced, entry.size);
  cumulative, monotonic within one call; exactly one final (total, total) on
  success; none on abort or failure. Directory entries: exactly one (0, 0)
  on success. During solid catch-up progress stays at (0, entry.size).
  total = 0 is legal (empty and UNPUNKNOWN entries).
- Cancel is polled per output chunk (≤ window granularity compressed, ≤
  64 KiB stored, per catch-up flush chunk) and returns `RAR_ERR_ABORTED` with
  the destination untouched. Cancel observed only after the atomic rename
  has committed is ignored (the operation already succeeded).
- Callbacks run on the calling thread with no DLL-internal lock held
  (`HandleTable::pin` lifetime), and must not re-enter the same handle.
- Open-scan progress is byte-based across the volume set (done = bytes
  consumed, total = bytes of volumes opened so far — both monotonic).
- Pinned by: `progress_contract`, `cancel_leaves_no_temp`,
  `solid_cancel_catchup_reusable`, `directory_progress`.

## 5. Durability & temp-file lifecycle

- `handle_extract_to_path` writes `dest_path + ".openrar-tmp.<pid>.<seq>"`,
  opened CREATE_NEW (symlink/collision safe) with ≤ 10 seq retries, flushes
  (FlushFileBuffers / fsync) before close, then atomically renames over
  dest_path (MoveFileExW REPLACE_EXISTING / rename()).
- Abort, cancel, or any error closes and deletes the temp; dest_path is
  never left partial. Hosts may sweep orphaned `.openrar-tmp.*` files.
- Destination guard: dest_path resolving to the archive or any volume of its
  set fails up front with RAR_ERR_INVALID_ARG ("destination path cannot be
  the archive file").
- The frozen `openrar_archive_extract_file_to_path` keeps its direct-write
  behavior; only the handle surface owns durability.
- Pinned by: `atomic_durability_success`, `cancel_leaves_no_temp`,
  `destination_guard`.

## 6. Password & key hygiene

- Passwords enter via `open_file` only (no setter): header decryption needs
  them during the scan. `-hp` verification is the scan's PswCheck /
  header-CRC; `-p` verification is lazy, per entry, constant-time PswCheck
  before any decrypt (wrong password ⇒ `RAR_ERR_BAD_PASSWORD`, never silent
  garbage and never `CRC_MISMATCH`).
- Headers without a PswCheck record cannot distinguish wrong password from
  corruption — documented limitation, surfaced as decode/CRC failure.
- On close, password storage is best-effort zeroized before deallocation.
- No eager per-entry PBKDF2 at open (hostile `lg2_count` ≤ 24 would make
  open a DoS vector); PBKDF2 runs only when an encrypted entry is actually
  extracted or tested.
