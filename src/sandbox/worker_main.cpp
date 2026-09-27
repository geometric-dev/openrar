// ─────────────────────────────────────────────────────────────────────────────
//  openrar_worker — the sandboxed parse/decode worker process (v1.30.0 §5.1).
//
//  Spawned by the broker (CLI) with three native handle numbers on argv:
//
//      openrar_worker --openrar-sandbox-worker <cmd_read> <res_write> <vol>
//
//  (Windows: inheritable HANDLE values; POSIX: file descriptors. The volume
//  handle is an ALREADY-OPEN read handle — the worker never learns a path,
//  never opens, creates, or writes a file, never executes anything. Its only
//  I/O is the two channel ends and pread on the volume. The per-OS sandbox
//  profiles (AppContainer / seccomp-BPF / Seatbelt, M3a/b/c) enforce exactly
//  that minimal capability set at the OS level.)
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

#include "channel.hpp"
#include "ipc.hpp"
#include "worker_protocol.hpp"

#include "../archive/archive_reader.hpp"
#include "../core/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 5 || std::strcmp(argv[1], "--openrar-sandbox-worker") != 0) {
        std::fprintf(stderr, "usage: openrar_worker --openrar-sandbox-worker "
                             "<cmd_read> <res_write> <volume_handle>\n");
        return 3;
    }
    void* cmd_read = parse_native(argv[2]);
    void* res_write = parse_native(argv[3]);
    void* volume = parse_native(argv[4]);
    if (cmd_read == nullptr || res_write == nullptr || volume == nullptr) return 3;

#if defined(_WIN32)
    sandbox::ChannelPair p;
    p.cmd_read = static_cast<HANDLE>(cmd_read);
    p.res_write = static_cast<HANDLE>(res_write);
#else
    sandbox::ChannelPair p;
    p.cmd_read = static_cast<int>(reinterpret_cast<intptr_t>(cmd_read));
    p.res_write = static_cast<int>(reinterpret_cast<intptr_t>(res_write));
#endif
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
                    reader_open = true;
                    sandbox::ScanView view;
                    view.entries.reserve(reader.entries().size());
                    std::string paths;
                    for (const archive::ArchiveEntry& e : reader.entries()) {
                        sandbox::WireEntry w{};
                        const format::FileBlock& fb = e.header;
                        const std::string& name = fb.file_name;
                        w.path_offset = static_cast<core::uint32>(paths.size());
                        w.path_len = static_cast<core::uint32>(name.size());
                        w.is_dir = (fb.file_flags & format::FHFL_DIRECTORY) ? 1u : 0u;
                        w.method = fb.method;
                        w.is_encrypted = fb.is_encrypted ? 1u : 0u;
                        w.crc32 = fb.has_crc32 ? fb.data_crc32 : 0u;
                        w.size = fb.unp_size;
                        w.packed_size = e.data_size;
                        // FILETIME (100 ns since 1601) → UNIX seconds, the
                        // ABI's mtime convention.
                        w.mtime = fb.mtime_win >= 116444736000000000ull
                                      ? (fb.mtime_win - 116444736000000000ull) / 10000000ull
                                      : 0ull;
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
                core::uint32 index = 0;
                if (!reader_open ||
                    !sandbox::unpack_extract_req(f.payload.data(), f.payload.size(), index) ||
                    index >= reader.entries().size()) {
                    sandbox::Frame out{sandbox::FrameType::Error, {}};
                    sandbox::pack_error(-9, "invalid extract request", out.payload);
                    if (!send_frame(res, out)) return 2;
                    break;
                }
                core::uint64 emitted = 0;
                auto sink = [&](const core::byte* data, size_t n) -> bool {
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
                // Open/ExtractReq/KdfParams/Key handled above; anything else
                // from the broker is a protocol violation.
                return 2;
            }
        }
    }
}
