#ifndef OPENRAR_IO_CONTAINMENT_HPP
#define OPENRAR_IO_CONTAINMENT_HPP

#include "file_stream.hpp"
#include "path_util.hpp"

#include <filesystem>
#include <list>
#include <string>
#include <vector>

namespace openrar::io {

// ── Syscall-level path containment (v1.24.0 plan §1, SECURITY_ARCHITECTURE §4.1)
//
// Primary containment is OS-level: every directory component of an entry's
// destination is opened (or created) through an anchored syscall chain
// starting at a PINNED root handle, rejecting reparse points / symlinks on
// every component, and the file is written through the handle produced by
// that chain — there is no check-then-open window on the path itself.
// String sanitization (path_util) remains defense-in-depth only.
//
//   Windows: NtCreateFile with RootDirectory anchoring, every component
//     opened with FILE_OPEN_REPARSE_POINT and verified reparse-free; final
//     containment assertion via GetFinalPathNameByHandleW with \\?\-
//     canonicalized comparison on both sides.
//   Linux 5.6+: openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS) issued via
//     direct syscall (glibc wrappers landed in 2.36); availability probed
//     once per process.
//   POSIX fallback: openat() walk with O_NOFOLLOW per component; the final
//     anchor is verified by comparing the kernel-resolved path
//     (/proc/self/fd on Linux, F_GETPATH on macOS) against the root's.
//     Residual race (parent replaced between two opens) documented in the
//     plan; weaker than openat2, mitigated by the prefix assertion.
//
// 8.3-alias-shaped components (`NAME~X.ext`) are rejected before resolution
// everywhere: the walk fails closed and the CLI pre-filters with a report.

class ContainmentRoot {
public:
    ContainmentRoot() = default;
    ~ContainmentRoot();
    ContainmentRoot(const ContainmentRoot&) = delete;
    ContainmentRoot& operator=(const ContainmentRoot&) = delete;

    // Canonicalizes (lexically absolute + normal) and pins the root handle
    // for the whole session. The root must exist. Returns false on IO error.
    bool attach(const std::filesystem::path& root);
    bool attached() const { return pinned_ != nullptr; }
    const std::filesystem::path& canonical_root() const { return canonical_root_; }

    // Test hook: force the POSIX walk fallback even when openat2 is
    // available (no-op where openat2 does not exist / on Windows).
    void force_walk_fallback_for_test() { force_fallback_ = true; }

    // Opaque verified directory anchor (HANDLE on Windows, dirfd on POSIX).
    // Valid until the VerifiedDir is destroyed (or released into the LRU
    // cache — see below). Raw handle is intentionally not exposed outside
    // io/.
    class VerifiedDir {
    public:
        VerifiedDir();
        ~VerifiedDir();
        VerifiedDir(VerifiedDir&& other) noexcept;
        VerifiedDir& operator=(VerifiedDir&& other) noexcept;
        VerifiedDir(const VerifiedDir&) = delete;
        VerifiedDir& operator=(const VerifiedDir&) = delete;

        bool valid() const { return handle_ != nullptr; }
        void* raw() const { return handle_; } // for io-internal anchoring only

    private:
        friend class ContainmentRoot;
        void* handle_ = nullptr;
        bool owned_by_cache_ = false;
    };

    // Walks `rel_dir` ('/'-separated, sanitized, may be "" = root) from the
    // pinned root with no-follow semantics, creating missing components when
    // `create` is set (anchored mkdirat/NtCreateFile + reopen-verify race
    // protocol). Returns a verified anchor for the directory. Fails closed:
    // any reparse point / symlink / non-directory component, any 8.3-shaped
    // component, or any IO error rejects the whole path.
    bool resolve_dir(const std::string& rel_dir, bool create, VerifiedDir& out);

    // Opens/creates a LEAF inside a verified anchor without any path
    // resolution of the leaf name:
    //   create_temp — anchored create-exclusive (O_EXCL / FILE_CREATE) with
    //     GENERIC_WRITE|DELETE access; out_handle is adopted by the caller
    //     (FileStream::attach_os_handle) and written through directly.
    //   open_journal — anchored create-exclusive read/write.
    // On false, *out_handle is untouched.
    bool anchored_create(const VerifiedDir& dir, const std::string& leaf, void*& out_handle);
    bool anchored_create_journal(const VerifiedDir& dir, const std::string& leaf,
                                 void*& out_handle);

    // Anchored atomic rename of `from_leaf` to `to_leaf` INSIDE the verified
    // anchor (same directory — same volume by construction). Same CommitMode
    // semantics as file_stream's path cascade. POSIX-only: on Windows the
    // commit goes through the writer's own open handle
    // (FileStream::commit_rename_in) because FileRenameInformation renames
    // via the source handle, and the temp handle is exclusively held.
    bool anchored_rename(const VerifiedDir& dir, const std::string& from_leaf,
                         const std::string& to_leaf, CommitMode mode, int& last_error);

    // True when `leaf` inside the verified anchor is a reparse point
    // (symlink/junction), checked WITHOUT traversing it. False on lookup
    // failure (not there / IO error) — out is false both ways.
    bool leaf_is_reparse(const VerifiedDir& dir, const std::string& leaf, bool& out);

    // Deletes `leaf` inside the verified anchor without following it (used
    // for symlink leaves the POSIX-semantics rename cannot replace — NTFS
    // quirk: REPLACE over a symlink-to-existing-file reports success and
    // consumes the source without replacing the link). The caller must have
    // verified the leaf IS a reparse point first.
    bool anchored_unlink_leaf(const VerifiedDir& dir, const std::string& leaf);

    // Final containment assertion (Windows): the handle's kernel-resolved
    // path must equal canonical_root() + rel prefix + leaf, both sides \\?\-
    // canonicalized. POSIX callers use fd_prefix_verified() instead.
    bool final_path_inside(const void* handle, const std::string& rel_dir,
                           const std::string& leaf) const;

    // POSIX fallback verification: true when the fd's kernel-resolved path
    // is a strict prefix-path of the root's own kernel-resolved path.
    bool fd_prefix_verified(const void* handle) const;

private:
    // LRU of verified directory handles keyed by rel dir (Windows:
    // lower-cased). A cache hit skips the walk but never the final
    // containment assertion; cached handles pin verified inodes, so an
    // attacker renaming over a cached directory cannot redirect writes.
    static constexpr size_t kCacheCapacity = 64;
    struct CacheEntry {
        std::string key;
        void* handle = nullptr;
    };
    std::list<CacheEntry> cache_; // front = most recent
    void cache_put(const std::string& key, void* handle);
    void* cache_get(const std::string& key);
    void cache_clear();

    void* pinned_ = nullptr; // root handle (HANDLE / dirfd)
    std::filesystem::path canonical_root_;
    bool force_fallback_ = false;
#if defined(_WIN32)
    std::filesystem::path canonical_root_nt_; // \\?\-prefixed form
#endif
};

// 8.3 short-name alias shape (`NAME~1.ext`): components that Win32 would
// resolve through short-name alias space. Rejected, never mangled (plan §0).
bool is_83_alias_component(const std::string& component);

// True when any '/'-separated component of `rel` has the 8.3 alias shape.
// Used by the CLI pre-filter (skip-with-report) and enforced again in the
// walk (fail closed).
bool path_has_83_component(const std::string& rel);

// Splits a sanitized '/'-separated relative path into (dir, leaf). The leaf
// is "" for a path that ends in a separator (never produced by the
// sanitizer).
void split_archive_relpath(const std::string& rel, std::string& dir_out, std::string& leaf_out);

} // namespace openrar::io

#endif // OPENRAR_IO_CONTAINMENT_HPP
