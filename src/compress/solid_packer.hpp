#ifndef OPENRAR_COMPRESS_SOLID_PACKER_HPP
#define OPENRAR_COMPRESS_SOLID_PACKER_HPP

#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "stream_encoder.hpp"

#include <vector>

namespace openrar::compress {

// Encoder-side solid-window session (v1.26 plan §1): packs a sequence of
// files so each file's RAR5 stream may reference the previous files' bytes
// inside the shared dictionary window — the write-side mirror of the
// decoder's solid carry (Decompressor50::decompress_internal solid=true,
// which OpenRAR's reader has always honored). Until now the write side
// compressed every file from an empty window, so solid archives were
// header-solid but never window-solid.
//
// Chain contract (must match compress_plan / the writer's re-plan):
//  - chain_member=false  → fresh window (chain head: first data entry of a
//    run, or the entry after a FILECOPY redir gap);
//  - chain_member=true   → carry from the previously PACKED file.
// Entries that end up STORED must never join the session — their bytes never
// enter the decoder's window — so the caller keeps the packed stream even
// when it is marginally larger than the raw input (block-codec overhead for
// incompressible data is a few per-mille). Empty files are transparent to
// the chain and skip the session entirely.
//
// Strictly sequential by contract: one caller, files in archive order. The
// CLI's solid batches already force threads = 1; the caller owns
// synchronization. Per-file raw-data CRC is the caller's job (it may also
// encrypt after pack_end; the window always holds plaintext).
class SolidPacker {
public:
    SolidPacker(int method, size_t win_size) : enc_(method, win_size) {}

    // True once at least one file has been packed (a carried window exists).
    bool has_state() const { return has_state_; }

    // Route packed bytes to a sink (streaming/spool path). Without a sink,
    // packed bytes accumulate and come back through pack_end's `out`.
    void set_sink(int (*cb)(void*, const core::byte*, size_t), void* user) {
        enc_.set_flush(cb, user);
    }

    // One entry = pack_begin(chain_member) → pack_feed* → pack_end.
    // pack_begin returns false when the session cannot honor the requested
    // carry (e.g. geometry mismatch); pack_feed/pack_end return false on
    // encode failure. After a successful pack_end the window carries this
    // file's bytes for the next chain member; `out` (when non-null) receives
    // the packed stream if no sink is set.
    bool pack_begin(bool chain_member);
    bool pack_feed(const core::byte* data, size_t n);
    bool pack_end(std::vector<core::byte>* out = nullptr);

private:
    StreamEncoder enc_;
    bool has_state_{false};
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_SOLID_PACKER_HPP
