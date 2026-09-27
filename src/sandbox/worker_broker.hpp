#ifndef OPENRAR_SANDBOX_WORKER_BROKER_HPP
#define OPENRAR_SANDBOX_WORKER_BROKER_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/worker_broker.hpp — the broker side of the sandboxed worker
//  (v1.30.0 §5.1). The broker OWNS:
//    - the volume handle (it opens the archive; the worker gets the handle,
//      never the path),
//    - ALL policy: extraction caps are enforced HERE on bytes received from
//      the worker (the worker is untrusted; its DataChunks never reach the
//      sink past a cap),
//    - the spawn (this file spawns the worker UNSANDBOXED; the per-OS models
//      wrap this same spawn with the AppContainer/seccomp/Seatbelt profile —
//      the protocol and policy are identical either way).
//
//  v1.30.0 worker scope (documented degradations, each loud at the CLI):
//    - single-volume archives (a volume set reports MISSING_VOLUME from the
//      worker scan → broker falls back in-process);
//    - unencrypted archives (encrypted entries fail with the engine's own
//      codes → same fallback; raw passwords never cross the boundary).
// ─────────────────────────────────────────────────────────────────────────────

#include "spawn.hpp"
#include "channel.hpp"
#include "worker_protocol.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openrar::sandbox {

// The broker's view of a scanned entry (WireEntry by value; paths blob held
// once by the broker).
struct BrokerEntry {
    WireEntry wire{};
    std::string path; // resolved from the paths blob at unpack time
    bool is_service() const { return (wire._pad[0] & kWIRE_FLAG_SERVICE) != 0; }
};

class WorkerBroker {
public:
    struct Limits {
        core::uint64 max_member_bytes = ~0ull; // per-entry decode cap (broker-enforced)
        core::uint64 max_total_bytes = ~0ull;  // cumulative across entries, never reset
    };

    ~WorkerBroker() { shutdown(); }

    // Spawns the worker (unsandboxed spawn; models wrap this), opens the
    // volume read-only, performs the Ping/Open/ScanResult handshake.
    // `arc_path` stays in the broker process. On failure the object returns
    // to the closed state and `detail` carries a human message.
    bool start(const std::string& worker_exe, const std::filesystem::path& arc_path, Limits limits,
               std::string& detail, SpawnProfile profile = SpawnProfile::Unsandboxed);

    // Proof-of-denial self-test (v1.29 Gate 0 directive): spawns the worker
    // with `profile` against `probe_dir` (a scratch dir the CALLER created —
    // no AppContainer ACE → the write probe must be denied), sends
    // SelftestReq, and returns the OBSERVED outcomes in `out`. Returns false
    // (with `detail`) on transport failure — a sandbox model that breaks the
    // channel/volume grant is a broken sandbox, reported as such.
    static bool run_selftest(const std::string& worker_exe, const std::filesystem::path& probe_dir,
                             const std::filesystem::path& volume_path, SpawnProfile profile,
                             WireSelftest& out, std::string& detail);

    // Sends Shutdown, waits briefly, closes everything. Safe to call twice.
    void shutdown();

    // Hard kill (crash-isolation paths; also the watchdog's action).
    void kill();

    bool is_open() const { return phase_ == Phase::Ready; }

    const std::vector<BrokerEntry>& entries() const { return entries_; }

    // Streams entry `index`'s decoded bytes into `sink`. Caps are enforced
    // HERE: a chunk that would cross a cap is NOT delivered; the operation
    // aborts with RAR_ERR_LIMIT_EXCEEDED and the worker pair is torn down
    // (a worker streaming past policy is dead policy-wise). Returns the
    // engine rc relayed in EntryResult, or the broker-side failure code.
    int extract(core::uint32 index, const std::function<bool(const core::byte*, size_t)>& sink,
                core::uint64* bytes_out, std::string& detail);

private:
    enum class Phase { Closed, Ready };

    bool read_frames(std::vector<Frame>& out, std::string& detail); // EOF/error-aware
    void close_pair();

    ChannelPair pair_{};
    void* proc_ = nullptr; // platform process handle (HANDLE / pid as void*)
    native_io_handle volume_ = kInvalidIoHandle;
    std::unique_ptr<FramedChannel> cmd_; // broker → worker
    std::unique_ptr<FramedChannel> res_; // worker → broker
    Phase phase_ = Phase::Closed;
    SpawnProfile profile_ = SpawnProfile::Unsandboxed;
    Limits limits_{};
    core::uint64 total_emitted_ = 0;
    std::vector<BrokerEntry> entries_;
};

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_WORKER_BROKER_HPP
