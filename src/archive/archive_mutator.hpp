#ifndef OPENRAR_ARCHIVE_ARCHIVE_MUTATOR_HPP
#define OPENRAR_ARCHIVE_ARCHIVE_MUTATOR_HPP

#include "archive_reader.hpp"
#include "../compress/compress_plan.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace openrar::archive {

// Which times FHEXTRA_HTIME carries (per -ts switches). The file header's
// 32-bit mtime field is always written from the file's mtime regardless.
// Flags-as-namespace, the same bitmask convention as format::header_flags.
namespace time_flags {
inline constexpr core::uint32 MTIME = 0x1;
inline constexpr core::uint32 CTIME = 0x2;
inline constexpr core::uint32 ATIME = 0x4;
} // namespace time_flags

class ArchiveMutator {
public:
    // Delete entries matching one or more wildcard masks (command 'd')
    static bool delete_entries(const std::filesystem::path& arc_path,
                               const std::vector<std::string>& masks);

    // Delete entries by identity: `header_offsets` are header_offset values
    // from a fresh ArchiveReader walk of arc_path (the DLL mutation surface
    // translates its listing indices up front, so matching can never break
    // on names containing '*'/'?' the way mask translation would). Refuses
    // locked and multi-volume archives and header-encrypted archives (this
    // surface takes no password), and — per docs/invariants.md §1 — any
    // deletion that would orphan a retained solid entry (suffix-only
    // delete). Returns RAR_OK or a RAR_ERR_* code with detail_out set; the
    // archive on disk is untouched unless RAR_OK.
    static int delete_entries_by_index(const std::filesystem::path& arc_path,
                                       const std::vector<core::uint64>& header_offsets,
                                       std::string& detail_out);

    // Lock archive against further modifications (command 'k')
    static bool lock_archive(const std::filesystem::path& arc_path);

    // Add file to archive (command 'a') - optionally with SFX stub + vol_size + password
    // password: empty = no encryption. Non-empty enables per-file AES-256-CBC (spec §4)
    // with a fresh salt+IV per file. encrypt_headers (-hp) additionally emits a
    // HEAD_CRYPT block and AES-encrypts every archive header; it implies file
    // encryption and is refused when it would have to mix with a plaintext
    // archive's existing headers.
    static bool add_file_to_archive(const std::filesystem::path& arc_path,
                                    const std::filesystem::path& src_file,
                                    const std::string& arc_entry_name, int method = 3,
                                    const std::filesystem::path& sfx_stub_path = {},
                                    core::uint64 vol_size = 0, const std::string& password = "",
                                    bool encrypt_headers = false, bool solid = false);

    // Volume-aware add ( -v )
    static bool add_file_to_archive_vol(const std::filesystem::path& arc_path,
                                        const std::filesystem::path& src_file,
                                        const std::string& arc_entry_name, int method,
                                        core::uint64 vol_size, const std::string& password = "",
                                        bool solid = false);

    // Move file to archive and delete from disk upon success (command 'm')
    static bool move_file_to_archive(const std::filesystem::path& arc_path,
                                     const std::filesystem::path& src_file,
                                     const std::string& arc_entry_name, int method = 3,
                                     const std::filesystem::path& sfx_stub_path = {},
                                     const std::string& password = "",
                                     bool encrypt_headers = false);

    // Spool guard ensuring temporary spool files are unlinked on any unwound
    // exception, error, or early abort.
    class SpoolFileGuard {
    public:
        explicit SpoolFileGuard(std::filesystem::path path = {}) : path_(std::move(path)) {}
        ~SpoolFileGuard() { cleanup(); }
        SpoolFileGuard(const SpoolFileGuard&) = delete;
        SpoolFileGuard& operator=(const SpoolFileGuard&) = delete;
        SpoolFileGuard(SpoolFileGuard&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
        SpoolFileGuard& operator=(SpoolFileGuard&& other) noexcept {
            if (this != &other) {
                cleanup();
                path_ = std::move(other.path_);
                other.path_.clear();
            }
            return *this;
        }
        void disarm() noexcept { path_.clear(); }
        void commit() noexcept { disarm(); }
        void reset(std::filesystem::path p) { cleanup(); path_ = std::move(p); }
        const std::filesystem::path& path() const noexcept { return path_; }
        void cleanup() noexcept {
            if (!path_.empty()) {
                std::error_code ec;
                std::filesystem::remove(path_, ec);
                path_.clear();
            }
        }
    private:
        std::filesystem::path path_;
    };

    static constexpr size_t SPOOL_MEMORY_THRESHOLD = 16 * 1024 * 1024; // 16 MiB

    // Fully prepared payload for batch archive creation: everything the CLI
    // can compute without touching the archive file (read + CRC + compress +
    // optional per-file encryption). Parallel-safe: preparation is a pure
    // per-file computation with no shared state.
    struct PreparedAdd {
        format::FileBlock fb;
        std::vector<core::byte> payload;    // packed (possibly encrypted) data if in memory
        std::filesystem::path spool_path;   // path to spooled payload on disk if > threshold
        std::filesystem::path src_path;     // original file, removed for move after success
        std::string entry_name;
        bool delete_source{false};
        bool needs_deferred_crc{false};
        std::vector<PreparedAdd> child_services;

        PreparedAdd() = default;
        ~PreparedAdd() {
            if (!spool_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(spool_path, ec);
                spool_path.clear();
            }
        }
        PreparedAdd(const PreparedAdd&) = delete;
        PreparedAdd& operator=(const PreparedAdd&) = delete;
        PreparedAdd(PreparedAdd&&) noexcept = default;
        PreparedAdd& operator=(PreparedAdd&&) noexcept = default;
    };

    // Stage 1 of batch add: read + CRC + compress (+ store fallback) + encrypt
    // one file into `out`. method may be downgraded to 0 (store) exactly like
    // the single-file path does. On success out.entry_name == arc_entry_name.
    // times_mask selects the FHEXTRA_HTIME records (default: mtime only).
    // dict_size selects the dictionary window for compressed methods
    // (0 uses tuned defaults per method; 1..15 translates to legacy log2
    // 128 KiB..2 GiB for backward compatibility; > 15 is treated directly as exact byte size).
    // Ignored for stored entries. The value is written into the
    // header's win_size and must match the compressor's dictionary.
    // want_streams / want_acl attach NTFS ADS and Security ACL child records.
    static bool prepare_add_file(const std::filesystem::path& src_file,
                                 const std::string& arc_entry_name, int method,
                                 const std::string& password, PreparedAdd& out,
                                 core::uint32 times_mask = time_flags::MTIME,
                                 core::uint64 dict_size = 0, bool want_streams = false,
                                 bool want_acl = false, bool is_solid = false);

    // Stage 1 variant for a directory: emits a directory record (FHFL_DIRECTORY,
    // no data area) carrying the directory's timestamps. Encryption does not
    // apply to directory records.
    static bool prepare_add_dir(const std::filesystem::path& src_dir,
                                const std::string& arc_entry_name, PreparedAdd& out,
                                core::uint32 times_mask = time_flags::MTIME, bool want_acl = false);

    // Stage 1 variant for a symbolic link: emits a symlink record (FHEXTRA_REDIR,
    // redir_type = 2 on Windows, 1 on POSIX, no data area) carrying the link's timestamps.
    static bool prepare_add_symlink(const std::filesystem::path& src_symlink,
                                    const std::string& arc_entry_name, const std::string& target,
                                    bool is_dir_target, PreparedAdd& out,
                                    core::uint32 times_mask = time_flags::MTIME,
                                    bool want_acl = false);

    // Stage 1 variant for a hard link: emits a hardlink record (FHEXTRA_REDIR,
    // redir_type = 4, no data area) pointing to target_entry_name.
    static bool prepare_add_hardlink(const std::filesystem::path& src_file,
                                     const std::string& arc_entry_name,
                                     const std::string& target_entry_name, PreparedAdd& out,
                                     core::uint32 times_mask = time_flags::MTIME,
                                     bool want_acl = false);

    // Query disk file last-modification time as unix epoch seconds.
    static bool get_file_mtime(const std::filesystem::path& path, core::uint64& mtime_out);

    // Stage 2 of batch add: one sequential pass that writes SFX stub, archive
    // prefix, every prepared file in vector order, and ENDARC, then atomically
    // replaces arc_path and deletes sources flagged delete_source. Entries in
    // files are prepared beforehand (read + compressed + encrypted) and
    // caller owns the concurrency; this function only writes.
    // Progress callback (optional): invoked on the writer thread before each
    // entry is committed.
    static bool
    write_batch_add(const std::filesystem::path& arc_path, std::vector<PreparedAdd>& files,
                    const std::filesystem::path& sfx_stub_path = {},
                    const std::string& password = "", bool encrypt_headers = false,
                    const std::function<void(size_t, const std::string&)>& on_write = {},
                    bool solid = false, const std::vector<core::byte>& comment = {},
                    bool want_qo = false, bool want_ams = false);

    // Status-code variant of write_batch_add (open_ex pattern: the bool
    // overload above delegates here and drops the detail). Same atomicity
    // and 'u' semantics, plus the mutation guards of docs/invariants.md §1:
    // replacing any member of a solid block — a solid entry, or the head of
    // a run that still has retained solid members — is refused with
    // RAR_ERR_UNSUPPORTED_FEATURE ("cannot replace entry in solid archive
    // without recompressing chain"). Refuses locked and multi-volume
    // archives the same way. detail_out is set for every non-OK return.
    static int write_batch_add_ex(const std::filesystem::path& arc_path,
                                  std::vector<PreparedAdd>& files,
                                  const std::filesystem::path& sfx_stub_path,
                                  const std::string& password, bool encrypt_headers,
                                  const std::function<void(size_t, const std::string&)>& on_write,
                                  bool solid, const std::vector<core::byte>& comment,
                                  std::string& detail_out, bool want_qo = false,
                                  bool want_ams = false);

    // SFX stub upper bound: the reader locates the signature behind the stub
    // by scanning at most 4 MiB (10-sfx.md:15), so a larger module would
    // produce an archive nothing can open. Modules are rejected with exit 7
    // before any output is created (10-sfx.md:80).
    static constexpr core::uint64 MAX_SFX_SIZE = 0x400000ULL; // 4 MiB

    // Helpers for CLI: resolve default.sfx lookup and SetSFXExt
    static std::filesystem::path resolve_sfx_stub(const std::string& sfx_name_raw,
                                                  const char* argv0 = nullptr);
    static std::filesystem::path apply_sfx_extension(const std::filesystem::path& arc_path);

    static bool move_file_to_archive_vol(const std::filesystem::path& arc_path,
                                         const std::filesystem::path& src_file,
                                         const std::string& arc_entry_name, int method,
                                         core::uint64 vol_size, const std::string& password = "",
                                         bool solid = false);

    static bool convert_to_sfx(const std::filesystem::path& arc_path,
                               const std::filesystem::path& sfx_stub_path, std::string& err_detail);

private:
    // Stages of the write pipeline: planning resolves solid chaining and method decisions
    // across all batch entries up front before execution begins.
    static compress::CompressPlan plan_batch(const std::vector<PreparedAdd>& files, bool solid,
                                             bool continue_solid_stream);
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_ARCHIVE_MUTATOR_HPP
