> **STATUS (2026-09, API v2).** Path A is shipped for the **encoder**:
> `Compressor50::begin_stream/feed/finish_stream` + incremental
> `StreamEncoder`, byte-identity gated by `tests/unit/stream_encoder_tests.cpp`,
> exposed as `openrar_stream_*` wasm exports and `OpenRAR.compressStream()`.
> The **decoder** still needs the mirror-image resumable pump before
> `decompressStream` can ship — do NOT patch decoder state from outside;
> add an internal `process_available(final)`-style step like the encoder did.

# Streaming Considerations — openrar-wasm & large payloads

**Status:** Investigation notes from a 2026-09 spike that was **deferred**. The
non-streaming API in `src/wasm/wasm_api.cpp` + `wasm/js/openrar.js` is the
current contract. This doc captures everything learned so the next attempt
starts from a solid base instead of re-deriving it.

---

## 1. Problem statement

The block-codec WASM module exposes `compress(data)` / `decompress(data)` that
materialise the full input as a `Uint8Array`, hand it to the WASM heap, run
the compressor, and copy the output back to JS. For an `N`-byte input the
peak resident memory is approximately:

| Stage | Bytes held | Notes |
|---|---:|---|
| JS `Uint8Array` source | `N` | Browser/Node heap |
| WASM heap input copy (embind path) | `N` | The `std::vector<byte>` inside `wasm_api.cpp` |
| Compressor scratchpad | ~7 MiB | Bounded by 2 MiB dictionary + 4 MiB read-ahead + 1 MiB read chunk; *not* a function of `N` |
| Compressed output | ≤ `N + 5` bytes/block | Worst case (incompressible input) |
| WASM heap → JS return copy | ≤ `N` | `HEAPU8.slice(ptr, ptr+len)` |
| **Peak resident** | **~3N + 7 MiB** | 1 GiB input ⇒ ~3 GiB |

The compressor itself is *already* streaming internally — its scratchpad
buffer is 7 MiB regardless of input size. The whole `N` cost is the
**buffer-copy choreography** between the JS host and the WASM heap, not the
compressor.

## 2. Goals (when we revisit)

| Goal | Target |
|---|---:|
| 1 GiB input peak resident | ≤ 1.2 GiB (one input buffer + ~7 MiB scratchpad + small output chunks) |
| Cross-chunk LZ matches | Preserved (no compression-ratio penalty vs. one-shot) |
| Compression ratio | Byte-identical to one-shot `compress_buffer` |
| API surface | New `feed()` / `finish()` on a stateful encoder; matches JS-side chunked reads |
| Test coverage | Native `stream_encoder_tests` runs on every CI matrix without emsdk |

## 3. Why the first attempt crashed

### What was tried (reverted in early prototype)

Added `Compressor50::feed_chunk(buffer, n)` that:
1. Re-points `mem_src_ptr_ = buffer` and `mem_src_pos_ = src_size_` (cumulative
   bytes already drained).
2. Bumps `src_size_ += n` and `mem_src_size_ = src_size_`.
3. Caller invokes `compress()`, which drains via the existing `while (cur_ <
   src_size_)` loop and `load_data()`'s `mem_src_ptr_ + mem_src_pos_` reads.

Added `Compressor50::finish_block()` that emits a final `last_block=true` block
when `last_block_emitted_` is false (set inside `write_block()`).

Added `StreamEncoder` wrapper that keeps a `std::vector<byte> input_`
cumulative buffer, calls `feed_chunk(input_.data(), input_.size())`, then
`compress()`.

### Crash signature

Iterations 1 and 2 worked byte-perfectly. Iteration 3 (256 KiB input fed in
4 × 64 KiB chunks) crashed with `-1073741819` (access violation) inside
`compress()`. Diagnostic prints showed `input_` did **not** reallocate during
`compress()`, so the stale-pointer hypothesis was ruled out.

### Root cause analysis

The packer internals assume `mem_src_ptr_` points at the **start of the full
input** so that `load_data()` can compute absolute offsets:

```cpp
size_t avail = mem_src_size_ - mem_src_pos_;          // bytes remaining
std::memcpy(&buf_[src_loaded_ - pos_base_],
            mem_src_ptr_ + mem_src_pos_,                // <-- assumes start of input
            read_size);
mem_src_pos_ += read_size;
```

This works in `compress_buffer()` because `mem_src_ptr_` is set once at the
start of the full buffer and `mem_src_pos_` walks it forward. It **also**
appears to work for a cumulative input where `mem_src_ptr_ = input_.data()`
and `mem_src_pos_ = src_size_ - n` (so reads at `input_.data()[mem_src_pos_]
... input_.data()[src_size_]`) — and indeed iterations 1 and 2 worked.

The crash on iteration 3 is most likely a **stale `src_loaded_`** state.
After iter 2, `src_loaded_ = 131072`. On iter 3, `feed_chunk` bumps `src_size_`
and `mem_src_size_` but leaves `src_loaded_` untouched. `load_data()` then
reads `min(to_read, until - src_loaded_) = min(0x80000, 196608 - 131072) =
65536`. So far so good. The read index is `src_loaded_ - pos_base_` in the
packer's own `buf_`, not in the input buffer — that part is independent of
the cumulative-buffer question.

The remaining candidate is the hash table: `head_[h] = pos & 0xffffffff`,
`prev_[pos & win_mask] = head_[h]`. These are keyed on absolute stream
position, which is consistent across iterations, so they should be OK.

**Most plausible explanation:** some interaction with `consume_source` or
`unhashed_pos_` when the `last_block_emitted_` flag interacts with the
boundary detection logic. The packer's `last_block_emitted_` was added in the
same patch; reverting it changes behavior. **Without a debugger trace** the
exact line is uncertain, but the patch is small enough to bisect by
commenting.

### Lesson: incremental state ownership is hard

The existing `Compressor50` was not designed for state-resumption across
multiple `compress()` calls. Patching fields from the outside (`mem_src_ptr_`,
`mem_src_pos_`, `src_size_`, `src_loaded_`) creates invariants that are hard
to maintain. Two cleaner paths exist:

#### Path A: internal `feed(src, n)` method

Add a `Compressor50::feed(const byte* src, size_t n)` that:

- Treats `src` as the **only source** for this call (no cumulative buffer in
  the packer; the JS side maintains it).
- Sets `mem_src_ptr_ = src`, `mem_src_size_ = n`, `mem_src_pos_ = 0`
  temporarily for this call's `load_data()` reads.
- Reads `n` bytes into the packer's own `buf_` scratchpad, then runs the
  match-finding loop on those bytes alone.
- After `feed()` returns, the packer's window/hash state is updated but
  `mem_src_ptr_/size_/pos_` are restored to the previous values.

This keeps the existing `compress()` loop untouched. The packer still needs
an internal "absolute stream position" counter (currently `cur_`), but that
already exists and is only invalidated when `begin_archive` is called.

#### Path B: incremental `process_window()`

Refactor `compress()`'s inner body into `process_window(until)` that runs
the loop until `cur_ == until`, with no eof handling. Then:

```cpp
for (chunk : chunks) {
    append_to_internal_buffer(chunk);
    process_window(chunk_end);  // matches + emit blocks
}
final_block();                 // close last block
```

Path A is preferred — fewer invasive changes, no internal buffer ownership
questions.

## 4. JS-side streaming (cheap, available now)

For most use cases the *JS* side can stream without C++ changes:

```js
// For a fetch Response with a known body length
const rar = new OpenRAR();
const reader = response.body.getReader();
const chunks = [];
while (true) {
  const { done, value } = await reader.read();
  if (done) break;
  chunks.push(value);
}
const blob = new Blob(chunks);
const compressed = await rar.compress(await blob.arrayBuffer(), 3);
```

Downsides:

- Each `chunk` is copied into JS `Uint8Array` (browser overhead).
- `arrayBuffer()` materialises the whole stream in JS before calling WASM.
- No cross-chunk LZ matches — if a pattern is split across two `value`s it
  won't be deduplicated.

For a 1 GiB file this is still ~1 GiB peak (the `Blob` then the
`arrayBuffer()` copy). The win over `compress(buffer)` is zero. To do
better, we need **Path A**.

## 5. Memory profile target (post-Path-A)

For a 1 GiB input fed in 1 MiB chunks:

| Stage | Bytes held |
|---|---:|
| Caller's input buffer (e.g. `Response.body`) | streaming — bounded by chunk size in the runtime |
| JS `Uint8Array` per chunk (browser fetch) | 1 MiB |
| WASM heap chunk copy (`feed_chunk`) | 1 MiB (per call; freed by next call) |
| Compressor scratchpad | ~7 MiB (constant) |
| Emitted blocks (in `out` vector) | ≤ N bytes (still need a final assembly) |
| Output buffer | 1 GiB worst case (we can flush periodically instead) |

To eliminate the **output** buffer too we'd need a `flush_cb` that returns
each block to the JS host immediately. That requires `BitOutput` to support
a callback variant instead of the `std::vector<byte>&` sink — also a small
change.

## 6. Open questions for next attempt

1. **Should `feed()` accept `std::span<const byte>` or raw pointer+length?**
   C++17 has `std::span`; using it documents lifetime. The current
   `Compressor50` API uses raw `const byte*` + `size_t`; staying consistent
   avoids the include cost.
2. **Where does the output buffer live?** Currently `mem_out_` is set per
   `compress()` call. For streaming we want either (a) one `mem_out_` per
   `feed()` call so the JS layer can flush between chunks, or (b) a
   `flush_cb` callback.
3. **Dictionary reset between independent files** vs. **persistent window**
   — `compress_buffer()` does the former via `begin_archive()`. Streaming
   across multiple logical files in the same archive (the WASM
   `Archive.toBytes()` use case) needs persistent window + per-file reset.
4. **How to test cross-process?** The 1 GiB peak memory is only measurable
   by actually running a 1 GiB compress in a memory-limited runner. The
   `tests/unit/wasm_api_tests.cpp` C ABI tests round-trip small payloads;
   a 100 MiB "large but tractable" benchmark is the missing test.
5. **`ALLOW_MEMORY_GROWTH=1` interaction.** Emscripten's `ALLOW_MEMORY_GROWTH=1`
   lets the heap grow to ~4 GiB on wasm32. For 1 GiB inputs + 1 GiB outputs
   we'd need ~3 GiB heap, which is within range but worth pinning.

## 7. Recommended approach for next attempt

1. **Start with Path A**: add `Compressor50::feed(const byte* src, size_t n)` as
   a thin wrapper that owns the per-call state. Don't touch private fields
   from outside.
2. **Add `StreamEncoder` class** (~80 LOC) that owns a `Compressor50` plus
   an `std::vector<byte>` cumulative input buffer. The wrapper's `feed()`
   calls `packer.feed(input_.data() + prev_size_, n)`, then triggers drain.
3. **Add `flush_cb` path**: optional `std::function<void(const byte*, size_t)>`
   constructor argument to `StreamEncoder`. When set, blocks are forwarded
   immediately instead of accumulating.
4. **Native tests first**: `tests/unit/stream_encoder_tests.cpp` should run
   on every CI matrix (no emsdk required). Cross-chunk LZ ratio is the
   key invariant to assert.
5. **WASM binding second**: only after native tests pass and the C++ is
   stable. Add `openrar_stream_create()` / `openrar_stream_feed()` /
   `openrar_stream_finish()` exports.
6. **JS driver last**: a `Stream` class that uses `fetch().body.getReader()`
   to feed the WASM encoder chunk by chunk.

Estimated complexity for steps 1–4: **~300 LOC + 100 LOC tests**. Steps 5–6
add ~250 more. Total ~650 LOC across C++/JS — substantial but bounded.

## 8. References

- `src/compress/compressor50.cpp` — packer internals, `load_data()`,
  `consume_source()`, `write_block()`.
- `src/compress/decompressor50.cpp` — already handles multi-block input
  via `for(;;) read_block_header()` loop, so the *decoder* needs no changes
  for streaming support.
- `src/wasm/wasm_api.cpp` — current single-buffer surface.
- `wasm/js/openrar.js` — JS wrapper; `OpenRAR.compress()` is the call site
  that would gain a streaming sibling.
