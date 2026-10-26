# Phase 2 Master Refactoring Action Plan

Sources: `docs/review-phase2.1.md` … `docs/review-phase2.4.md`, cross-checked against the current working tree (HEAD `ab94d82` plus the uncommitted, mostly staged Phase 2.1 remediation across 18 `src/` files).

Status key: **OPEN** = I confirmed the defect is still in the code. **FIXED (wt)** = I confirmed the fix in the uncommitted working tree. **STAGED?** = the file has staged edits but I didn't check the fix line by line. Line numbers come from the review docs and may be a few lines off in the working tree.

Effort: **S** ≤ ½ day · **M** 1–2 days · **L** 3–5 days (each includes a regression test).

## 1. Master Priority Table

| ID | Priority | Phase | Location (File:Line) | Issue Summary | Estimated Effort |
|---|---|---|---|---|---|
| C-01 | CRITICAL | 2.4 | `src/recovery/recovery_writer.cpp:1853-1966` | **OPEN.** Multi-physical RR reassembly trusts `d0` (up to 65535) before the `d > 200` cap at ~2031. The unchecked `nr_logical * canonical_total` wraps 32-bit `size_t`, so the shard loop writes past the end of the heap buffer. On 64-bit the same input causes a multi-GB `bad_alloc` DoS. | S |
| C-02 | CRITICAL | 2.2 | `src/compress/decompressor50.cpp:976-1021, 1351-1379, 1130` | **OPEN.** The match distance is accumulated in `size_t`. On ILP32/WASM32, `<< d_bits` with `d_bits ≥ 32` is UB, and slots 64/65 wrap into `[1, win]`, which slips past the window check. `extra_dist` is accepted on 32-bit. | S |
| C-03 | CRITICAL | 2.2 | `src/compress/compressor50.cpp:1291, 2090-2097, 2264`; `parallel_compressor.cpp:299`; `archive_mutator.cpp:1347` | **OPEN.** The table size (430 vs 446) is picked from the *clamped* internal window, but the header records the *declared* window. Any file ≤ 4 GiB with an explicit dictionary > 4 GiB, and mixed parallel chunks, produce a stream no decoder can read. | M |
| C-04 | CRITICAL | 2.2 | `src/compress/compressor50.hpp:447-452`; `compressor50.cpp:685, 742, 1155-1156` | **OPEN.** `reconstruct_pos()` accepts `dist32 == 0` when a hash entry from exactly 2³² bytes earlier aliases, so the match finder matches a position against itself. Release builds rewrite distance 0 to 1, which emits a silently corrupt 4 KiB run. | S |
| C-05 | CRITICAL | 2.4 | `src/archive/foreign_zip.cpp:121, 151-183, 409, 430` | **OPEN.** The additive bounds checks `data_offset + packed_size` and `eocd64_off + 56` wrap. `parse_zip64_extra` merges the unp/pack presence flags, so a valid >4 GiB member gets its `lfh_offset` read as `packed_size`. Its return value is also ignored. | M |
| C-06 | CRITICAL | 2.4 | `src/archive/foreign_tar.cpp:222, 317, 439` | **OPEN.** `padded` is computed from the ustar size before a PAX `size` override, so the parser reads member payload as headers. The `data_offset + v > fsize` check can wrap. | S |
| C-07 | CRITICAL | 2.2 | `src/compress/arch/match_simd.hpp:40-56, 126-140, 214-257, 282-290, 359-374`; `compressor50.cpp:620-661` | **OPEN.** `ctz` runs on native-endian `memcpy` loads, so match lengths come out wrong on big-endian targets. `hash_range()` also extracts the wrong word on BE. | S |
| C-08 | CRITICAL | 2.2 | `src/archive/foreign_transcode.cpp:67`; `foreign_zip.cpp:287` | **OPEN.** Signed `int64` overflow: `seconds → nanoseconds` past 2262, and `int64(ft) - 116444736000000000` on an untrusted FILETIME. | S |
| C-09 | CRITICAL | 2.1 | `src/compress/parallel_compressor.cpp:21-24, 259-273` | **FIXED (wt).** `win_size == 0` seed-length underflow, which led to a wild `memcpy`. | — |
| C-10 | CRITICAL | 2.1 | `src/compress/compressor50.cpp:1767-1803` | **FIXED (wt).** The filter token length no longer disagrees with the transformed length: `load_data(required)` now runs first and fails closed. | — |
| C-11 | CRITICAL | 2.1 | `src/compress/decompressor50.cpp:475-536` | **FIXED (wt).** `last_flushed_` no longer overshoots on cross-block filters (now `+= len - prior`, with guards). | — |
| C-12 | CRITICAL | 2.1 | `src/compress/decompressor50.cpp:449-450, 900-902, 1618-1619` | **FIXED (wt).** `f_start` math is now 64-bit with a `SIZE_MAX` gate. `flush_plain_up_to` fails closed past the window, and `flush_cadence` replaces the hard-coded 64 KiB. | — |
| C-13 | CRITICAL | 2.1 | `src/api/abi_contract.hpp:74,105`; `src/io/win32_meta.cpp:223,330,385`; `src/io/file_stream.cpp:476,591`; `src/crypto/aes256.cpp:305,317` | **STAGED?** Strict-aliasing and alignment violations from `reinterpret_cast` type-punning. | S |
| C-14 | CRITICAL | 2.1 | `src/cli/main.cpp:1051-1112`; `src/dll/dll_api.cpp:1396-1421, 2109-2163, 2348-2485`; `src/wasm/archive_api.cpp:452-662` | **STAGED?** Data races: non-atomic `skipped_count`, and `StreamHandle`/`ArchiveHandle` entry points that have no `BusyGuard`. | M |
| H-01 | HIGH | 2.1 | `src/recovery/recovery_writer.cpp:1594-1706`; `src/io/extraction_journal.cpp:375-761` | **STAGED?** Raw `new FileStream`/`LockedFile` leak OS handles when `bad_alloc` is thrown. | S |
| H-02 | HIGH | 2.1 | `include/openrar/openrar.hpp:67-579` | **STAGED?** C-ABI buffers are held as bare pointers across operations that can throw, so they leak. | M |
| H-03 | HIGH | 2.1 | `src/compress/compressor50.cpp:531-618` | `start_file()` leaves `external_buf_` and `mem_src_ptr_` stale, so it can use a dangling pointer or load nothing. | S |
| H-04 | HIGH | 2.1 | `src/archive/archive_reader.cpp:3308-3328` | `extract_store_entry_impl()` seeks the shared `stream_`, which races with other readers. | S |
| H-05 | HIGH | 2.2 | `src/compress/decompressor50.cpp:25, 102-110` | `peek_bits64(58..64)` zero-pads even when unread input remains. | S |
| H-06 | HIGH | 2.2 | `src/compress/decompressor50.cpp:821-830, 1118, 1226` | A token that reads past `end_bit` sends the sequential and parallel decoders to different next-block boundaries. | S |
| H-07 | HIGH | 2.2 | `src/compress/decompressor50.cpp:240-264, 584-597` | `HuffmanDecoder::decode` and `slot_to_length` don't check that enough bits remain, so a truncated stream yields made-up symbols (up to 4097 bytes). | S |
| H-08 | HIGH | 2.3 | `src/compress/compressor50.cpp:~1797, 2146-2151`; `filters50.cpp:422-427, 774-779` | A 1 MiB `std::vector tmp` is allocated and zeroed on every Delta filter chunk. | S |
| H-09 | HIGH | 2.3 | `src/compress/filters50.cpp:432-498, 781-824` | Delta with `channels ≥ 2` gets no SIMD and makes N strided passes over 1 MiB, which thrashes the cache. | L |
| H-10 | HIGH | 2.3 | `src/compress/compressor50.hpp:47-70`; `compressor50.cpp:1403-1497` | `BitOutput::put_bits` calls `vector::resize` every 32 bits, which zero-fills and blocks register allocation. Capacity grows linearly. | M |
| H-11 | HIGH | 2.3 | `src/compress/compressor50.cpp:906-914, 996-1004` | `price_fresh` computes log₂ with serial shift loops, up to 5 times per match search. | S |
| H-12 | HIGH | 2.3 | `src/compress/decompressor50.hpp:196-212`; `.cpp:1085-1123` | `prescan_member` heap-allocates a `lengths` vector for every block. | S |
| H-13 | HIGH | 2.3 | `src/compress/inflate.cpp:404-502` | `window_copy` makes O(len/dist) tiny `memcpy` calls, writes every byte twice, and copies stored blocks one byte at a time. | M |
| H-14 | HIGH | 2.3 | `src/crypto/blake2sp.cpp:60-94, 163-190` | BLAKE2sp runs its 8 leaves as sequential scalar code. | L |
| H-15 | HIGH | 2.4 | `src/format/header_reader.cpp:600-634`; `archive_reader.cpp:1538-1550, 3071-3094` | A truncated `FHEXTRA_CRYPT` silently gets a zero salt/IV and turns off `psw_check`. `crypt_version` and `psw_check_csum` are never checked. | S |
| H-16 | HIGH | 2.4 | `src/recovery/recovery_writer.cpp:1548-1664` | `.rev` repair renames the user's volume to `.bad` before checking the rev set. Duplicate `rec_num`s pass the count. | M |
| H-17 | HIGH | 2.4 | `src/compress/stream_decoder.cpp:36-49, 100-110` | With `flush_cb_ == nullptr`, `output_` has no size cap, which opens a decompression-bomb path through WASM/DLL. | S |
| M-01 | MEDIUM | 2.1 | `src/compress/compressor50.cpp:411-454` | `begin_stream(continue_window)` compares the window before pow2 rounding, so a solid continuation rejects a valid window. | S |
| M-02 | MEDIUM | 2.1 | `src/compress/decompressor50.cpp:14-15` | `BitReader(nullptr, n>0)` does `nullptr + n`, which is UB. | S |
| M-03 | MEDIUM | 2.1 | `src/dll/dll_api.cpp:1308-1310`; `src/wasm/archive_api.cpp:283-285` | The path scan reads up to 4096 bytes when it only needs 2049. | S |
| M-04 | MEDIUM | 2.2 | `src/compress/decompressor50.cpp:933-955, 1568-1584` | Slot 257 before the first match is treated as a no-op instead of failing the stream. | S |
| M-05 | MEDIUM | 2.2 | `src/io/mapped_file.cpp:107-119, 201-213`; `src/io/buffer_stream.hpp:43-54` | Signed overflow in `seek()`. A negative `Begin` offset is clamped instead of rejected. | S |
| M-06 | MEDIUM | 2.2 | `src/compress/compressor50.cpp:1845-1858, 2004-2017` | The lazy match at `cur_+1` is clamped to `rem` instead of `rem-1` at filter boundaries, which costs a little ratio. | S |
| M-07 | MEDIUM | 2.3 | `src/compress/compressor50.cpp:421-423` | `match_tokens_` reserves 16384 entries against a flush threshold of 32768, so it reallocates mid-block. | S |
| M-08 | MEDIUM | 2.3 | `src/compress/stream_decoder.cpp:51-53, 100-114` | Input is always copied into `in_queue_`, and a `std::function` is built for every block. | M |
| M-09 | MEDIUM | 2.3 | `src/format/header_reader.cpp:100-179` | Each header read allocates 1 to 4 `std::vector`s for ≤16-byte scratch. | S |
| M-10 | MEDIUM | 2.3 | `src/recovery/recovery_writer.cpp:2401, 2468-2495` | Tier-2 candidate verification does one CRC update and one GF multiply per byte. | M |
| M-11 | MEDIUM | 2.4 | `src/format/header_reader.cpp:28-33, 273-279, 325-331, 565-571` | An oversized `extra_size` falls back silently instead of being rejected. | S |
| M-12 | MEDIUM | 2.4 | `src/archive/archive_reader.cpp:1186-1244` | `open()` accepts truncated extents, which is inconsistent with `BufferArchive::list()`. | M |
| M-13 | MEDIUM | 2.4 | `src/archive/foreign_tar.cpp:180-194, 294-345` | Garbage numeric fields become 0, and the PAX uid/gid parse has no overflow guard. | S |
| L-01 | LOW | 2.1 | `src/io/mapped_file.hpp:61-62` | **STAGED?** Raw pimpl pointer should be a `std::unique_ptr<Impl>`. | S |
| L-02 | LOW | 2.1 | `src/dll/dll_api.cpp:1188`; `src/wasm/wasm_api.cpp:220,313`; `archive_api.cpp:199` | **STAGED?** `malloc(1)` followed immediately by `free` on empty outputs. | S |
| L-03 | LOW | 2.2 | `src/crypto/sha256.cpp:40-42`; `blake2sp.cpp:9-11` | The rotate helpers hit UB if `n == 0`. | S |
| L-04 | LOW | 2.3 | `src/crypto/crc64.cpp:36-53` | CRC-64/XZ has no PCLMUL/PMULL folding path. | M |
| L-05 | LOW | 2.4 | `src/recovery/recovery_writer.cpp:392` | A stale `SHARD_MAGIC` comment. | S |

Suggested order: C-01 → C-02 → C-03 → C-04 → C-05/C-06 → C-08 → H-15/H-17/H-07 → commit the 2.1 working tree once C-13/C-14 are verified → remaining HIGH correctness → hot-path batch (section 3) → MEDIUM/LOW. Gate C-07 behind a big-endian CI job (qemu s390x) so it stays fixed.

## 2. Top 3 Critical Blockers

### Blocker 1: C-01, attacker-controlled heap overflow in recovery-record reassembly

**Why it's #1:** The input is a crafted archive. `repair` runs on exactly the files users already suspect are damaged, so a hostile file reaches this path directly. Inside the multi-physical block, `d0` is never bounded; the only cap (`d > 200`) comes about 180 lines later. With `d0 = 65535` and `scale = 512`, `scaled_header` is about 268 MiB per shard. On 32-bit/WASM, `nr_logical * canonical_total` wraps, `out_data` comes back small, and the loop that follows writes into it with `memcpy`/`write_le*` at `j * canonical_total`, which is a heap overflow. Even on 64-bit, `canonical_total` is written as a **`uint32`** shard field, so anything ≥ 4 GiB gets truncated in the rebuilt header. Separately, with `d0 = 65535` the row memcmp `(d0 + 1) * 8` reads 512 KiB out of each piece.

**Fix:** Apply the writer's geometry invariants *before* any derived arithmetic, keep the on-disk 32-bit field widths, and route the allocation through the existing checked helper.

```cpp
// recovery_writer.cpp: multi-physical reassembly, immediately after reading d0
core::uint32 d0 =
    core::read_le16(rr.raw_data.data() + std::get<0>(pieces[0]) + SHARD_D_FIELD);
// Same invariants compute_geometry() guarantees and the canonical path enforces
// later (d <= 200, nr <= 10*d). Enforce them before anything is derived from d0.
if (d0 == 0 || d0 > 200) return false;
if (nr_logical > 10u * d0) return false;

const core::uint32 unscaled_header = d0 * 8 + 0x48;           // <= 1672, cannot wrap
core::uint64 scale = (group0 + 0xFFFF) / 0x10000;
if (scale == 0) scale = 1;

// The canonical shard stores total/header as uint32 (SHARD_TOTAL_OFF /
// SHARD_HEADER_OFF), so both must fit in 32 bits or the rewrite is lossy.
if (scale > UINT32_MAX / unscaled_header) return false;
const core::uint64 scaled_header = static_cast<core::uint64>(unscaled_header) * scale;
if (group0 > UINT32_MAX - scaled_header) return false;
const core::uint64 canonical_total = scaled_header + group0;

// Checked nr * total, and it must fit size_t on this target (32-bit/WASM).
const auto out_bytes = calculate_parity_buffer_size(nr_logical, canonical_total);
if (!out_bytes || *out_bytes > static_cast<core::uint64>(SIZE_MAX)) return false;
std::vector<core::byte> out_data(static_cast<size_t>(*out_bytes), 0);
```

Also harden the per-piece loop: before `payload = total - unscaled_header`, reject `total < unscaled_header + SHARD_STATE_ARR + (d0 + 1) * 8`. That subtraction feeds `chunkpos + payload`, which can wrap. I didn't trace where `total` is first validated during piece discovery, so treat this as defence in depth. Regression test: add a crafted multi-piece record with `d0 = 65535` to `attack_corpus_tests` and assert `repair()` returns `false` without allocating.

### Blocker 2: C-02, shift-count UB and distance wrap in the RAR5/7 decoder on 32-bit/WASM

**Why it's #2:** It's on the main decode path for every untrusted archive on a shipping target (WASM32). `distance` is a `size_t`. With `extra_dist`, `dist_slot` goes up to 79, so `d_bits` reaches 38. `static_cast<size_t>(…) << d_bits` with `d_bits ≥ 32` is UB under `[expr.shift]`, and UBSan on a 32-bit build will flag it. Even without `extra_dist`, slots 64/65 (`d_bits = 31`), plus `static_cast<size_t>(extra) << 4` with 27 to 34-bit extras, wrap mod 2³². A huge distance can then land back in `[1, win_size_]`, pass the window check at line 1021, and copy stale window bytes into the output. It also poisons `old_dist_`, because the distance is stored before validation.

**Fix:** Do the arithmetic in `uint64`, validate *before* storing or using it, then narrow. Put it in one helper so `decompress_internal()` and `decode_span()` can't drift apart:

```cpp
// decompressor50.cpp: shared by decompress_internal() and decode_span()
// Returns 0 on malformed input (0 is never a valid RAR5 distance).
static core::uint64 decode_distance(BitReader& reader, HuffmanDecoder& ldd,
                                    core::uint32 dist_slot) {
    if (dist_slot < 4) return 1u + dist_slot;
    const core::uint32 d_bits = dist_slot / 2 - 1;              // 1..38
    core::uint64 distance = 1u + (static_cast<core::uint64>(2u | (dist_slot & 1u)) << d_bits);
    if (d_bits >= 4) {
        if (d_bits > 4) {
            const core::uint32 n = d_bits - 4;
            if (reader.bits_remaining() < n) return 0;
            const core::uint64 extra = (n > 32) ? reader.get_bits64(n) : reader.get_bits(n);
            distance += extra << 4;                              // 64-bit, no truncation
        }
        if (reader.bits_remaining() < 1) return 0;
        distance += ldd.decode(reader);
    } else {
        if (reader.bits_remaining() < d_bits) return 0;
        distance += reader.get_bits(d_bits);
    }
    return distance;
}

// call site
const core::uint32 max_slot = use_extra_dist_ ? 80u : 64u;
if (dist_slot >= max_slot) return false;            // replaces the assert (Release-safe)
const core::uint64 d64 = decode_distance(reader, ldd_decoder_, dist_slot);
if (d64 == 0 || d64 > static_cast<core::uint64>(engine_.win_size_)) return false;
const size_t distance = static_cast<size_t>(d64);   // safe: <= win_size_ <= SIZE_MAX
// ...length bonus, then old_dist_ rotation, using the validated value only
```

In `initialize_from_description()` and the table reader, refuse windows a 32-bit target can't address:

```cpp
if constexpr (sizeof(size_t) < 8) {
    if (desc.extra_dist) return false;   // >4 GiB dictionary is unaddressable here
}
```

Regression test: a hand-built block with `dist_slot = 64` and `win_size = 1 MiB` must fail on the WASM/x86 build. Also run the existing fuzz corpus under `-m32 -fsanitize=undefined`.

### Blocker 3: C-03, encoder emits 430-symbol tables under a header that declares a 446-symbol window

**Why it's #3:** It's deterministic, needs no hostile input, and the archive created is unreadable. That's the worst case for an archiver, because users find out at restore time. `make_tables()` picks the table from `win_size_`, the **internal** window (`compressor50.cpp:1291`). `compress_buffer()` shrinks that to `pow2(src_size)` (2090-2097). The parallel workers shrink it to `seed + len` (`compressor50.cpp:2264`, `parallel_compressor.cpp:299`). Meanwhile `archive_mutator.cpp:1347` deliberately records the **declared** dictionary in the file header, and the decoder sizes its tables from that. So any file ≤ 4 GiB compressed with `-md` > 4 GiB gets 430-entry tables that the decoder reads as 446. The parallel path can even mix both kinds inside one member.

**Fix:** Separate "LZ allocation window" from "bitstream format window". The format window has to follow what the header declares.

```cpp
// compressor50.hpp
public:
    // Window the *file header* will declare. Drives bitstream format decisions
    // (table size / DCX) independently of the possibly-clamped LZ allocation.
    void set_declared_window(core::uint64 declared) { declared_win_ = declared; }
private:
    core::uint64 declared_win_{0};   // 0 => same as win_size_

// begin_archive(): reset so a reused packer can't inherit a stale value
declared_win_ = 0;

// make_tables()
const core::uint64 fmt_win = declared_win_ != 0 ? declared_win_ : win_size_;
cur_table_size_ = (fmt_win > (4ULL << 30)) ? TABLE_SIZEX : TABLE_SIZE;
```

```cpp
// compress_buffer(): after begin_archive(..., comp_win_size)
packer.set_declared_window(win_size);

// compress_buffer_parallel() and ParallelBlockPipeline worker:
packer->begin_archive(nullptr, method, worker_win);
packer->set_declared_window(win_size);   // cfg_.win_size in the pipeline
```

The encoder never emits a distance > `worker_win`, so the extra 16 distance slots just get zero-length codes. That's valid. Regression test (64-bit only, since `normalize_window_size` caps 32-bit at 1 GiB): compress 2 MiB with `win_size = 8 GiB` through `compress_buffer` and through `compress_buffer_parallel(threads = 4)`, then round-trip through `Decompressor50` and an external `unrar`.

## 3. Hot-Path Optimization List

These are in rough order of throughput gain per unit of effort. All are behaviour-preserving, so the output bytes stay the same and the golden fixtures should keep passing unchanged.

- **H-10, `BitOutput` raw cursor:** reserve the block worst case once (`tokens * 10 + 1024`), write 64-bit words through a `core::byte*` cursor, and commit `size()` once in `finish_block_data()`. Grow capacity geometrically. This removes a resize and zero-fill per 32 bits on every token.
- **H-11, branchless log₂ in `price_fresh`:** replace both `while` loops with `std::bit_width(v | 1) - 1` (C++20) or `63 - __builtin_clzll(v | 1)` / `_BitScanReverse64`. That's up to 5 calls per match search on the innermost encoder loop.
- **H-08, persistent filter scratch:** add a `filter_scratch_` member to `Compressor50`, sized once to `filter_max_chunk()`, and hoist `tmp` in `compress_buffer()`. This removes a 1 MiB allocate-zero-free per Delta chunk.
- **H-09, Delta multi-channel SIMD:** dedicated `channels == 2/4` kernels (SSSE3 `pshufb` / NEON `vld2q`/`vld4q` de-interleave plus an in-register prefix sum). For other channel counts, a 16 KiB L1-tiled single pass replaces the N full-buffer strided passes.
- **M-07, `match_tokens_` reserve 32768:** matches the flush threshold and removes a mid-block realloc and 256 KiB copy. It's a one-line change.
- **H-12, `PrescanDescription::lengths` → `std::array<core::byte, 446>`:** also pre-reserve `blocks` and `descriptions` from `src_size / 32 KiB`. This removes the per-block heap allocation on the parallel decode prescan.
- **H-13, Inflate:** flush straight from `window_` at the 32 KiB wrap (dropping `out_buf_` halves write bandwidth), expand overlapping matches with dist < 16 by pattern doubling, and bulk-copy stored blocks from `in_buf_`.
- **M-08, `StreamDecoder` zero-copy:** decode complete blocks directly from the caller's buffer when the queue is empty, queue only the tail, and hoist the `OutputCallback` out of the loop.
- **M-09, header scratch on the stack:** use `core::byte[16]` / `core::byte[10]` instead of 1 to 4 `std::vector`s per header. This is measurable on archives with many small entries.
- **H-14, BLAKE2sp 8-way AVX2 (4-way SSE2/NEON):** one `__m256i` per state word across the 8 leaves, compressing straight from the input when ≥ 512 B are available. This makes hashing far cheaper whenever BLAKE2 checksums are enabled.
- **M-10 / L-04, recovery CRC:** feed the untouched prefix and suffix to CRC-64 in bulk, transform only the syndrome slice in a stack chunk, and later add a PCLMUL/PMULL CRC-64 fold. This affects repair and RR-generation speed, not the compression core.

## 4. Remediation Status (2026-10-21)

Verified with a Release (MSVC x64, AVX2) build: 52/52 ctest suites pass, including the `fuzz_*` standalone sweeps (`-DOPENRAR_FUZZ=ON`). The clang/libFuzzer/ASan/UBSan lane runs only in the nightly `fuzz.yml` job. It could not run locally because WSL failed to start.

| Status | IDs |
|---|---|
| Fixed this pass | C-01, C-02, C-03, C-04, C-05, C-06, C-07, C-08, H-05, H-06, H-07, H-08, H-09, H-10, H-11, H-12, H-13, H-14, H-15, H-16, H-17, M-05, M-06, M-07, M-08, M-09, M-10, M-11, M-12, M-13 (PAX ids), L-03, L-05 |
| Already fixed in the working tree (verified) | C-09..C-14, H-01..H-04, M-01..M-03, L-01, L-02 |
| Kept by design | M-04: slot 257 before the first match stays a no-op, matching unrar's Unpack5; the misleading encoder comment is corrected. M-13: garbage ustar mode/uid/gid/mtime still coerce to 0, which `foreign_tar.cpp` documents as cosmetic once the checksum passes. |
| Deferred | L-04 (CRC-64 PCLMUL fold). Optional, and it needs derived folding constants plus a dedicated KAT. |

New finding from `tests/fuzz/fuzz_target.cpp` (fixed): at the 64 KiB window floor the plain-byte flush cadence (`min(64 KiB, win)`) equalled the window. A long match crossing the cadence point left more than `win_size_` bytes unflushed, so `flush_plain_up_to()` rejected valid streams. The cadence is now `min(64 KiB, win / 2)` (`ApplyEngine::flush_cadence()`), and `test_small_window_long_match_roundtrip` covers it.

Verification limits:
- **Untestable here:** C-02 (32-bit only), C-07 (big-endian only) and the NEON Delta kernels (no ARM host). These rely on code review plus the new differential tests on other hosts.
- **C-03:** checked by inspection only. In-process decode refuses dictionaries over 1 GiB, so an external `unrar` run is the real check.
- **No dedicated regression test:** C-01, H-15 at the reader layer, and H-16. They are covered by the existing recovery/archive suites.

## 5. Large dictionaries: C-03 follow-up (2026-10-21)

The table flavor is now picked from the right field. The 446-entry RAR7 tables are selected by the header's algorithm field: `unp_ver == 1` without `FCI_RAR5_COMPAT`, which is what unrar does (`archive_reader.cpp`, `decompressor50.cpp`). The window size no longer decides it. Our headers always set the compat flag, so RAR read any >4 GiB-window archive we wrote as 430-table. The original C-03 fix made this worse: it emitted 446 tables, and RAR reported checksum errors. The encoder now always emits 430 tables and caps match distance at 4 GiB, and the decoder takes the flavor from `FileBlock::extra_dist` (per member, so solid chains can switch). Archives an older openrar wrote with a >4 GiB window over more than 4 GiB of input carry 446-entry tables behind a RAR5-compat header. RAR always rejected those, and this decoder now rejects them too.

The decoder window is now allocated lazily. `WindowBuffer` reserves address space for the declared size and commits pages as output arrives (VirtualAlloc/mmap on 64-bit; a growable vector below 64 MiB and on 32-bit/WASM). Members no longer zero the whole window at start. A 64 GiB-declared archive tests with a 9 MiB peak. This also fixes a hostile-header memory DoS, where a few hundred bytes could make the decoder commit 64 GiB.

Encoder memory is bounded too. The streaming encoder paths and the CLI's solid session sized match-finder state from the declared dictionary, so `-md8g` on a 24 MiB file failed with out of memory. They now use `lz_window_for_input`, while the header still records the declared size.

Test-runner fix: 4 suites passed a hard-coded case count to the runner that was lower than their table size, so newly added cases were silently skipped in ctest. Every suite now passes `sizeof(k_cases)/sizeof(k_cases[0])`. The previously skipped cases include the BLAKE2sp bulk differential, the ZIP64 and TAR PAX regressions, and the header flavor test, and all of them pass.

| Interop evidence | Result |
|---|---|
| `tools/interop_gate.py` full run (27 stages) | pass |
| Track 19: 64m/1g/3g/4g/5g/8g/64g × {small, parallel, E8, Delta, solid}, openrar → UnRAR/RAR/openrar and RAR → openrar | 35 cases pass |
| Track 19 coverage gap | RAR/UnRAR ran out of memory at 64g (every case) and at 8g on some cases on this 16 GB host. Those cases are openrar-verified only; the gate prints them. |
| Track 20 (opt-in `OPENRAR_GATE_HEAVY=1`): RAR 7.20 `v70 -md=4480m` member over 4.5 GiB of input | decoded byte-identically |
| Track 20 encode leg (`OPENRAR_GATE_HEAVY_ENCODE=1`) | not run. The encoder needs about 48 GiB for -md6g over 4.5 GiB. |

### Pre-tag additions (v1.45.0)

- C-03 follow-up: `FCI_RAR5_COMPAT` is now written whenever `unp_ver==1` without `extra_dist`, not only for windows >= 128 KiB (sub-128 KiB explicit dictionaries produced a 430-table encoder against a 446-table decoder). Tests: `test_subminimum_dictionary_member_decodes`, extended `test_extra_dist_flavor_from_comp_info`.
- Regression tests: C-01 `test_hostile_reassembly_geometry_refused`; C-04 `test_find_match_rejects_zero_distance_self_candidate`; C-08 `test_zip_ntfs_filetime_hostile_values`.
- C-08 is reachable on x64 (not 32-bit only). Writing its test exposed a second bug: the ZIP NTFS extra read tag/size from the reserved word, so NTFS timestamps were never honored. Fixed.
- Known gaps (no 32-bit or big-endian CI leg): C-02, C-07, NEON kernels. Unrun: clang libFuzzer/ASan/UBSan lane (nightly only).
