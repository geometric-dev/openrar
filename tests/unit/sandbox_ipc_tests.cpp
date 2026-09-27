// Sandbox IPC gates (v1.30.0 M3 foundation; plan §1.3, Gate 0 directives 2/8).
//
// The broker↔worker frame protocol is the ONE new attack surface the
// sandboxed-worker design adds, so it gets parser-grade negative coverage
// here, independent of any sandbox model:
//
//   sandbox_ipc_malformed_frame_rejected — overlong length claims and
//     unregistered types POISON the stream permanently (no resync);
//   sandbox_ipc_bounded_frame — a hostile length claim cannot reserve memory
//     before its bytes arrive; buffers never exceed header + 4 MiB;
//   channel roundtrip over REAL OS pipes in both directions, EOF semantics,
//     poison propagation, and a large DataChunk-sized frame under a thread.
//
// The per-OS privilege models (AppContainer / seccomp / Seatbelt) carry
// their own proof-of-denial suites when they land (M3a/b/c).

#include "test_support.hpp"

#include "../../src/sandbox/channel.hpp"
#include "../../src/sandbox/ipc.hpp"
#include "../../src/sandbox/sandbox_mode.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <cstdlib>
static void set_env(const char* name, const char* value) {
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
}
static void unset_env(const char* name) {
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
}
#else
#include <cstdlib>
#include <unistd.h>
static void set_env(const char* name, const char* value) {
    setenv(name, value, 1);
}
static void unset_env(const char* name) {
    unsetenv(name);
}
#endif

using namespace openrar;
using namespace openrar::sandbox;

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

namespace {

Frame make_frame(FrameType t, size_t payload_len, unsigned seed) {
    Frame f;
    f.type = t;
    f.payload.resize(payload_len);
    for (size_t i = 0; i < payload_len; ++i)
        f.payload[i] = core::byte(static_cast<core::uint8>((i * 31u + seed) & 0xFF));
    return f;
}

bool frames_equal(const Frame& a, const Frame& b) {
    return a.type == b.type && a.payload.size() == b.payload.size() &&
           std::memcmp(a.payload.data(), b.payload.data(), a.payload.size()) == 0;
}

std::vector<core::byte> encode_all(const std::vector<Frame>& frames) {
    std::vector<core::byte> wire;
    for (const Frame& f : frames) {
        CHECK(encode_frame(f, wire));
    }
    return wire;
}

// Appends a raw hostile header: u32le len + u8 type.
void append_raw_header(std::vector<core::byte>& out, core::uint32 len, core::uint8 type) {
    out.push_back(core::byte(static_cast<core::uint8>(len & 0xFF)));
    out.push_back(core::byte(static_cast<core::uint8>((len >> 8) & 0xFF)));
    out.push_back(core::byte(static_cast<core::uint8>((len >> 16) & 0xFF)));
    out.push_back(core::byte(static_cast<core::uint8>((len >> 24) & 0xFF)));
    out.push_back(core::byte(type));
}

void test_frame_roundtrip() {
    std::cout << "[+] frame_roundtrip\n";
    std::vector<Frame> frames = {
        make_frame(FrameType::Ping, 0, 1),
        make_frame(FrameType::Open, 4, 2),
        make_frame(FrameType::KdfParams, 64, 3),
        make_frame(FrameType::Key, 32, 4),
        make_frame(FrameType::ScanResult, 640, 5),
        make_frame(FrameType::ExtractReq, 4, 6),
        make_frame(FrameType::DataChunk, 256 * 1024, 7),
        make_frame(FrameType::EntryResult, 24, 8),
        make_frame(FrameType::Error, 12, 9),
        make_frame(FrameType::Cancel, 0, 10),
        make_frame(FrameType::Shutdown, 0, 11),
    };
    const std::vector<core::byte> wire = encode_all(frames);

    FrameReader rd;
    std::vector<Frame> got;
    // Feed in irregular chunks (1, 7, 64 KiB, rest) to exercise the
    // incremental state machine across boundaries.
    size_t off = 0;
    const size_t steps[] = {1, 7, 64 * 1024};
    FeedResult last = FeedResult::Ok;
    for (size_t s : steps) {
        if (off >= wire.size()) break;
        const size_t n = (wire.size() - off) < s ? (wire.size() - off) : s;
        last = rd.feed(wire.data() + off, n, got);
        off += n;
        CHECK(last == FeedResult::Incomplete || last == FeedResult::Ok);
    }
    if (off < wire.size()) last = rd.feed(wire.data() + off, wire.size() - off, got);
    CHECK(last == FeedResult::Ok);
    CHECK(!rd.poisoned());
    CHECK(got.size() == frames.size());
    for (size_t i = 0; i < frames.size() && i < got.size(); ++i) {
        CHECK(frames_equal(got[i], frames[i]));
    }
}

void test_malformed_frames_poison() {
    std::cout << "[+] sandbox_ipc_malformed_frame_rejected\n";
    // Length claim one past the cap → poisoned, permanently.
    {
        std::vector<core::byte> wire;
        append_raw_header(wire, kMaxFramePayload + 1,
                          static_cast<core::uint8>(FrameType::DataChunk));
        FrameReader rd;
        std::vector<Frame> got;
        CHECK(rd.feed(wire.data(), wire.size(), got) == FeedResult::Poisoned);
        CHECK(rd.poisoned());
        got.clear();
        // No resync, ever: even a perfectly valid frame after the violation
        // stays dead.
        const std::vector<core::byte> good = encode_all({make_frame(FrameType::Ping, 0, 1)});
        CHECK(rd.feed(good.data(), good.size(), got) == FeedResult::Poisoned);
        CHECK(got.empty());
    }
    // Type 0 (unregistered) → poisoned.
    {
        std::vector<core::byte> wire;
        append_raw_header(wire, 0, 0);
        FrameReader rd;
        std::vector<Frame> got;
        CHECK(rd.feed(wire.data(), wire.size(), got) == FeedResult::Poisoned);
    }
    // Type above the registry → poisoned.
    {
        std::vector<core::byte> wire;
        append_raw_header(wire, 3, 200);
        wire.push_back(core::byte(1));
        wire.push_back(core::byte(2));
        wire.push_back(core::byte(3));
        FrameReader rd;
        std::vector<Frame> got;
        CHECK(rd.feed(wire.data(), wire.size(), got) == FeedResult::Poisoned);
    }
}

void test_incomplete_then_complete() {
    std::cout << "[+] incomplete_then_complete\n";
    const std::vector<core::byte> wire = encode_all({make_frame(FrameType::EntryResult, 40, 42)});
    FrameReader rd;
    std::vector<Frame> got;
    // Half the header.
    CHECK(rd.feed(wire.data(), 2, got) == FeedResult::Incomplete);
    CHECK(got.empty());
    // Rest of the header.
    CHECK(rd.feed(wire.data() + 2, 3, got) == FeedResult::Incomplete);
    // Ten payload bytes (32 short).
    CHECK(rd.feed(wire.data() + 5, 10, got) == FeedResult::Incomplete);
    CHECK(rd.buffered() == 10);
    // The tail completes it.
    CHECK(rd.feed(wire.data() + 15, wire.size() - 15, got) == FeedResult::Ok);
    CHECK(got.size() == 1);
    CHECK(frames_equal(got[0], make_frame(FrameType::EntryResult, 40, 42)));
    CHECK(!rd.poisoned());
}

void test_bounded_memory() {
    std::cout << "[+] sandbox_ipc_bounded_frame\n";
    // A VALID 4 MiB claim followed by only 10 real bytes: the reader counts
    // REAL buffered bytes (10), not the claimed length, and does not poison —
    // the claim is legal, just unfinished. Capacity never exceeds
    // header + kMaxFramePayload by construction (the >cap claim poisons
    // before any reserve).
    std::vector<core::byte> wire;
    append_raw_header(wire, kMaxFramePayload, static_cast<core::uint8>(FrameType::DataChunk));
    for (int i = 0; i < 10; ++i) wire.push_back(core::byte(static_cast<core::uint8>(i)));
    FrameReader rd;
    std::vector<Frame> got;
    CHECK(rd.feed(wire.data(), wire.size(), got) == FeedResult::Incomplete);
    CHECK(got.empty());
    CHECK(rd.buffered() == 10);
    CHECK(rd.buffered() <= kFrameHeaderSize + kMaxFramePayload);
}

void test_encode_rejections() {
    std::cout << "[+] encode_rejections\n";
    std::vector<core::byte> out;
    Frame big;
    big.type = FrameType::DataChunk;
    big.payload.resize(kMaxFramePayload + 1);
    CHECK(!encode_frame(big, out));
    CHECK(out.empty());
    Frame bad;
    bad.type = static_cast<FrameType>(99);
    CHECK(!encode_frame(bad, out));
    CHECK(out.empty());
}

void test_channel_roundtrip_and_eof() {
    std::cout << "[+] channel_roundtrip_and_eof\n";
    ChannelPair p;
    CHECK(create_channel_pair(p));
    {
        FramedChannel cmd_w(p.cmd_write);
        FramedChannel cmd_r(p.cmd_read);
        const Frame out1 = make_frame(FrameType::Open, 12, 5);
        const Frame out2 = make_frame(FrameType::Key, 32, 6);
        CHECK(cmd_w.send(out1));
        CHECK(cmd_w.send(out2));
        std::vector<Frame> got;
        CHECK(cmd_r.recv(got) == 1);
        CHECK(got.size() == 2);
        CHECK(frames_equal(got[0], out1));
        CHECK(frames_equal(got[1], out2));
        got.clear();

        // Reverse direction over the results channel.
        FramedChannel res_w(p.res_write);
        FramedChannel res_r(p.res_read);
        const Frame r1 = make_frame(FrameType::EntryResult, 24, 7);
        CHECK(res_w.send(r1));
        CHECK(res_r.recv(got) == 1);
        CHECK(got.size() == 1);
        CHECK(frames_equal(got[0], r1));

        // EOF: closing the peer's write end turns the next recv into 0.
        close_io_handle(p.cmd_write);
        got.clear();
        CHECK(cmd_r.recv(got) == 0);
    }
    close_channel_pair(p);
}

void test_channel_poison_propagates() {
    std::cout << "[+] channel_poison_propagates\n";
    ChannelPair p;
    CHECK(create_channel_pair(p));
    {
        // Hostile bytes on the wire (length claim past the cap) → -2, and
        // the channel stays dead for every later call.
        const core::byte hostile[5] = {core::byte(0xFF), core::byte(0xFF), core::byte(0xFF),
                                       core::byte(0xFF), core::byte(0x07)};
        CHECK(write_all(p.cmd_write, hostile, sizeof(hostile)));
        FramedChannel cmd_r(p.cmd_read);
        std::vector<Frame> got;
        CHECK(cmd_r.recv(got) == -2);
        CHECK(cmd_r.poisoned());
        got.clear();
        CHECK(cmd_r.recv(got) == -2);
    }
    close_channel_pair(p);
}

void test_channel_large_frame_threaded() {
    std::cout << "[+] channel_large_frame_threaded\n";
    ChannelPair p;
    CHECK(create_channel_pair(p));
    const Frame big = make_frame(FrameType::DataChunk, 1024 * 1024, 9); // > pipe buffer
    std::atomic<bool> sender_ok{false};
    std::thread sender([&] {
        FramedChannel res_w(p.res_write);
        sender_ok = res_w.send(big);
        // Close after the frame so the reader sees EOF once it drains.
        close_io_handle(p.res_write);
    });
    {
        FramedChannel res_r(p.res_read);
        std::vector<Frame> got;
        const int rc = res_r.recv(got); // blocks until the sender drains
        CHECK(rc == 0 || rc == 1);      // EOF after the single frame is fine
        CHECK(got.size() == 1);
        if (got.size() == 1) CHECK(frames_equal(got[0], big));
    }
    sender.join();
    CHECK(sender_ok.load());
    close_io_handle(p.cmd_read);
    close_io_handle(p.cmd_write);
    close_io_handle(p.res_read);
}

void test_mode_decision_function() {
    std::cout << "[+] sandbox_mode_decision\n";
    unset_env("OPENRAR_IN_PROC");
    // Explicit flag wins.
    CHECK(sandbox_mode_for(true) == SandboxMode::InProc);
    // Env kill-switch wins over the (currently false) platform probe.
    set_env("OPENRAR_IN_PROC", "1");
    CHECK(sandbox_mode_for(false) == SandboxMode::InProc);
    set_env("OPENRAR_IN_PROC", "0");
    unset_env("OPENRAR_IN_PROC");
    // No flag, no env, probe false (foundation stub) → in-process.
    CHECK(sandbox_mode_for(false) == SandboxMode::InProc);
    CHECK(!platform_sandbox_available());
}

} // namespace

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    test_frame_roundtrip();
    test_malformed_frames_poison();
    test_incomplete_then_complete();
    test_bounded_memory();
    test_encode_rejections();
    test_channel_roundtrip_and_eof();
    test_channel_poison_propagates();
    test_channel_large_frame_threaded();
    test_mode_decision_function();
    if (fails == 0) std::printf("[sandbox-ipc] frame codec + channel gates: OK\n");
    return fails == 0 ? 0 : 1;
}
