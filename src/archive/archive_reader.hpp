#ifndef OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
#define OPENRAR_ARCHIVE_ARCHIVE_READER_HPP

#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "../format/headers.hpp"
#include "archive_entry.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace openrar::compress {
class Decompressor50;
}

namespace openrar::crypto {
struct Rar5Keys; // POD key bundle (pbkdf2.hpp); streaming methods take it by pointer
}

namespace openrar::archive {

// ── Hooks for the streaming file-handle APIs (v1.3.0) ────────────────────────
// Same convention as buffer_archive's core-facing callbacks: progress is
// (done, total) cumulative and monotonic; cancel is polled per output chunk
// (dictionary-window granularity compressed, <= 64 KiB stored), per solid
// catch-up flush and per header block during open, and a non-zero return
// aborts. Declared locally because the reader is always built while the
// buffer layer is optional (OPENRAR_INMEM_ARCHIVE).
struct ReaderHooks {
    void (*progress)(uint64_t done, uint64_t total, void* user) = nullptr;
    void* progress_user = nullptr;
    int (*cancel)(void* user) = nullptr;
    void* cancel_user = nullptr;

    void emit(uint64_t done, uint64_t total) const {
        if (progress) progress(done, total, progress_user);
    }
    bool cancelled() const { return cancel != nullptr && cancel(cancel_user) != 0; }
};

class ArchiveReader {
public:
    ArchiveReader();
    ~ArchiveReader();

    bool open(const std::filesystem::path& arc_path, const std::string& password = "");
    void close();

    void set_password(const std::string& password) { password_ = password; }
    const std::string& password() const { return password_; }
    bool has_bad_password() const { return bad_password_; }

    bool is_open() const;
    bool is_locked() const;
    bool is_volume() const;
    bool is_solid() const;
    bool is_header_encrypted() const { return header_encrypted_; }
    const format::CryptBlock& header_crypt() const { return header_crypt_; }
    // Set as soon as a HEAD_CRYPT block is parsed (even when key init later
    // fails) so open_ex can distinguish "encrypted, needs password" from a
    // generic parse failure.
    bool saw_crypt_header() const { return saw_crypt_header_; }
    bool crypt_version_unsupported() const { return crypt_unsupported_; }
    core::uint64 sfx_offset() const { return sfx_offset_; }
    // Path this reader was opened from (the first volume for multi-volume
    // sets) — collision-check input for the DLL mutation surface.
    const std::filesystem::path& path() const { return path_; }

    // For testing: get the window size of the active solid chain (returns 0 if none)
    size_t test_get_solid_window_size() const;

    const format::MainBlock& main_block() const { return main_block_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }

    // Test CRC32 of stored uncompressed or compressed payload
    bool test_entry(const ArchiveEntry& entry);

    // Extract entry (stored or compressed) directly to disk
    bool extract_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                       const std::string& password = "");

    // Extract stored (method 0) entry directly to disk
    bool extract_store_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                             const std::string& password = "");

    io::FileStream& stream() { return stream_; }
    bool read_packed_data(const ArchiveEntry& entry, std::vector<core::byte>& out) const;

    // ── Streaming file-handle APIs (v1.3.0; the DLL surface) ─────────────────
    // As open(), plus: a middle-volume path (.partNN.rar, NN > 1) is rewound
    // to the derived first volume; a volume required by split_after /
    // ENDARC NEXTVOL flags that cannot be opened fails the open when
    // `strict_volumes` is set (the CLI's tolerant stop-at-gap scan behavior
    // is kept for open()); hooks drive progress/cancel over the scan.
    // status_out receives a canonical RAR_* code, detail_out a human message
    // for openrar_archive_get_error.
    bool open_ex(const std::filesystem::path& arc_path, const std::string& password,
                 int& status_out, std::string& detail_out, const ReaderHooks& hooks = {},
                 bool strict_volumes = false);

    // Volume paths in the open set (primary first, then every extent volume,
    // deduplicated) — destination-guard input for the DLL layer.
    std::vector<std::filesystem::path> volume_paths() const;

    // Volume whose absence failed the most recent open or payload read; empty
    // when no volume was missing.
    const std::filesystem::path& missing_volume_path() const { return missing_volume_path_; }

    // Stream entries_[entry_index]'s payload into `out` (assumed open at
    // position 0; the CALLER owns durability: temp file, flush, atomic
    // rename). Verifies BLAKE2sp / CRC32 while writing and advances the
    // solid chain position on success. Precondition: entry_index is a
    // data-bearing file entry (directories and links are the caller's
    // concern). Returns RAR_OK or RAR_ERR_BAD_PASSWORD / RAR_ERR_ENCRYPTED /
    // RAR_ERR_CRC_MISMATCH / RAR_ERR_TRUNCATED / RAR_ERR_ABORTED /
    // RAR_ERR_MISSING_VOLUME / RAR_ERR_IO.
    int extract_entry_stream(size_t entry_index, io::FileStream& out, const ReaderHooks& hooks);

    // Extract into memory (preview path). Fails with RAR_ERR_NOMEM when the
    // uncompressed size exceeds max_bytes (checked before any work).
    int extract_entry_to_memory(size_t entry_index, std::vector<core::byte>& out,
                                uint64_t max_bytes, const ReaderHooks& hooks);

    // Streaming integrity test: verifies BLAKE2sp / CRC32 without retaining
    // output. Directory and link entries verify trivially (RAR_OK, no
    // callbacks). Error mapping as extract_entry_stream.
    int test_entry_stream(size_t entry_index, const ReaderHooks& hooks);

private:
    bool scan_archive(const ReaderHooks& hooks, bool strict_volumes, int& status_out,
                      std::string& detail_out);

    // ── Streaming internals (v1.3.0) ─────────────────────────────────────────
    // Chain bookkeeping (docs/invariants.md §1): reader index of the last
    // compressed entry fully decoded on this reader, -1 = none. Advances only
    // on complete, verified success — any failure or cancel leaves it, so a
    // retry re-runs catch-up cleanly.
    long long last_decoded_index_{-1};
    std::filesystem::path missing_volume_path_;

    // Reader index of the last data-bearing compressed file entry before
    // `idx` (services/stored/dirs skipped — they don't carry the chain).
    size_t prev_chain_index(size_t idx) const;
    // First compressed, non-service, non-solid entry at or before `idx`
    // (the run head a fresh-window decode must start from).
    size_t solid_run_head(size_t idx) const;
    // Solid position guarantee: runs catch-up (decode-and-discard of the run
    // prefix) when the chain position doesn't let entry `idx` decode from the
    // immediately preceding state. Returns RAR_OK or the failure code.
    int ensure_solid_position(size_t idx, const ReaderHooks& hooks);
    // Per-entry password → keys + PswCheck verdict. RAR_OK when the entry is
    // not encrypted (keys untouched); BAD_PASSWORD / ENCRYPTED / UNSUPPORTED
    // otherwise. Sets bad_password_ on a PswCheck mismatch.
    int derive_entry_keys(const ArchiveEntry& entry, crypto::Rar5Keys& keys);

    // Abort/IO flags an out_sink reports through (stream_payload cannot
    // interpret sink failures itself).
    struct SinkStatus {
        bool aborted{false};
        bool failed{false};
    };
    // Workhorse: stream entries_[idx]'s payload (stored or compressed,
    // decrypting when `keys` != null) into `out_sink`, hashing and verifying
    // checksums on the fly. The solid chain position must be guaranteed by
    // the caller (ensure_solid_position). Emits progress (produced, total)
    // per chunk; polls cancel per chunk and per flush.
    int stream_payload(size_t idx, crypto::Rar5Keys* keys,
                       const std::function<bool(const core::byte*, size_t)>& out_sink,
                       SinkStatus& status, const ReaderHooks& hooks);

    bool scan_archive();
    static std::filesystem::path derive_next_volume_name(const std::filesystem::path& cur,
                                                         bool old_numbering);
    static std::filesystem::path derive_first_volume_name(const std::filesystem::path& cur,
                                                          bool old_numbering);

    // Decode a compressed (method != 0) entry payload, continuing the shared
    // LZ dictionary/huffman state across solid entries when required.

    // Shared solid/window/unpacker-selection logic used by both
    // decode_compressed overloads. `sel.unpacker` survives only until
    // `sel.local` is destroyed (the caller holds sel through the call).
    struct UnpackerSelection {
        compress::Decompressor50* unpacker = nullptr;
        bool solid = false;
        std::unique_ptr<compress::Decompressor50> local;
    };
    bool select_unpacker(const ArchiveEntry& entry, UnpackerSelection& sel);

    bool decode_compressed(const ArchiveEntry& entry, const core::byte* src, size_t src_size,
                           std::function<bool(const core::byte*, size_t)> flush_cb);

    bool decode_compressed(const ArchiveEntry& entry,
                           std::function<size_t(core::byte*, size_t)> src_cb, size_t src_size,
                           std::function<bool(const core::byte*, size_t)> flush_cb);

    io::FileStream stream_;
    std::filesystem::path path_;
    std::string password_;
    bool bad_password_{false};
    bool saw_crypt_header_{false};
    bool crypt_unsupported_{false};
    core::uint64 sfx_offset_{0};
    format::MainBlock main_block_;
    std::vector<ArchiveEntry> entries_;
    bool header_encrypted_{false};
    format::CryptBlock header_crypt_{};

    // Persistent LZ state for solid archives: solid entries decode against
    // the window contents, Huffman tables and repeat distances left behind by
    // the previous entry, so one Decompressor50 must survive across per-entry
    // calls. Null until the first compressed entry of a solid archive is
    // decoded. solid_chain_ok_ tracks whether the shared state actually
    // reflects the archive position (any skip/failure invalidates it).
    std::unique_ptr<compress::Decompressor50> solid_unpacker_;
    bool solid_chain_ok_{false};

    // M9: symlinks/junctions this reader created during the current
    // extraction session. convert_self_links() may replace only these with
    // real directories; pre-existing user links on the machine are never
    // touched (safe links-to-directories conversion semantics).
    std::vector<std::filesystem::path> links_created_;
    void convert_self_links(const std::filesystem::path& dest_path);
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
