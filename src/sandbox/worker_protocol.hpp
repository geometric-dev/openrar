#ifndef OPENRAR_SANDBOX_WORKER_PROTOCOL_HPP
#define OPENRAR_SANDBOX_WORKER_PROTOCOL_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/worker_protocol.hpp — payload shapes for the broker↔worker
//  frames (v1.30.0). Little-endian packed structs; every payload must parse
//  with the same hostile-input discipline as the frame layer (size-checked,
//  never trusted). These shapes are sandbox-INTERNAL wire format — the ABI
//  structs in include/openrar/ are a separate, frozen surface; the entry
//  shape here mirrors the 64-byte layout by value because both the broker
//  (CLI) and the worker (engine) already speak it, but nothing in this
//  directory includes src/api.
// ─────────────────────────────────────────────────────────────────────────────

#include "../core/types.hpp"
#include "ipc.hpp" // kMaxFramePayload

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openrar::sandbox {

#pragma pack(push, 1)

// Mirrors the 64-byte entry layout (openrar_archive_entry_t / ArchiveEntryOut
// — see docs/abi-freeze.md §3): identical field order and widths so the CLI's
// downstream consumers can share conversion helpers.
struct WireEntry {
    core::uint32 path_offset;
    core::uint32 path_len;
    core::uint32 is_dir;
    core::uint32 method;
    core::uint32 is_encrypted;
    core::uint32 crc32;
    core::uint64 size;
    core::uint64 packed_size;
    core::uint64 mtime; // UNIX seconds
    // Sandbox-INTERNAL wire flags (this struct mirrors the 64-byte ABI layout
    // by FIELD ORDER, but is a separate surface — the ABI's _pad stays zero).
    // _pad[0] bit 0 = is_service (QO/CMT/RR service headers are not file
    // entries and must not surface in list/test output); _pad[1] stays zero.
    core::uint64 _pad[2];
};
static_assert(sizeof(WireEntry) == 64, "WireEntry must mirror the 64-byte ABI layout");
inline constexpr core::uint64 kWIRE_FLAG_SERVICE = 1ull;

struct WireScanHeader {
    core::uint32 entry_count;
    core::uint32 reserved; // zero
    core::uint64 paths_bytes;
};

struct WireExtractReq {
    core::uint32 entry_index;
    core::uint32 reserved; // zero
};

struct WireEntryResult {
    core::uint32 entry_index;
    core::uint32 reserved; // zero
    core::int32 rc;        // RarError value
    core::uint32 pad;      // zero
    core::uint64 bytes_emitted;
};

struct WireError {
    core::int32 rc; // RarError value
    // followed by (payload_size - sizeof(WireError)) bytes of UTF-8 detail
};

// v1.30.0 M3a/b proof-of-denial self-test results (SelftestResult payload).
// The worker ATTEMPTS each probe and reports what the OS actually did —
// the e2e suites assert the DENIAL matrix per sandbox model (v1.29 Gate 0
// directive: an observed denial, never mere worker liveness). errs are the
// platform error codes (Windows GetLastError / POSIX errno); 0 = success.
struct WireSelftest {
    core::uint32 write_ok;       // 1 if the probe file write SUCCEEDED
    core::uint32 write_err;      // platform error when write_ok == 0
    core::uint32 net_ok;         // 1 if a loopback socket()/connect() SUCCEEDED
    core::uint32 net_err;        // platform error when net_ok == 0
    core::uint32 volume_read_ok; // 1 if the inherited volume handle was usable
    core::uint32 volume_read_err;
    core::uint32 pad[2]; // zero
};

#pragma pack(pop)

// ── ScanResult payload: [WireScanHeader][entry_count × WireEntry][paths] ─────
struct ScanView {
    std::vector<WireEntry> entries;
    std::string paths; // contiguous UTF-8 blob; entries' path_offset index into it
};

// Serializes a scan result. Returns false when the wire would overflow the
// frame payload cap (u32 path space — the L17 guard, sandbox-local).
bool pack_scan_result(const ScanView& view, std::vector<core::byte>& payload);

// Parses a ScanResult payload. Returns false on ANY size inconsistency
// (truncation, count mismatch, path blob shorter than the widest
// path_offset+len) — the broker treats that as a protocol violation.
bool unpack_scan_result(const core::byte* payload, size_t len, ScanView& out);

// ExtractReq / EntryResult / Error payload helpers (size-checked).
bool pack_extract_req(core::uint32 index, std::vector<core::byte>& payload);
bool unpack_extract_req(const core::byte* payload, size_t len, core::uint32& index);
bool pack_entry_result(core::uint32 index, core::int32 rc, core::uint64 bytes,
                       std::vector<core::byte>& payload);
bool unpack_entry_result(const core::byte* payload, size_t len, WireEntryResult& out);
bool pack_error(core::int32 rc, const std::string& detail, std::vector<core::byte>& payload);
bool unpack_error(const core::byte* payload, size_t len, core::int32& rc, std::string& detail);

// Selftest payloads (v1.30.0 M3a/b).
bool pack_selftest(const WireSelftest& st, std::vector<core::byte>& payload);
bool unpack_selftest(const core::byte* payload, size_t len, WireSelftest& out);

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_WORKER_PROTOCOL_HPP
