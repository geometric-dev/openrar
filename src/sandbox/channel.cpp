#include "channel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace openrar::sandbox {

namespace {
constexpr size_t kReadChunk = 256 * 1024; // recv staging granularity (≤ 4 MiB frames)
} // namespace

#if defined(_WIN32)

bool create_channel_pair(ChannelPair& p) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // worker ends must survive the spawn
    HANDLE cmd_r = nullptr, cmd_w = nullptr, res_r = nullptr, res_w = nullptr;
    if (!CreatePipe(&cmd_r, &cmd_w, &sa, 0)) goto fail;
    if (!CreatePipe(&res_r, &res_w, &sa, 0)) {
        CloseHandle(cmd_r);
        CloseHandle(cmd_w);
        goto fail;
    }
    p.cmd_read = cmd_r;
    p.cmd_write = cmd_w;
    p.res_read = res_r;
    p.res_write = res_w;
    return true;
fail:
    p.cmd_read = p.cmd_write = p.res_read = p.res_write = kInvalidIoHandle;
    return false;
}

size_t read_some(native_io_handle h, core::byte* buf, size_t max) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>((std::min)(max, size_t{0x7FFFFFF0}));
    if (!ReadFile(h, buf, want, &got, nullptr)) {
        // ERROR_BROKEN_PIPE = the worker's write end closed: that is EOF,
        // not an error.
        return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : static_cast<size_t>(-1);
    }
    return got; // 0 only on a zero-byte read of a closed pipe (EOF)
}

bool write_all(native_io_handle h, const core::byte* buf, size_t len) {
    while (len > 0) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>((std::min)(len, size_t{0x7FFFFFF0}));
        if (!WriteFile(h, buf, want, &wrote, nullptr)) return false;
        if (wrote == 0) return false;
        buf += wrote;
        len -= wrote;
    }
    return true;
}

void close_io_handle(native_io_handle& h) {
    if (h != kInvalidIoHandle) {
        CloseHandle(h);
        h = kInvalidIoHandle;
    }
}

#else // POSIX

bool create_channel_pair(ChannelPair& p) {
    int cmd[2] = {-1, -1};
    int res[2] = {-1, -1};
    if (::pipe(cmd) != 0) goto fail;
    if (::pipe(res) != 0) {
        ::close(cmd[0]);
        ::close(cmd[1]);
        goto fail;
    }
    p.cmd_read = cmd[0];
    p.cmd_write = cmd[1];
    p.res_read = res[0];
    p.res_write = res[1];
    return true;
fail:
    p.cmd_read = p.cmd_write = p.res_read = p.res_write = kInvalidIoHandle;
    return false;
}

size_t read_some(native_io_handle h, core::byte* buf, size_t max) {
    for (;;) {
        const ssize_t got = ::read(h, buf, max);
        if (got >= 0) return static_cast<size_t>(got);
        if (errno == EINTR) continue;
        return static_cast<size_t>(-1);
    }
}

bool write_all(native_io_handle h, const core::byte* buf, size_t len) {
    while (len > 0) {
        const ssize_t wrote = ::write(h, buf, len);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (wrote == 0) return false;
        buf += wrote;
        len -= static_cast<size_t>(wrote);
    }
    return true;
}

void close_io_handle(native_io_handle& h) {
    if (h != kInvalidIoHandle) {
        ::close(h);
        h = kInvalidIoHandle;
    }
}

#endif

void close_channel_pair(ChannelPair& p) {
    close_io_handle(p.cmd_read);
    close_io_handle(p.cmd_write);
    close_io_handle(p.res_read);
    close_io_handle(p.res_write);
}

bool FramedChannel::send(const Frame& f) {
    std::vector<core::byte> wire;
    if (!encode_frame(f, wire)) return false;
    return write_all(h_, wire.data(), wire.size());
}

int FramedChannel::recv(std::vector<Frame>& out) {
    if (reader_.poisoned()) return -2;
    if (chunk_.size() < kReadChunk) chunk_.resize(kReadChunk);
    for (;;) {
        const size_t got = read_some(h_, chunk_.data(), chunk_.size());
        if (got == static_cast<size_t>(-1)) return -1;
        if (got == 0) {
            // EOF: whatever is buffered stays buffered — a clean peer closes
            // only after a complete frame (or mid-handshake death, which the
            // broker reports as an aborted/failed operation, not a partial
            // one).
            return out.empty() ? 0 : 1;
        }
        const FeedResult r = reader_.feed(chunk_.data(), got, out);
        if (r == FeedResult::Poisoned) return -2;
        if (!out.empty()) return 1;
        // Incomplete → read more.
    }
}

} // namespace openrar::sandbox
