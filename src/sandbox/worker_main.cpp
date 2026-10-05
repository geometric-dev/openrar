// ─────────────────────────────────────────────────────────────────────────────
//  openrar_worker — the sandboxed parse/decode worker process (v1.30.0 §5.1).
//
//  Spawned by the broker (CLI) with three native handle numbers on argv:
//
//      openrar_worker [--install-seccomp] --openrar-sandbox-worker
//                     <cmd_read> <res_write> <volume_handle>
//
//  (Windows: inheritable HANDLE values; POSIX: file descriptors. The volume
//  handle is an ALREADY-OPEN read handle — the worker never learns a path,
//  never opens, creates, or writes a file, never executes anything. Its only
//  I/O is the two channel ends and pread on the volume. The per-OS sandbox
//  profiles (AppContainer / seccomp-BPF, M3a/b) enforce exactly that minimal
//  capability set at the OS level; --install-seccomp installs the Linux
//  filter in-child, fail-closed.)
//
//  Proof-of-denial self-test mode (M3a/b, the v1.29 Gate 0 directive):
//
//      openrar_worker [--install-seccomp] --sandbox-selftest
//                     <probe_dir> <cmd_read> <res_write> <volume_handle>
//
//  Attempts the operations the sandbox must deny (file write, loopback
//  network) and one it must allow (reading the inherited volume handle),
//  and reports the OBSERVED outcomes over the result channel.
//
//  Protocol (worker side): Ping handshake → Open → ScanResult (or Error) →
//  per-ExtractReq: DataChunk stream + final EntryResult → Shutdown/EOF exit.
//  Cancel is close-based: the broker closes the channels; the worker's
//  blocked send fails, the sink returns false, extraction aborts, the worker
//  exits — no mid-entry cancel frame in v1.30.0 (documented, plan §1.3).
//
//  Encrypted archives are out of worker scope in v1.30.0: a hostile or
//  legitimate HEAD_CRYPT/encrypted entry fails the scan/extract with the
//  engine's own codes and the broker falls back in-process, loudly. The
//  Gate-0 directive-4 KDF legs (broker-side derivation) land with encrypted-
//  worker support; raw passwords never cross the boundary in any shape.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)
#include <winsock2.h> // MUST precede windows.h (pulled in by channel.hpp)
#include <ws2tcpip.h>
#endif

#include "channel.hpp"
#include "ipc.hpp"
#include "spawn.hpp"
#include "worker_protocol.hpp"

#include "../archive/archive_reader.hpp"
#include "../core/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace openrar;

namespace {

#if defined(_WIN32)
void* parse_native(const char* s) {
    return reinterpret_cast<void*>(_strtoui64(s, nullptr, 10));
}
#else
void* parse_native(const char* s) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(strtol(s, nullptr, 10)));
}
#endif

constexpr size_t kChunkPayload = 256 * 1024; // per DataChunk frame (≤ 4 MiB cap)

// Send or die: any channel failure on the worker side means the broker is
// gone (or the pair is poisoned) — exit quietly, no partial state exists
// here (the worker never owns filesystem state).
bool send_frame(sandbox::FramedChannel& res, const sandbox::Frame& f) {
    return res.send(f);
}

#if defined(_WIN32)
sandbox::ChannelPair make_pair_from(void* cmd_read, void* res_write) {
    sandbox::ChannelPair p;
    p.cmd_read = static_cast<HANDLE>(cmd_read);
    p.res_write = static_cast<HANDLE>(res_write);
    return p;
}
#else
sandbox::ChannelPair make_pair_from(void* cmd_read, void* res_write) {
    sandbox::ChannelPair p;
    p.cmd_read = static_cast<int>(reinterpret_cast<intptr_t>(cmd_read));
    p.res_write = static_cast<int>(reinterpret_cast<intptr_t>(res_write));
    return p;
}
#endif

// Proof-of-denial self-test (M3a/b): attempts the operations the sandbox
// must deny and the one it must allow, and reports the OBSERVED outcomes
// over the result channel. Exit 0 after reporting; the broker reaps.
int run_selftest(const char* probe_dir, const char* cmd_read_s, const char* res_write_s,
                 const char* volume_s) {
    void* cmd_read = parse_native(cmd_read_s);
    void* res_write = parse_native(res_write_s);
    void* volume = parse_native(volume_s);
    if (cmd_read == nullptr || res_write == nullptr || volume == nullptr) return 3;
    sandbox::ChannelPair p = make_pair_from(cmd_read, res_write);
    sandbox::FramedChannel cmd(p.cmd_read);
    sandbox::FramedChannel res(p.res_write);

    sandbox::WireSelftest st{};
    st.write_ok = 1;
    st.net_ok = 1;
    st.volume_read_ok = 1;

    // Probe 1: file write under the probe dir.
    {
        const std::string path = std::string(probe_dir) + "/escape.bin";
#if defined(_WIN32)
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            st.write_ok = 0;
            st.write_err = GetLastError();
        } else {
            CloseHandle(h);
        }
#else
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            st.write_ok = 0;
            st.write_err = static_cast<uint32_t>(errno);
        } else {
            ::close(fd);
        }
#endif
    }

    // Probe 2: loopback network (socket + connect to 127.0.0.1).
    {
#if defined(_WIN32)
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            st.net_ok = 0;
            st.net_err = WSAGetLastError();
        } else {
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == INVALID_SOCKET) {
                st.net_ok = 0;
                st.net_err = WSAGetLastError();
            } else {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(1);
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                    st.net_ok = 0;
                    st.net_err = WSAGetLastError();
                }
                closesocket(s);
            }
            WSACleanup();
        }
#else
        const int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            st.net_ok = 0;
            st.net_err = static_cast<uint32_t>(errno);
        } else {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(1);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                st.net_ok = 0;
                st.net_err = static_cast<uint32_t>(errno);
            }
            ::close(s);
        }
#endif
    }

    // Probe 3: the inherited volume handle must remain usable (read the
    // signature bytes). A sandbox that breaks the broker's grant is a
    // broken sandbox, not a stricter one.
    {
        core::byte sig[8] = {};
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(volume), sig, sizeof(sig), &got, nullptr) ||
            got != sizeof(sig)) {
            st.volume_read_ok = 0;
            st.volume_read_err = GetLastError();
        }
#else
        if (::pread(static_cast<int>(reinterpret_cast<intptr_t>(volume)), sig, sizeof(sig), 0) !=
            static_cast<ssize_t>(sizeof(sig))) {
            st.volume_read_ok = 0;
            st.volume_read_err = static_cast<uint32_t>(errno);
        }
#endif
    }

    // Handshake then report.
    if (!send_frame(res, sandbox::Frame{sandbox::FrameType::Ping, {}})) return 2;
    for (;;) {
        std::vector<sandbox::Frame> in;
        const int rc = cmd.recv(in);
        if (rc <= 0) return 0;
        if (rc == -2) return 2;
        for (const sandbox::Frame& f : in) {
            if (f.type == sandbox::FrameType::SelftestReq) {
                sandbox::Frame out{sandbox::FrameType::SelftestResult, {}};
                if (!sandbox::pack_selftest(st, out.payload) || !send_frame(res, out)) return 2;
                return 0;
            }
            if (f.type == sandbox::FrameType::Shutdown) return 0;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // Sandbox model install (Linux seccomp; M3b). MUST run before any
    // protocol I/O and before the self-test probes. Fail-closed: the
    // installer _exits(4) if the filter cannot be installed — never
    // continues unsandboxed when a sandbox was requested.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--install-seccomp") == 0) {
#if defined(__linux__)
            sandbox::install_linux_seccomp();
#else
            std::fprintf(stderr, "openrar_worker: --install-seccomp on non-Linux\n");
            return 4;
#endif
        }
    }

    // Positional scan: each mode flag is followed by its fixed arguments; the
    // flag order is free and at most one mode runs.
    const char* probe_dir = nullptr;
    const char* selftest_args[3] = {nullptr, nullptr, nullptr};
    const char* worker_args[3] = {nullptr, nullptr, nullptr};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--openrar-sandbox-worker") == 0) {
            for (int k = 0; k < 3 && i + 1 + k < argc; ++k) worker_args[k] = argv[i + 1 + k];
            i += 3;
        } else if (std::strcmp(argv[i], "--sandbox-selftest") == 0) {
            if (i + 4 < argc) {
                probe_dir = argv[i + 1];
                selftest_args[0] = argv[i + 2];
                selftest_args[1] = argv[i + 3];
                selftest_args[2] = argv[i + 4];
                i += 4;
            }
        }
    }
    if (probe_dir != nullptr && selftest_args[0] != nullptr && selftest_args[1] != nullptr &&
        selftest_args[2] != nullptr) {
        return run_selftest(probe_dir, selftest_args[0], selftest_args[1], selftest_args[2]);
    }
    if (worker_args[0] == nullptr || worker_args[1] == nullptr || worker_args[2] == nullptr) {
        std::fprintf(stderr, "usage: openrar_worker [--install-seccomp] --openrar-sandbox-worker "
                             "<cmd_read> <res_write> <volume_handle>\n"
                             "       openrar_worker [--install-seccomp] --sandbox-selftest "
                             "<probe_dir> <cmd_read> <res_write> <volume_handle>\n");
        return 3;
    }
    void* cmd_read = parse_native(worker_args[0]);
    void* res_write = parse_native(worker_args[1]);
    void* volume = parse_native(worker_args[2]);
    if (cmd_read == nullptr || res_write == nullptr || volume == nullptr) return 3;
    sandbox::ChannelPair p = make_pair_from(cmd_read, res_write);
    sandbox::FramedChannel cmd(p.cmd_read);
    sandbox::FramedChannel res(p.res_write);

    // Handshake.
    if (!send_frame(res, sandbox::Frame{sandbox::FrameType::Ping, {}})) return 2;

    archive::ArchiveReader reader;
    bool reader_open = false;

    for (;;) {
        std::vector<sandbox::Frame> in;
        const int rc = cmd.recv(in);
        if (rc <= 0) return 0;  // EOF (clean shutdown or broker death)
        if (rc == -2) return 2; // protocol poison on our own receive side

        for (const sandbox::Frame& f : in) {
            switch (f.type) {
            case sandbox::FrameType::Open: {
                int status = 0;
                std::string detail;
                if (reader.open_read_handle(volume, "", status, detail)) {
                    // Worker mode is single-volume by contract: a set (split
                    // entries in the scan) refuses HERE so the broker can
                    // fall back in-process loudly — never a partial decode.
                    for (const auto& e : reader.entries()) {
                        if (e.split_before || e.split_after) {
                            sandbox::Frame err{sandbox::FrameType::Error, {}};
                            sandbox::pack_error(-13,
                                                "multi-volume set: worker mode is "
                                                "single-volume",
                                                err.payload);
                            if (!send_frame(res, err)) return 2;
                            return 0;
                        }
                    }
                    reader_open = true;
                    sandbox::ScanView view;
                    view.entries.reserve(reader.entries().size());
                    std::string paths;
                    for (const archive::ArchiveEntry& e : reader.entries()) {
                        sandbox::WireEntry w{};
                        const format::FileBlock& fb = e.header;
                        const std::string& name = fb.file_name;
                        w.path_offset = static_cast<uint32_t>(paths.size());
                        w.path_len = static_cast<uint32_t>(name.size());
                        w.is_dir = (fb.file_flags & format::FHFL_DIRECTORY) ? 1u : 0u;
                        w.method = fb.method;
                        w.is_encrypted = fb.is_encrypted ? 1u : 0u;
                        w.crc32 = fb.has_crc32 ? fb.data_crc32 : 0u;
                        w.size = fb.unp_size;
                        w.packed_size = e.data_size;
                        // FILETIME (100 ns since 1601) to UNIX seconds, the
                        // ABI mtime convention.
                        w.mtime = fb.mtime_win >= 116444736000000000ull
                                      ? (fb.mtime_win - 116444736000000000ull) / 10000000ull
                                      : 0ull;
                        w._pad[0] = fb.is_service ? sandbox::kWIRE_FLAG_SERVICE : 0ull;
                        view.entries.push_back(w);
                        paths += name;
                    }
                    view.paths = std::move(paths);
                    sandbox::Frame out{sandbox::FrameType::ScanResult, {}};
                    if (!sandbox::pack_scan_result(view, out.payload) || !send_frame(res, out)) {
                        return 2;
                    }
                } else {
                    sandbox::Frame out{sandbox::FrameType::Error, {}};
                    if (!sandbox::pack_error(status, detail, out.payload) ||
                        !send_frame(res, out)) {
                        return 2;
                    }
                }
                break;
            }
            case sandbox::FrameType::ExtractReq: {
                uint32_t index = 0;
                if (!reader_open ||
                    !sandbox::unpack_extract_req(f.payload.data(), f.payload.size(), index) ||
                    index >= reader.entries().size()) {
                    sandbox::Frame out{sandbox::FrameType::Error, {}};
                    sandbox::pack_error(-9, "invalid extract request", out.payload);
                    if (!send_frame(res, out)) return 2;
                    break;
                }
                uint64_t emitted = 0;
                auto sink = [&](const core::byte* data, size_t n) -> bool {
                    if (std::getenv("OPENRAR_PD_DEBUG"))
                        std::fprintf(stderr, "[pd] sink n=%zu emitted=%llu\n", n,
                                     (unsigned long long)emitted);
                    if (n > sandbox::kMaxFramePayload) return false;
                    sandbox::Frame chunk{sandbox::FrameType::DataChunk, {}};
                    chunk.payload.assign(data, data + n);
                    if (!send_frame(res, chunk)) return false;
                    emitted += n;
                    return true;
                };
                const int erc = reader.extract_entry_sink(index, sink);
                sandbox::Frame out{sandbox::FrameType::EntryResult, {}};
                sandbox::pack_entry_result(index, erc, emitted, out.payload);
                if (!send_frame(res, out)) return 2;
                break;
            }
            case sandbox::FrameType::Shutdown:
                return 0;
            default:
                // Open/ExtractReq handled above; anything else from the broker
                // is a protocol violation.
                return 2;
            }
        }
    }
}
