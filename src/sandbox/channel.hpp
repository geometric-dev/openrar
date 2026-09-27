#ifndef OPENRAR_SANDBOX_CHANNEL_HPP
#define OPENRAR_SANDBOX_CHANNEL_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/channel.hpp — two one-way framed byte channels (v1.30.0).
//
//  The broker/worker pair speaks over TWO independent one-way channels
//  (Gate 0 directive 2: shaped as two one-way pipes internally so POSIX
//  socketpairs and Windows anonymous pipes are both natural):
//
//      commands:  broker → worker     results:  worker → broker
//
//  Blocking I/O with close-based cancellation: the broker cancels by closing
//  the command channel; the worker observes EOF and exits; a worker death is
//  observed as EOF on the results channel. Deadlines are the broker's job
//  (watchdog closing the handles) — the channel itself stays minimal.
//
//  No archive-layer, crypto-layer, or CLI-layer dependencies: this file sits
//  beside cli at layer rank 5 (tools/layer_check.py) and includes core +
//  OS headers only.
// ─────────────────────────────────────────────────────────────────────────────

#include "ipc.hpp"

#include <vector>

#if defined(_WIN32)
#include <windows.h> // GLOBAL scope only (msvc-arm64 trap; ARCHITECTURE.md §3)
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace openrar::sandbox {

#if defined(_WIN32)
using native_io_handle = HANDLE; // anonymous-pipe read/write ends (inheritable)
// INVALID_HANDLE_VALUE is a reinterpret cast, not a constant expression on
// MSVC — inline const, not constexpr.
inline const native_io_handle kInvalidIoHandle = INVALID_HANDLE_VALUE;
#else
using native_io_handle = int; // pipe fd (-1 = none)
inline constexpr native_io_handle kInvalidIoHandle = -1;
#endif

// One process pair as viewed by the OWNER of all four ends (the spawn site).
// The broker uses {cmd_write, res_read}; the worker uses {cmd_read, res_write}
// (inherited/duplicated at spawn; the broker closes its copies of the
// worker-side ends immediately after spawn so EOF semantics stay exact).
struct ChannelPair {
    native_io_handle cmd_read = kInvalidIoHandle; // broker → worker
    native_io_handle cmd_write = kInvalidIoHandle;
    native_io_handle res_read = kInvalidIoHandle; // worker → broker
    native_io_handle res_write = kInvalidIoHandle;
};

// Creates both pipes (Windows: anonymous inheritable pipes; POSIX: pipe(2)).
// Returns false on any failure with all handles closed.
bool create_channel_pair(ChannelPair& out);

// Raw blocking byte I/O over one handle. read_some returns 0 on EOF (peer
// closed / broken pipe), (size_t)-1 on error. write_all returns false on
// error/EOF.
size_t read_some(native_io_handle h, core::byte* buf, size_t max);
bool write_all(native_io_handle h, const core::byte* buf, size_t len);
void close_io_handle(native_io_handle& h);

// Closes all four handles (safe on already-invalid ones).
void close_channel_pair(ChannelPair& p);

// Framed channel over one direction. One instance per handle. The decoder is
// FrameReader — the bounded-memory, poison-on-violation semantics of ipc.hpp
// apply verbatim; a poisoned stream is permanent and the pair must be torn
// down.
class FramedChannel {
public:
    explicit FramedChannel(native_io_handle h) : h_(h) {}

    // Encodes + writes one frame (write_all semantics: partial writes retried
    // internally; a failure leaves teardown to the caller).
    bool send(const Frame& f);

    // Reads bytes until at least one complete frame is available (blocking).
    // Returns: 1 = frames decoded, 0 = EOF (peer closed), -1 = I/O error,
    // -2 = protocol violation (stream poisoned — tear the pair down).
    int recv(std::vector<Frame>& out);

    bool poisoned() const { return reader_.poisoned(); }

private:
    native_io_handle h_;
    std::vector<core::byte> chunk_; // read staging buffer, reused across recv calls
    FrameReader reader_;
};

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_CHANNEL_HPP
