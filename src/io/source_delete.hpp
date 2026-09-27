#ifndef OPENRAR_IO_SOURCE_DELETE_HPP
#define OPENRAR_IO_SOURCE_DELETE_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/io/source_delete.hpp — source deletion modes for -df / -dr / -dw
//  (v1.30.0 M2.5; the README switch contract made real).
//
//    Plain   — remove() (the `m` command's historical behavior; -df)
//    Recycle — Windows: SHFileOperationW with FOF_ALLOWUNDO (the Recycle
//              Bin). POSIX: there is no recycle bin — degrades to Plain
//              (the CLI prints a W: notice; the delete still happens ONLY
//              after the archive write succeeded).
//    Wipe    — the documented sequence: zero overwrite → truncate →
//              rename to a random temp name → delete (-dw). Best-effort
//              data erasure, not a certified secure-erase.
//
//  Callers delete ONLY after the archive write has fully succeeded (the
//  mutator's post-replace loop) — an aborted or failed add never deletes.
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <system_error>

namespace openrar::io {

enum class SourceDeleteMode {
    Plain,
    Recycle,
    Wipe,
};

// Deletes `path` per `mode`. Returns false with `ec` set on failure (the
// caller reports and fails the command — a source that could not be deleted
// after a successful add is an error, never silence).
bool delete_source_securely(const std::filesystem::path& path, SourceDeleteMode mode,
                            std::error_code& ec);

} // namespace openrar::io

#endif // OPENRAR_IO_SOURCE_DELETE_HPP
