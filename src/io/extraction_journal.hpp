#ifndef OPENRAR_IO_EXTRACTION_JOURNAL_HPP
#define OPENRAR_IO_EXTRACTION_JOURNAL_HPP

#include "file_stream.hpp"

#include <filesystem>
#include <map>

namespace openrar::io {

// ── Atomic extraction foundation (v1.24.0 plan §2) ──────────────────────────
//
// Every extracted file is written to `<name>.<128-bit-crypto-random>.tmp`
// created in the destination directory (same volume — required for the atomic
// rename) and committed by a no-follow rename cascade (file_stream.hpp
// atomic_rename_commit / FileStream::commit_rename). Abandoned temps are
// cleaned through a per-run journal manifest, never by pattern matching
// (SECURITY_ARCHITECTURE.md §3.3): a pattern sweep could be fed pre-planted
// victim names, a journal replay can only delete what a live run recorded
// before creating the temp, and only while it holds the journal lock.
//
// Journals are per directory (one `.openrar_journal_<pid>_<time>_<nonce>.tmp`
// in every destination directory the run touches), held under an exclusive
// advisory lock for the run's lifetime, and closed + unlinked as soon as no
// temp is in flight there — descriptor use is bounded by extraction
// parallelism, not tree size. Durability ordering: a temp's record is fsynced
// to the journal BEFORE the temp file is created, so a crash can never leave
// an unreferenced orphan. (Linux `O_TMPFILE` + `linkat(AT_EMPTY_PATH)` would
// remove crash orphans entirely and is a post-1.24 follow-up; the journal
// covers all platforms uniformly.)

class ExtractionSession {
public:
    ExtractionSession();
    ~ExtractionSession();

    ExtractionSession(const ExtractionSession&) = delete;
    ExtractionSession& operator=(const ExtractionSession&) = delete;

    // Best-effort startup sweep: scans `dir` for `.openrar_journal_*.tmp`
    // files, locks each, and — for journals no live process holds — deletes
    // the temps they reference and unlinks the journal. Locked journals (live
    // concurrent extractors) are skipped untouched. Validation per record:
    // the referenced path must lie IN `dir` and match this build's exact temp
    // shape (`<name>.<32 lowercase hex>.tmp`), so a forged journal can only
    // ever name candidates we would have created ourselves, and never
    // anything outside the directory the attacker wrote the journal into.
    // Garbage or torn records are ignored (checksummed records, conservative
    // stop). Returns true if the directory could be scanned.
    static bool sweep_directory(const std::filesystem::path& dir);

    // True when the filename matches this build's temp shape
    // (`<name>.<32hex>.tmp`, non-empty name part).
    static bool matches_temp_shape(const std::string& filename);

    // Called by AtomicWriter::open before the temp exists: sweeps stale
    // journals in `dir` (unless this session already holds one there),
    // creates + locks this run's journal for `dir` when absent, and appends
    // an fsynced record naming `temp_abs`. False only on IO failure — the
    // caller must not create the temp then (unreferenced-orphan rule).
    bool register_temp(const std::filesystem::path& dir, const std::filesystem::path& temp_abs);

    // The temp's lifecycle ended (committed or deleted). Closes and unlinks
    // the directory's journal when nothing is in flight. No-op for unknown
    // temps.
    void release_temp(const std::filesystem::path& temp_abs);

    // Test/introspection: raw journal content for `dir` (empty when this
    // session holds none). Reads through the owning handle — the exclusive
    // lock deliberately makes the file unopenable by anything else.
    std::string journal_content(const std::filesystem::path& dir);

private:
    struct DirJournal {
        void* file = nullptr; // LockedFile (extraction_journal.cpp); opaque here
        unsigned in_flight = 0;
    };

    static void journal_name(std::filesystem::path& out);

    std::map<std::filesystem::path, DirJournal> journals_;
    std::map<std::filesystem::path, std::filesystem::path> journal_paths_;
};

// RAII temp writer: creates a crypto-random temp next to the destination,
// journal-referenced (durable-first), and commits it with the atomic rename
// cascade. On destruction anything not committed is deleted.
class AtomicWriter {
public:
    AtomicWriter();
    ~AtomicWriter();

    AtomicWriter(const AtomicWriter&) = delete;
    AtomicWriter& operator=(const AtomicWriter&) = delete;

    // Creates the journal record then the temp file (CreateNew; retries with
    // a fresh random name on the astronomically-unlikely collision). The
    // destination's parent directory must already exist.
    bool open(ExtractionSession& session, const std::filesystem::path& dest);

    FileStream& stream() { return stream_; }
    bool is_open() const { return stream_.is_open(); }

    // Flush + fsync, then atomic commit:
    //   ReplaceExisting — POSIX-semantics rename that replaces the leaf
    //     without following it (a planted symlink at the destination is
    //     replaced, not traversed). A read-only destination fails the commit
    //     (respected, never clobbered): on Windows via the missing
    //     IGNORE_READONLY flag, on POSIX via a W_OK pre-check.
    //   NoClobber — fails atomically when the destination exists.
    // Terminal either way: on false the temp has already been disposed of
    // and the destination is untouched.
    bool commit(CommitMode mode = CommitMode::ReplaceExisting);

    // Closes and deletes the temp. The journal record becomes stale but is
    // harmless: the name no longer exists, and sweeps verify the shape.
    void abandon();

    const std::filesystem::path& dest_path() const { return dest_path_; }
    const std::filesystem::path& temp_path() const { return temp_path_; }

private:
    ExtractionSession* session_ = nullptr;
    FileStream stream_;
    std::filesystem::path dest_path_; // absolute (CWD-pinned at open)
    std::filesystem::path temp_path_; // absolute
    bool journaled_ = false;
    bool finished_ = false;
};

} // namespace openrar::io

#endif // OPENRAR_IO_EXTRACTION_JOURNAL_HPP
