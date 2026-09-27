#include "worker_broker.hpp"

#include "../archive/rar_errors.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
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

#if defined(_WIN32)
std::string handle_str(native_io_handle h) {
    return std::to_string(reinterpret_cast<uintptr_t>(h));
}
#else
std::string handle_str(native_io_handle h) {
    return std::to_string(h);
}
#endif

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
                         Limits limits, std::string& detail) {
    shutdown();
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
#if defined(_WIN32)
    const std::string cmdline = "\"" + worker_exe + "\" --openrar-sandbox-worker " +
                                handle_str(pair_.cmd_read) + " " + handle_str(pair_.res_write) +
                                " " + handle_str(volume_);
    std::string cmdline_mut = cmdline; // CreateProcessA may write its argument
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdline_mut.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &si, &pi)) {
        detail = "cannot spawn worker process";
        close_pair();
        CloseHandle(volume_);
        volume_ = kInvalidIoHandle;
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        detail = "cannot fork worker process";
        close_pair();
        ::close(volume_);
        volume_ = kInvalidIoHandle;
        return false;
    }
    if (pid == 0) {
        // Child: the worker side. Never returns.
        const std::string a1 = handle_str(pair_.cmd_read);
        const std::string a2 = handle_str(pair_.res_write);
        const std::string a3 = handle_str(volume_);
        ::execl(worker_exe.c_str(), worker_exe.c_str(), "--openrar-sandbox-worker", a1.c_str(),
                a2.c_str(), a3.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    proc_ = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
#endif

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

} // namespace openrar::sandbox
