#ifndef OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
#define OPENRAR_ARCHIVE_ARCHIVE_READER_HPP

#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "../format/headers.hpp"
#include "archive_entry.hpp"
#include "extraction_limits.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace openrar::compress {
class Decompressor50;
enum class DecompressErrorCode;
} // namespace openrar::compress

namespace openrar::io {
class ExtractionSession; // per-run journal anchor for atomic extraction (v1.24 M1)
class AtomicWriter;      // temp-in-destination atomic writer (v1.24 M1)
} // namespace openrar::io

namespace openrar::archive {
struct CollisionPair; // collision_detector.hpp (v1.24 M3)
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
    bool has_recovery_record() const;
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

    // Archive-internal collision detection (v1.24 M3, plan §3): runs the
    // four-class detector over the final merged entry list with service
    // headers filtered. Returns true and fills `out` when the archive is
    // self-contradictory — the caller must abort BEFORE writing anything.
    bool detect_collisions(std::vector<CollisionPair>& out) const;

    // Extract entry (stored or compressed) directly to disk. `limits`/
    // `state`, when provided, are debited by hardlink fallback copies
    // (v1.24 plan §7.2 — excess aborts the entry).
    bool extract_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                       const std::string& password = "", const ExtractionLimits* limits = nullptr,
                       LimitState* state = nullptr);

    // Extract stored (method 0) entry directly to disk
    bool extract_store_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                             const std::string& password = "",
                             const ExtractionLimits* limits = nullptr, LimitState* state = nullptr);

    void set_keep_broken(bool kb) { keep_broken_ = kb; }
    bool keep_broken() const { return keep_broken_; }
    void set_extract_symlinks(bool es) { extract_symlinks_ = es; }
    bool extract_symlinks() const { return extract_symlinks_; }

    // v1.26 M3 (security-arch §4.4 reconciliation): absurd archive mtimes
    // clamp to these bounds when file/dir times are applied — the v1.24
    // bounds helpers finally wired into time application. Defaults:
    // 1970-01-01 .. 3000-01-01.
    void set_mtime_bounds(const MtimeBounds& bounds) { mtime_bounds_ = bounds; }
    // True when the MOST RECENT extract_entry call clamped a timestamp
    // (file commit or deferred dir-meta build). The CLI surfaces it as the
    // timestamp_clamped security flag plus a report line.
    bool last_mtime_clamped() const { return last_mtime_clamped_; }

    // v1.24 plan §7.1: --preserve-suid admin opt-in — when false (default),
    // archived POSIX modes lose their SUID/SGID/sticky bits.
    void set_preserve_suid(bool ps) { preserve_suid_ = ps; }

    // v1.27 plan §1.4: --xattr-security admin opt-in — when false (default),
    // security.*/trusted.* extended attributes are never restored from the
    // record data (user.*/com.apple.metadata.* restore by default).
    void set_restore_xattr_security(bool v) { restore_xattr_security_ = v; }

    // Restore policy shared by file commit and deferred dir restoration:
    // user.* + com.apple.metadata.* always; security.*/trusted.* only with
    // the opt-in (the --preserve-suid model); everything else never — the
    // system.* ACL side door, transport-provenance namespaces, resource
    // forks (SECURITY_ARCHITECTURE §4.3).
    static bool xattr_restorable(const std::string& name, bool security_opt_in);

    // Mode policy shared by file chmod and deferred dir restoration:
    // umask-bounded, SUID/SGID/sticky masked unless preserve_suid.
    static core::uint32 sanitize_extract_mode(core::uint32 raw_mode, bool preserve_suid,
                                              core::uint32 umask_bits);

    // v1.24 plan §6.3: DLL/WASM extraction calls are separate hardlink
    // sessions — clears the session-scoped link registry (and deferred
    // metadata) so per-call surfaces never link against a previous call's
    // outputs. The CLI's whole x|e run is naturally one session (fresh
    // readers), so it never needs to call this.
    void begin_extraction_session();

    // v1.24 plan §7.4: applies deferred directory metadata bottom-up
    // (deepest first) — directory mtimes and modes, after all writes
    // complete. Best-effort: failures are skipped, never fatal.
    void apply_deferred_dir_metadata();

    // v1.25 M2: mapped scan engine (default on; fail-open to buffered).
    // --no-mmap / OPENRAR_NO_MMAP=1 force the buffered engine.
    void set_use_mapped_scan(bool v) { use_mapped_scan_ = v; }
    bool use_mapped_scan() const { return use_mapped_scan_; }
    bool use_mapped_scan_{true};

    // v1.25 M3: random-read region of a STORED entry's payload (mapped view
    // per region when available, buffered otherwise — §5.2: never the
    // extraction input). Encrypted/compressed entries are refused. Verifies
    // the entry checksum when the region covers the whole payload.
    int read_payload_region(size_t entry_index, core::uint64 offset, core::uint32 length,
                            void* out_buf, size_t out_len, size_t* out_written);

    // v1.24 M2: explicit containment root — the extraction destination,
    // canonicalized and pinned for the whole session (plan §1.3). When unset,
    // the root is inferred per-entry from dest_path by popping the entry's
    // own sanitized directory components (works for full-path and flat
    // layouts). Every regular-file write then goes through the no-follow
    // containment walk and is committed through the verified handle.
    void set_extraction_root(const std::filesystem::path& root) { extraction_root_ = root; }

    // Test hook (gate-1 suite): force the POSIX openat walk fallback even
    // where openat2 exists. No-op on Windows.
    void force_containment_fallback_for_test();

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
                 bool strict_volumes = false, const ExtractionLimits* limits = nullptr,
                 LimitState* state = nullptr);

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
    // RAR_ERR_MISSING_VOLUME / RAR_ERR_IO / RAR_ERR_LIMIT_EXCEEDED.
    int extract_entry_stream(size_t entry_index, io::FileStream& out, const ReaderHooks& hooks,
                             const ExtractionLimits* limits = nullptr, LimitState* state = nullptr);

    // Stream entries_[entry_index]'s payload into a caller-supplied sink function.
    int extract_entry_sink(size_t entry_index,
                           const std::function<bool(const core::byte*, size_t)>& out_sink,
                           const ReaderHooks& hooks = {}, const ExtractionLimits* limits = nullptr,
                           LimitState* state = nullptr);

    // Extract into memory (preview path). Fails with RAR_ERR_NOMEM when the
    // uncompressed size exceeds max_bytes (checked before any work).
    int extract_entry_to_memory(size_t entry_index, std::vector<core::byte>& out,
                                uint64_t max_bytes, const ReaderHooks& hooks,
                                const ExtractionLimits* limits = nullptr,
                                LimitState* state = nullptr);

    // Decodes the archive's CMT service-header payload (the SFX directive
    // carrier). First CMT wins; subsequent CMT headers in a crafted archive
    // are ignored. Returns false — with out left empty — when the archive
    // has no comment, the decoded size exceeds 1 MiB (directives disabled,
    // never an archive error), or decoding fails. v1.23.0 SFX (plan §3).
    bool read_archive_comment(std::vector<core::byte>& out);

    // Streaming integrity test: verifies BLAKE2sp / CRC32 without retaining
    // output. Directory and link entries verify trivially (RAR_OK, no
    // callbacks). Error mapping as extract_entry_stream.
    int test_entry_stream(size_t entry_index, const ReaderHooks& hooks,
                          const ExtractionLimits* limits = nullptr, LimitState* state = nullptr);

private:
    bool scan_archive(const ReaderHooks& hooks, bool strict_volumes, int& status_out,
                      std::string& detail_out, const ExtractionLimits* limits = nullptr,
                      LimitState* state = nullptr);

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
        bool limit_exceeded{false};
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
    compress::DecompressErrorCode last_decompress_error_{};

    // M9: symlinks/junctions this reader created during the current
    // extraction session. convert_self_links() may replace only these with
    // real directories; pre-existing user links on the machine are never
    // touched (safe links-to-directories conversion semantics).
    std::vector<std::filesystem::path> links_created_;
    bool keep_broken_{false};
    // v1.24 plan §6.1: links are DEFAULT DENY — symlinks, hardlinks and
    // junctions extract only with the explicit opt-in (-ol / set_extract_
    // symlinks(true)). Opt-in is decoupled from path resolution: even with
    // links enabled, absolute/escaping links are rejected and every regular
    // file write still goes through the containment walk.
    bool extract_symlinks_{false};

    // v1.26 M3: timestamp clamping bounds + the per-entry clamp flag.
    MtimeBounds mtime_bounds_{};
    bool last_mtime_clamped_{false};

    // Session-scoped link registry (v1.24 plan §6.3): absolute normalized
    // paths of files created by THIS extraction session. Hardlink entries
    // may only target files in the registry — a hardlink to any pre-existing
    // user file is skipped. One extract_all / one CLI x|e invocation is one
    // session; DLL/WASM calls begin a fresh session per call
    // (begin_extraction_session).
    std::set<std::string> session_created_paths_;

    // Deferred directory metadata (v1.24 plan §7.4): directories are created
    // permissive with default timestamps; (path, mode, mtime) stack here and
    // are applied BOTTOM-UP (deepest first) by apply_deferred_dir_metadata()
    // after all writes complete — preserving directory mtimes that children
    // would clobber and letting read-only directories receive children.
    struct PendingDirMeta {
        std::filesystem::path path;
        bool has_mode = false;
        core::uint32 mode = 0; // POSIX mode bits (host_os == 1 archives)
        bool has_mtime = false;
        core::uint64 mtime_unix = 0;
        // v1.27: allow-listed attributes restore bottom-up with the rest of
        // the directory's deferred metadata.
        std::vector<format::FileBlock::FileXattr> xattrs;
    };
    std::vector<PendingDirMeta> pending_dir_meta_;

    void convert_self_links(const std::filesystem::path& dest_path);
    static bool ensure_parent_dir(const std::filesystem::path& dest_path,
                                  const std::string& entry_name);
    bool preserve_suid_{false};
    bool restore_xattr_security_{false}; // --xattr-security (v1.27)

    // v1.24 M5: impl bodies of the public extract wrappers — the wrappers
    // record successful destinations into session_created_paths_ (plan §6.3).
    bool extract_entry_impl(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                            const std::string& password, const ExtractionLimits* limits,
                            LimitState* state);
    bool extract_store_entry_impl(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                                  const std::string& password, const ExtractionLimits* limits,
                                  LimitState* state);

    // Atomic extraction (v1.24 M1): per-reader journal anchor, created lazily
    // on the first file write. One journal per destination directory the
    // reader touches; readers are thread-confined, so no internal locking.
    std::unique_ptr<io::ExtractionSession> extraction_session_;
    io::ExtractionSession& ensure_extraction_session();
    std::filesystem::path extraction_root_;

    // v1.24 M2: containment wiring for the regular-file write paths —
    // attaches the root (explicit or inferred), derives the caller-layout
    // relative directory, and opens the writer through the verified anchor.
    bool open_contained_writer(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                               io::AtomicWriter& writer);
    // Contained directory materialization (dir records).
    bool create_contained_dir(const ArchiveEntry& entry, const std::filesystem::path& dest_path);
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
