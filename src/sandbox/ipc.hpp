#ifndef OPENRAR_SANDBOX_IPC_HPP
#define OPENRAR_SANDBOX_IPC_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/ipc.hpp — the broker↔worker wire protocol (v1.30.0, §5.1).
//
//  The frame format is deliberately trivial and strictly bounded:
//
//      u32le payload_len | u8 type | payload[payload_len]
//
//  SECURITY POSTURE (Gate 0 directives 4/8, plan §1.3): the worker is
//  UNTRUSTED. The broker validates every frame with the same hostile-input
//  standard as the archive parser itself:
//    - payload_len > kMaxFramePayload  → the stream is POISONED (fail-closed:
//      no resync, ever — a length lie means the byte stream's framing can no
//      longer be trusted);
//    - type outside the registry       → POISONED;
//    - a truncated tail                → incomplete, NOT poisoned (the next
//      feed() may complete it);
//    - no dynamic allocation grows past kMaxFramePayload per frame — a
//      hostile length claim cannot reserve memory before the bytes arrive.
//
//  Codes carried in ERROR frames are the shared RarError values
//  (src/api/abi_contract.hpp) — the exit taxonomy gains no new namespace
//  (SECURITY_ARCHITECTURE §3.1).
// ─────────────────────────────────────────────────────────────────────────────

#include "../core/types.hpp"

#include <cstdint>
#include <vector>

namespace openrar::sandbox {

// Frame registry. Wire values are frozen with the v1.30.0 protocol; new
// frames append (never renumber). A broker/worker pair built from the same
// tree always agrees; a version skew surfaces as a POISONED stream (unknown
// type) — loud, never a silent misparse.
enum class FrameType : core::uint8 {
    Ping = 1,        // liveness (worker init-ready handshake)
    Open = 2,        // broker→worker: volume count confirmed, begin scan
    KdfParams = 3,   // worker→broker: HEAD_CRYPT params (salt, lg2, PswCheck)
    Key = 4,         // broker→worker: derived key material (NEVER the password)
    ScanResult = 5,  // worker→broker: packed entries (ArchiveEntryOut layout)
    ExtractReq = 6,  // broker→worker: entry index to decode
    DataChunk = 7,   // worker→broker: decoded bytes (≤ kMaxFramePayload)
    EntryResult = 8, // worker→broker: per-entry status/metadata
    Error = 9,       // worker→broker: RarError code + UTF-8 detail
    Cancel = 10,     // broker→worker: abort current entry (cooperative)
    Shutdown = 11,   // broker→worker: clean exit
    // v1.30.0 M3a/b: the sandbox proof-of-denial self-test (the v1.29 Gate 0
    // directive requires an OBSERVED denial per shipped sandbox model).
    SelftestReq = 12,    // broker→worker: attempt the denial probes
    SelftestResult = 13, // worker→broker: probe outcomes (worker_protocol.hpp)
};

inline constexpr core::uint32 kMaxFramePayload = 4u * 1024 * 1024; // 4 MiB
inline constexpr size_t kFrameHeaderSize = 5;                      // u32le + u8

struct Frame {
    FrameType type = FrameType::Ping;
    std::vector<core::byte> payload;
};

// Wire size: header + payload. Frames larger than kMaxFramePayload are
// rejected by encode() — a caller-side bug must not become a wire lie.
inline size_t frame_wire_size(const Frame& f) {
    return kFrameHeaderSize + f.payload.size();
}

// Appends the wire bytes. Returns false (and writes nothing) when the frame
// exceeds kMaxFramePayload or the type is unregistered.
bool encode_frame(const Frame& f, std::vector<core::byte>& out);

enum class FeedResult {
    Ok,         // zero or more complete frames decoded
    Incomplete, // need more bytes (never fatal)
    Poisoned,   // protocol violation — the stream is dead; no resync
};

// Incremental frame decoder. Feed raw channel bytes; complete frames come
// back. Once poisoned, every later call returns Poisoned and the transport
// must be torn down (both directions) — the only sane reaction to a framing
// violation on a privilege boundary.
class FrameReader {
public:
    // Consumes as many complete frames as the buffer holds. `offset` is
    // advanced past the bytes consumed into frames (bytes may be retained
    // for an incomplete tail — do not reuse them).
    FeedResult feed(const core::byte* data, size_t len, std::vector<Frame>& out);

    // Bytes currently buffered toward an incomplete frame (≤ kFrameHeaderSize
    // + kMaxFramePayload by construction). Testability hook for the bounded-
    // memory guarantee: a hostile length claim must not grow this.
    size_t buffered() const { return pending_.size(); }

    bool poisoned() const { return poisoned_; }

private:
    std::vector<core::byte> pending_;
    bool poisoned_ = false;
    bool header_done_ = false;
    core::uint32 frame_len_ = 0;
    FrameType frame_type_ = FrameType::Ping;
};

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_IPC_HPP
