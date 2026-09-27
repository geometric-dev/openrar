#include "worker_broker.hpp"
#include "spawn.hpp"

#include "../archive/rar_errors.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
// windows.h arrives via channel.hpp (global scope, before anything else)
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace openrar::sandbox {

// The canonical RarError codes live in openrar::archive (the shared ABI
// contract); the broker relays them verbatim — no new codes (§3.1).
using namespace openrar::archive;

namespace {

std::string handle_str(native_io_handle h) {
#if defined(_WIN32)
    return std::to_string(reinterpret_cast<uintptr_t>(h));
#else
    return std::to_string(h);
#endif
}

} // namespace

void WorkerBroker::close_pair() {
    close_channel_pair(pair_);
    cmd_.reset();
    res_.reset();
}

void WorkerBroker::kill() {
    // Terminate + reap ONLY. The channel handles are NOT closed here: the
    // broker's own thread may be blocked in ReadFile on the results channel,
    // and closing a handle under a blocked reader is undefined behavior.
    // When the worker dies, its pipe ends are destroyed by the kernel, the
    // blocked read returns broken-pipe/EOF, and the reader thread observes
    // the death cleanly. The handles close later in shutdown().
    if (proc_ != nullptr) {
#if defined(_WIN32)
        TerminateProcess(static_cast<HANDLE>(proc_), 1);
        WaitForSingleObject(static_cast<HANDLE>(proc_), 2000);
        CloseHandle(static_cast<HANDLE>(proc_));
#else
        const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc_));
        ::kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);
#endif
        proc_ = nullptr;
    }
    phase_ = Phase::Closed;
}

void WorkerBroker::shutdown() {
    if (proc_ != nullptr) {
        // Best-effort clean exit: the worker answers Shutdown by returning
        // from main. Close-based cancellation remains the hard path.
        if (cmd_ != nullptr && phase_ == Phase::Ready) {
            Frame f{FrameType::Shutdown, {}};
            cmd_->send(f);
        }
#if defined(_WIN32)
        WaitForSingleObject(static_cast<HANDLE>(proc_), 2000);
        CloseHandle(static_cast<HANDLE>(proc_));
#else
        const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc_));
        for (int i = 0; i < 20; ++i) {
            if (waitpid(pid, nullptr, WNOHANG) != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#endif
        proc_ = nullptr;
    }
    close_pair();
    volume_ = kInvalidIoHandle;
    phase_ = Phase::Closed;
}

bool WorkerBroker::read_frames(std::vector<Frame>& out, std::string& detail) {
    const int r = res_->recv(out);
    if (r == 0) {
        detail = "worker closed the channel unexpectedly";
        return false;
    }
    if (r == -1) {
        detail = "worker channel I/O error";
        return false;
    }
    if (r == -2) {
        detail = "protocol violation from worker (poisoned stream)";
        return false;
    }
    return true;
}

bool WorkerBroker::start(const std::string& worker_exe, const std::filesystem::path& arc_path,
                         Limits limits, std::string& detail, SpawnProfile profile) {
    shutdown();
    profile_ = profile;
    limits_ = limits;
    total_emitted_ = 0;
    entries_.clear();

    // 1. Channels (inheritable on Windows: the worker ends survive spawn).
    if (!create_channel_pair(pair_)) {
        detail = "cannot create broker/worker channels";
        return false;
    }

    // 2. Volume handle — the BROKER opens the archive; the worker receives
    // only the read handle (§5.1: the worker never learns a path).
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    volume_ = CreateFileW(arc_path.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (volume_ == kInvalidIoHandle) {
        detail = "cannot open archive volume for the worker";
        close_pair();
        return false;
    }
#else
    volume_ = ::open(arc_path.c_str(), O_RDONLY);
    if (volume_ == kInvalidIoHandle) {
        detail = "cannot open archive volume for the worker";
        close_pair();
        return false;
    }
#endif

    // 3. Spawn. The unsandboxed spawn is the seam the per-OS models wrap:
    // same argv contract, same handles, plus the platform privilege profile.
    std::vector<std::string> argv_tail;
#if defined(__linux__)
    if (profile_ == SpawnProfile::PlatformSandboxed)
        argv_tail.push_back("--install-seccomp"); // in-child filter install
#endif
    argv_tail.push_back("--openrar-sandbox-worker");
    argv_tail.push_back(handle_str(pair_.cmd_read));
    argv_tail.push_back(handle_str(pair_.res_write));
    argv_tail.push_back(handle_str(volume_));
    if (!spawn_worker_process(worker_exe, argv_tail, profile_, &proc_, detail)) {
        close_pair();
        close_io_handle(volume_);
        volume_ = kInvalidIoHandle;
        return false;
    }

    // 4. Parent drops its copies of the worker-side ends (exact EOF semantics).
    close_io_handle(pair_.cmd_read);
    close_io_handle(pair_.res_write);
    // The worker holds its own inherited copy of the volume handle now.
    close_io_handle(volume_);

    cmd_ = std::make_unique<FramedChannel>(pair_.cmd_write);
    res_ = std::make_unique<FramedChannel>(pair_.res_read);

    // 5. Handshake: Ping → Open → ScanResult | Error.
    {
        std::vector<Frame> in;
        if (!read_frames(in, detail)) {
            kill();
            return false;
        }
        bool pinged = false;
        for (const Frame& f : in)
            if (f.type == FrameType::Ping) pinged = true;
        if (!pinged) {
            detail = "worker handshake failed (no Ping)";
            kill();
            return false;
        }
    }
    {
        Frame open{FrameType::Open, {}};
        if (!cmd_->send(open)) {
            detail = "worker channel died at Open";
            kill();
            return false;
        }
    }
    {
        std::vector<Frame> in;
        if (!read_frames(in, detail)) {
            kill();
            return false;
        }
        for (const Frame& f : in) {
            if (f.type == FrameType::ScanResult) {
                ScanView view;
                if (!unpack_scan_result(f.payload.data(), f.payload.size(), view)) {
                    detail = "malformed ScanResult from worker";
                    kill();
                    return false;
                }
                entries_.reserve(view.entries.size());
                for (const WireEntry& w : view.entries) {
                    BrokerEntry be;
                    be.wire = w;
                    if (w.path_offset + w.path_len > view.paths.size()) {
                        detail = "ScanResult path out of range";
                        kill();
                        return false;
                    }
                    be.path = view.paths.substr(w.path_offset, w.path_len);
                    entries_.push_back(std::move(be));
                }
                phase_ = Phase::Ready;
                return true;
            }
            if (f.type == FrameType::Error) {
                core::int32 rc = 0;
                unpack_error(f.payload.data(), f.payload.size(), rc, detail);
                detail = "worker open failed: " + detail;
                kill();
                return false;
            }
        }
        detail = "worker handshake failed (no ScanResult)";
        kill();
        return false;
    }
}

int WorkerBroker::extract(core::uint32 index,
                          const std::function<bool(const core::byte*, size_t)>& sink,
                          core::uint64* bytes_out, std::string& detail) {
    if (phase_ != Phase::Ready) {
        detail = "worker is not running";
        return RAR_ERR_INVALID_ARG;
    }
    if (index >= entries_.size()) {
        detail = "entry index out of range";
        return RAR_ERR_INVALID_ARG;
    }
    Frame req{FrameType::ExtractReq, {}};
    if (!pack_extract_req(index, req.payload) || !cmd_->send(req)) {
        detail = "worker channel died at ExtractReq";
        kill();
        return RAR_ERR_IO;
    }

    core::uint64 member_bytes = 0;
    int rc = RAR_ERR_IO;
    bool done = false;
    while (!done) {
        std::vector<Frame> in;
        if (!read_frames(in, detail)) {
            kill();
            return RAR_ERR_IO; // EOF mid-extract = worker death (crash isolation)
        }
        for (const Frame& f : in) {
            if (done) break;
            switch (f.type) {
            case FrameType::DataChunk: {
                // Broker-side cap enforcement BEFORE delivery: a worker
                // streaming past policy never reaches the sink (plan §1.3).
                if (member_bytes + f.payload.size() > limits_.max_member_bytes ||
                    total_emitted_ + f.payload.size() > limits_.max_total_bytes) {
                    detail = "broker cap exceeded";
                    kill();
                    return RAR_ERR_LIMIT_EXCEEDED;
                }
                if (!sink(f.payload.data(), f.payload.size())) {
                    detail = "sink refused the stream";
                    kill();
                    return RAR_ERR_ABORTED;
                }
                member_bytes += f.payload.size();
                total_emitted_ += f.payload.size();
                break;
            }
            case FrameType::EntryResult: {
                WireEntryResult er{};
                if (!unpack_entry_result(f.payload.data(), f.payload.size(), er) ||
                    er.entry_index != index) {
                    detail = "malformed EntryResult from worker";
                    kill();
                    return RAR_ERR_IO;
                }
                rc = er.rc;
                if (bytes_out != nullptr) *bytes_out = er.bytes_emitted;
                done = true;
                break;
            }
            case FrameType::Error: {
                core::int32 code = 0;
                unpack_error(f.payload.data(), f.payload.size(), code, detail);
                rc = code;
                done = true;
                break;
            }
            default:
                detail = "unexpected frame from worker during extract";
                kill();
                return RAR_ERR_IO;
            }
        }
    }
    return rc;
}

bool WorkerBroker::run_selftest(const std::string& worker_exe,
                                const std::filesystem::path& probe_dir,
                                const std::filesystem::path& volume_path, SpawnProfile profile,
                                WireSelftest& out, std::string& detail) {
    // The volume handle gives the worker's third probe something real to
    // read (the sandbox must keep the broker's grant usable); the broker
    // opens it, exactly like the real flow.
    WorkerBroker broker;
    broker.profile_ = profile;
    if (!create_channel_pair(broker.pair_)) {
        detail = "cannot create selftest channels";
        return false;
    }
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    broker.volume_ = CreateFileW(volume_path.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (broker.volume_ == kInvalidIoHandle) {
        detail = "cannot open selftest volume handle";
        broker.close_pair();
        return false;
    }
#else
    broker.volume_ = ::open(volume_path.c_str(), O_RDONLY);
    if (broker.volume_ == kInvalidIoHandle) {
        detail = "cannot open selftest volume handle";
        broker.close_pair();
        return false;
    }
#endif
    std::vector<std::string> argv_tail;
#if defined(__linux__)
    if (profile == SpawnProfile::PlatformSandboxed) argv_tail.push_back("--install-seccomp");
#endif
    argv_tail.push_back("--sandbox-selftest");
    argv_tail.push_back(probe_dir.string());
    argv_tail.push_back(handle_str(broker.pair_.cmd_read));
    argv_tail.push_back(handle_str(broker.pair_.res_write));
    argv_tail.push_back(handle_str(broker.volume_));
    if (!spawn_worker_process(worker_exe, argv_tail, profile, &broker.proc_, detail)) {
        broker.close_pair();
        return false;
    }
    close_io_handle(broker.pair_.cmd_read);
    close_io_handle(broker.pair_.res_write);
    close_io_handle(broker.volume_);
    broker.cmd_ = std::make_unique<FramedChannel>(broker.pair_.cmd_write);
    broker.res_ = std::make_unique<FramedChannel>(broker.pair_.res_read);

    // Handshake: Ping.
    {
        std::vector<Frame> in;
        if (!broker.read_frames(in, detail)) {
            broker.kill();
            return false;
        }
        bool pinged = false;
        for (const Frame& f : in)
            if (f.type == FrameType::Ping) pinged = true;
        if (!pinged) {
            detail = "selftest handshake failed (no Ping)";
            broker.kill();
            return false;
        }
    }
    // Ask for the probes.
    {
        Frame req{FrameType::SelftestReq, {}};
        if (!broker.cmd_->send(req)) {
            detail = "selftest channel died at SelftestReq";
            broker.kill();
            return false;
        }
    }
    {
        std::vector<Frame> in;
        if (!broker.read_frames(in, detail)) {
            broker.kill();
            return false;
        }
        for (const Frame& f : in) {
            if (f.type == FrameType::SelftestResult) {
                if (!unpack_selftest(f.payload.data(), f.payload.size(), out)) {
                    detail = "malformed SelftestResult";
                    broker.kill();
                    return false;
                }
                broker.kill(); // the worker exits after reporting; reap + close
                return true;
            }
        }
        detail = "selftest failed (no SelftestResult)";
        broker.kill();
        return false;
    }
}

} // namespace openrar::sandbox
