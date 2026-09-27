#include "worker_protocol.hpp"

#include <cstring>

namespace openrar::sandbox {

bool pack_scan_result(const ScanView& view, std::vector<core::byte>& payload) {
    // u32 path-space guard (64-bit intermediate; the L17 pattern).
    core::uint64 total_paths = 0;
    for (const WireEntry& e : view.entries) total_paths += e.path_len;
    if (total_paths > 0xFFFFFFFFull) return false;
    if (view.entries.size() > 0xFFFFFFFFull) return false;

    WireScanHeader hdr{};
    hdr.entry_count = static_cast<core::uint32>(view.entries.size());
    hdr.paths_bytes = total_paths;
    payload.clear();
    payload.reserve(sizeof(hdr) + view.entries.size() * sizeof(WireEntry) +
                    static_cast<size_t>(total_paths));
    const core::byte* hb = reinterpret_cast<const core::byte*>(&hdr);
    payload.insert(payload.end(), hb, hb + sizeof(hdr));
    for (const WireEntry& e : view.entries) {
        const core::byte* eb = reinterpret_cast<const core::byte*>(&e);
        payload.insert(payload.end(), eb, eb + sizeof(e));
    }
    payload.insert(payload.end(), view.paths.begin(), view.paths.end());
    return payload.size() <= kMaxFramePayload;
}

bool unpack_scan_result(const core::byte* payload, size_t len, ScanView& out) {
    if (payload == nullptr || len < sizeof(WireScanHeader)) return false;
    WireScanHeader hdr;
    std::memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.reserved != 0) return false;
    const core::uint64 entries_bytes =
        static_cast<core::uint64>(hdr.entry_count) * sizeof(WireEntry);
    // Overflow-safe total: header + entries + paths must equal len exactly.
    if (entries_bytes > len - sizeof(WireScanHeader)) return false;
    if (sizeof(WireScanHeader) + entries_bytes + hdr.paths_bytes != len) return false;
    // Path blob must be addressable through every entry's offset+len (checked
    // per entry below) — this also bounds entry_count × path_len ≤ blob size.

    out.entries.clear();
    out.paths.clear();
    out.entries.reserve(hdr.entry_count);
    const core::byte* eb = payload + sizeof(WireScanHeader);
    for (core::uint32 i = 0; i < hdr.entry_count; ++i) {
        WireEntry e;
        std::memcpy(&e, eb + static_cast<size_t>(i) * sizeof(WireEntry), sizeof(WireEntry));
        if (e._pad[1] != 0) return false;                  // reserved
        if (e._pad[0] & ~kWIRE_FLAG_SERVICE) return false; // unknown wire flags
        const core::uint64 end = static_cast<core::uint64>(e.path_offset) + e.path_len;
        if (end > hdr.paths_bytes) return false;
        out.entries.push_back(e);
    }
    out.paths.assign(
        reinterpret_cast<const char*>(payload + sizeof(WireScanHeader) + entries_bytes),
        static_cast<size_t>(hdr.paths_bytes));
    return true;
}

bool pack_extract_req(core::uint32 index, std::vector<core::byte>& payload) {
    WireExtractReq r{};
    r.entry_index = index;
    payload.assign(reinterpret_cast<const core::byte*>(&r),
                   reinterpret_cast<const core::byte*>(&r) + sizeof(r));
    return true;
}

bool unpack_extract_req(const core::byte* payload, size_t len, core::uint32& index) {
    if (payload == nullptr || len != sizeof(WireExtractReq)) return false;
    WireExtractReq r;
    std::memcpy(&r, payload, sizeof(r));
    if (r.reserved != 0) return false;
    index = r.entry_index;
    return true;
}

bool pack_entry_result(core::uint32 index, core::int32 rc, core::uint64 bytes,
                       std::vector<core::byte>& payload) {
    WireEntryResult r{};
    r.entry_index = index;
    r.rc = rc;
    r.bytes_emitted = bytes;
    payload.assign(reinterpret_cast<const core::byte*>(&r),
                   reinterpret_cast<const core::byte*>(&r) + sizeof(r));
    return true;
}

bool unpack_entry_result(const core::byte* payload, size_t len, WireEntryResult& out) {
    if (payload == nullptr || len != sizeof(WireEntryResult)) return false;
    std::memcpy(&out, payload, sizeof(out));
    return out.reserved == 0 && out.pad == 0;
}

bool pack_error(core::int32 rc, const std::string& detail, std::vector<core::byte>& payload) {
    WireError e{};
    e.rc = rc;
    payload.clear();
    const core::byte* eb = reinterpret_cast<const core::byte*>(&e);
    payload.insert(payload.end(), eb, eb + sizeof(e));
    payload.insert(payload.end(), detail.begin(), detail.end());
    return payload.size() <= kMaxFramePayload;
}

bool unpack_error(const core::byte* payload, size_t len, core::int32& rc, std::string& detail) {
    if (payload == nullptr || len < sizeof(WireError)) return false;
    WireError e;
    std::memcpy(&e, payload, sizeof(e));
    rc = e.rc;
    detail.assign(reinterpret_cast<const char*>(payload + sizeof(WireError)),
                  len - sizeof(WireError));
    return true;
}

bool pack_selftest(const WireSelftest& st, std::vector<core::byte>& payload) {
    payload.assign(reinterpret_cast<const core::byte*>(&st),
                   reinterpret_cast<const core::byte*>(&st) + sizeof(st));
    return true;
}

bool unpack_selftest(const core::byte* payload, size_t len, WireSelftest& out) {
    if (payload == nullptr || len != sizeof(WireSelftest)) return false;
    std::memcpy(&out, payload, sizeof(out));
    return out.pad[0] == 0 && out.pad[1] == 0;
}

} // namespace openrar::sandbox
