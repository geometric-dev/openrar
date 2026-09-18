#ifndef OPENRAR_ARCHIVE_RAR_ERRORS_HPP
#define OPENRAR_ARCHIVE_RAR_ERRORS_HPP

#include "../core/types.hpp"

namespace openrar::archive {

// ── Error codes — canonical definition of the shared ABI contract ──────────
// Lives in its own header so the always-built engine layers (archive_reader,
// archive_mutator) can return contract codes without depending on the
// optional buffer layer (OPENRAR_INMEM_ARCHIVE). Re-exported by
// src/api/abi_contract.hpp to the DLL and WASM surfaces; JS mirrors it as
// RarErrorCode (wasm/js/openrar-archive.d.ts).
enum BufferArchiveError : int {
    RAR_OK = 0,
    RAR_ERR_PARTIAL_OK = 1,
    RAR_ERR_NOT_RAR = -1,
    RAR_ERR_UNSUPPORTED_FEATURE = -2,
    RAR_ERR_TRUNCATED = -3,
    RAR_ERR_CRC_MISMATCH = -4,
    RAR_ERR_NOMEM = -5,
    RAR_ERR_IO = -6,
    RAR_ERR_BAD_PASSWORD = -7,
    // -8 intentionally skipped (reserved; never assigned to preserve binary
    //    compatibility with consumers that range-check known error codes).
    RAR_ERR_INVALID_ARG = -9,
    // -10 intentionally skipped (reserved; same rationale as -8).
    RAR_ERR_ABORTED = -11,
    // Archive headers are encrypted (HEAD_CRYPT): a password is required
    // before any header can be read. Emitted only by list_file_stream and the
    // DLL _ex listing exports; BufferArchive::list keeps its historical
    // RAR_ERR_UNSUPPORTED_FEATURE for the same condition.
    RAR_ERR_ENCRYPTED = -12,
    // A volume of a multi-volume set is missing (explicitly required by
    // split_after / ENDARC NEXTVOL flags or the extent chain). Emitted by the
    // DLL file-mode handle exports only (ArchiveReader::scan_archive keeps
    // its tolerant CLI behavior of stopping the walk at the missing volume).
    RAR_ERR_MISSING_VOLUME = -13,
    // The target archive is held open by a file-mode handle in this process
    // (open_file keeps the volume open with FILE_SHARE_READ, so a rewrite
    // under it would fail opaquely on Windows). Emitted by the DLL mutation
    // exports only, up front before any temp file is created.
    RAR_ERR_BUSY = -14,
    // A caller-imposed resource limit (ExtractionLimits) was exceeded: per-member
    // output cap, cumulative total-output cap, or header count/byte cap. The value
    // is assigned explicitly to prevent collisions with future additions and because
    // the C ABI header and the JS TypeScript mirror must match this exact integer.
    // Gaps at -8 and -10 are documented above. Next available slot: -16.
    RAR_ERR_LIMIT_EXCEEDED = -15,
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_RAR_ERRORS_HPP
