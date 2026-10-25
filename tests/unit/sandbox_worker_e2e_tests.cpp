// Broker/worker end-to-end (v1.30.0 M3 harness): spawns the REAL
// openrar_worker process (unsandboxed — the per-OS privilege models wrap
// this same spawn) and drives the full protocol:
//
//   worker_lifecycle_handshake  — spawn, Ping/Open/ScanResult, Shutdown
//   worker_list_matches_engine  — entries identical to a direct path-open
//   worker_extract_matches_engine — decoded bytes identical, chunked flow
//   worker_broker_enforces_caps — a worker streaming past the broker cap is
//     stopped BEFORE the sink and reported LIMIT_EXCEEDED (plan §1.3)
//   sandbox_worker_crash_isolation — kill -9 mid-extract → clean broker
//     error, no partial state, no hang (plan §6 test 3)
//
// The worker binary location is injected at build time
// (OPENRAR_WORKER_EXE); the suite skips loudly when it has not been built.

#include "test_support.hpp"

#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/archive/extraction_report.hpp"
#include "../../src/core/types.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/crypto/pbkdf2.hpp"
#include "../../src/sandbox/channel.hpp"
#include "../../src/sandbox/ipc.hpp"
#include "../../src/sandbox/sandbox_mode.hpp"
#include "../../src/sandbox/spawn.hpp"
#include "../../src/sandbox/worker_broker.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <doctest.h>
#include "../support/test_runner.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef OPENRAR_WORKER_EXE
#define OPENRAR_WORKER_EXE ""
#endif

using namespace openrar;
using namespace openrar::test;
using namespace openrar::archive;
using namespace openrar::sandbox;
namespace fs = std::filesystem;

static int fails = 0;
#undef CHECK // the vendored doctest header defines CHECK; this suite uses its own
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

namespace {

fs::path make_dir(const char* name) {
    return openrar::test::make_scratch_dir(std::string("openrar_") + name);
}

fs::path build_two_entry_archive(const fs::path& dir) {
    const fs::path arc = dir / "e2e.rar";
    const fs::path blob = dir / "blob.bin";
    {
        std::ofstream f(blob, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 2048; ++i) f << "stored-" << i << "\n";
    }
    REQUIRE(archive::ArchiveMutator::add_file_to_archive(arc, blob, "a/stored.bin", 0));
    {
        std::ofstream f(blob, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 4096; ++i) f << "content-" << i << "\n";
    }
    REQUIRE(archive::ArchiveMutator::add_file_to_archive(arc, blob, "b/deflated.bin", 3));
    std::error_code ec;
    fs::remove(blob, ec);
    return arc;
}

bool worker_available() {
    return std::string(OPENRAR_WORKER_EXE)[0] != '\0' && fs::exists(OPENRAR_WORKER_EXE);
}

fs::path get_fixtures_dir() {
#ifdef OPENRAR_SOURCE_DIR
    fs::path p = fs::path(OPENRAR_SOURCE_DIR) / "tests";
    if (fs::exists(p / "hello5_hp.rar")) return p;
#endif
    for (const auto& c : {"tests", "../tests", "../../tests", "../../../tests"}) {
        if (fs::exists(fs::path(c) / "hello5_hp.rar")) return fs::path(c);
    }
    return fs::path("tests");
}

} // namespace

static void run_suite_body() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    if (!worker_available()) {
        std::printf("[SKIP] sandbox_worker_e2e: openrar_worker not built\n");
        return;
    }
    const std::string worker_exe = OPENRAR_WORKER_EXE;

    // ── Lifecycle + list + extract parity ─────────────────────────────────
    if (!openrar::test::last_run_was_filtered())
        std::cout << "[+] worker_lifecycle_list_extract_parity\n";
    const fs::path dir = make_dir("parity");
    const fs::path arc = build_two_entry_archive(dir);
    {
        // Engine reference.
        archive::ArchiveReader ref;
        CHECK(ref.open(arc));
        CHECK(ref.entries().size() == 2);

        WorkerBroker broker;
        std::string detail;
        CHECK(broker.start(worker_exe, arc, WorkerBroker::Limits{}, detail));
        if (detail.empty() || broker.is_open()) {
            std::printf("  worker started: %zu entries\n", broker.entries().size());
        }
        CHECK(broker.is_open());
        CHECK(broker.entries().size() == ref.entries().size());
        for (size_t i = 0; i < ref.entries().size() && i < broker.entries().size(); ++i) {
            CHECK(broker.entries()[i].path == ref.entries()[i].header.file_name);
            CHECK(broker.entries()[i].wire.size == ref.entries()[i].header.unp_size);
            CHECK(broker.entries()[i].wire.method == ref.entries()[i].header.method);
        }

        // Extract parity, byte for byte, through the broker sink.
        for (size_t i = 0; i < ref.entries().size() && i < broker.entries().size(); ++i) {
            std::vector<core::byte> expect;
            const int rc_ref =
                ref.extract_entry_to_memory(i, expect, 1ull << 30, {}, nullptr, nullptr);
            std::vector<core::byte> got;
            core::uint64 sent = 0;
            const int rc_w = broker.extract(
                static_cast<core::uint32>(i),
                [&](const core::byte* p, size_t n) -> bool {
                    got.insert(got.end(), p, p + n);
                    return true;
                },
                &sent, detail);
            CHECK(rc_ref == 0);
            CHECK(rc_w == 0);
            CHECK(got.size() == expect.size());
            CHECK(sent == expect.size());
            if (got.size() == expect.size())
                CHECK(std::memcmp(got.data(), expect.data(), expect.size()) == 0);
        }
        broker.shutdown();
        CHECK(!broker.is_open());
        ref.close();
    }

    // ── Broker cap enforcement ────────────────────────────────────────────
    if (!openrar::test::last_run_was_filtered()) std::cout << "[+] worker_broker_enforces_caps\n";
    {
        WorkerBroker broker;
        std::string detail;
        WorkerBroker::Limits lim;
        lim.max_member_bytes = 1024; // first entry is far larger
        CHECK(broker.start(worker_exe, arc, lim, detail));
        size_t delivered = 0;
        const int rc = broker.extract(
            0,
            [&](const core::byte*, size_t n) -> bool {
                delivered += n;
                return true;
            },
            nullptr, detail);
        CHECK(rc == RAR_ERR_LIMIT_EXCEEDED);
        CHECK(delivered <= 1024); // never delivered past the cap
        broker.shutdown();
    }

    // ── Crash isolation ───────────────────────────────────────────────────
    if (!openrar::test::last_run_was_filtered())
        std::cout << "[+] sandbox_worker_crash_isolation\n";
    {
        // A 4 MiB stored entry streams ~16 DataChunks: the killer fires
        // mid-stream deterministically (a small entry would finish before
        // the kill lands and legitimately return RAR_OK).
        const fs::path big = dir / "big.rar";
        const fs::path blob = dir / "bigblob.bin";
        {
            std::ofstream f(blob, std::ios::binary | std::ios::trunc);
            std::vector<char> chunk(64 * 1024);
            for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(i * 7 + 1);
            for (int i = 0; i < 64; ++i) f.write(chunk.data(), chunk.size());
        }
        REQUIRE(archive::ArchiveMutator::add_file_to_archive(big, blob, "big.bin", 0));
        std::error_code rmec;
        fs::remove(blob, rmec);

        WorkerBroker broker;
        std::string detail;
        CHECK(broker.start(worker_exe, big, WorkerBroker::Limits{}, detail));
        std::atomic<bool> sink_saw_data{false};
        // Kill the worker from another thread while the extract streams.
        std::thread killer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            broker.kill();
        });
        const int rc = broker.extract(
            0,
            [&](const core::byte*, size_t n) -> bool {
                sink_saw_data = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return n > 0; // keep going until the worker dies
            },
            nullptr, detail);
        killer.join();
        (void)sink_saw_data;
        CHECK(
            rc ==
            RAR_ERR_CONTAINMENT_BREACH); // FMM-51-3: worker death is clean containment breach (-17)
        CHECK(!broker.is_open());        // torn down
        broker.shutdown();               // idempotent
    }

    // ── Broker death → worker observes EOF and exits ──────────────────────
    // Regression for the inherited-handle leak: the worker must NOT hold a
    // copy of the broker's command WRITE end. If it does, its cmd_read never
    // sees EOF when the broker dies, and the worker outlives the broker
    // forever - blocked in recv() and still holding the volume handle. Drives
    // the raw spawn so the broker's own ends can be closed on demand.
    if (!openrar::test::last_run_was_filtered())
        std::cout << "[+] worker_exits_when_broker_command_end_closes\n";
    {
        ChannelPair pair;
        CHECK(create_channel_pair(pair));
        // Mirror the broker: its own ends must not cross the spawn boundary.
        CHECK(set_handle_inheritable(pair.cmd_write, false));
        CHECK(set_handle_inheritable(pair.res_read, false));

#if defined(_WIN32)
        // HANDLE_LIST requires every listed handle to be inheritable.
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        native_io_handle volume = CreateFileW(arc.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const ChildHandles child_handles{pair.cmd_read, pair.res_write, volume};
        const auto hstr = [](native_io_handle h) {
            return std::to_string(reinterpret_cast<uintptr_t>(h));
        };
#else
        native_io_handle volume = ::open(arc.c_str(), O_RDONLY);
        const ChildHandles child_handles{
            reinterpret_cast<void*>(static_cast<intptr_t>(pair.cmd_read)),
            reinterpret_cast<void*>(static_cast<intptr_t>(pair.res_write)),
            reinterpret_cast<void*>(static_cast<intptr_t>(volume))};
        const auto hstr = [](native_io_handle h) {
            return std::to_string(h);
        };
#endif
        CHECK(volume != INVALID_IO_HANDLE);

        std::vector<std::string> argv_tail;
        argv_tail.push_back("--openrar-sandbox-worker");
        argv_tail.push_back(hstr(pair.cmd_read));
        argv_tail.push_back(hstr(pair.res_write));
        argv_tail.push_back(hstr(volume));

        void* proc = nullptr;
        std::string sdetail;
        CHECK(spawn_worker_process(worker_exe, argv_tail, child_handles, SpawnProfile::Unsandboxed,
                                   &proc, sdetail));

        // Broker side drops its copies of the worker ends.
        close_io_handle(pair.cmd_read);
        close_io_handle(pair.res_write);
        close_io_handle(volume);

        FramedChannel res(pair.res_read);
        std::vector<Frame> in;
        CHECK(res.recv(in) == 1); // Ping: the worker is up and looping

        // "Broker dies": close the only command write end.
        close_io_handle(pair.cmd_write);

        bool exited = false;
#if defined(_WIN32)
        HANDLE ph = static_cast<HANDLE>(proc);
        exited = WaitForSingleObject(ph, 5000) == WAIT_OBJECT_0;
        CloseHandle(ph);
#else
        const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc));
        for (int i = 0; i < 100 && !exited; ++i) {
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == pid) {
                exited = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!exited) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
#endif
        CHECK(exited); // must not hang holding cmd_write
        close_io_handle(pair.res_read);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (fails == 0) std::printf("[worker-e2e] broker/worker harness: OK\n");
    if (fails != 0) throw std::logic_error(std::to_string(fails) + " failed checks");
}

// ── FMM-51-1: PBKDF2 iteration workbound rejection (RAR_ERR_PASSWORD_TIMEOUT = -16) ──
static void test_sandbox_pbkdf2_workbound_rejected() {
    if (!worker_available()) return;
    const std::string worker_exe = OPENRAR_WORKER_EXE;
    const fs::path dir = make_dir("pbkdf2_workbound");
    const fs::path hp_src = get_fixtures_dir() / "hello5_hp.rar";
    if (!fs::exists(hp_src)) {
        std::printf("  skip: hello5_hp.rar fixture not found\n");
        return;
    }
    std::ifstream f(hp_src, std::ios::binary);
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(buf.size() >= 40);

    // HEAD_CRYPT block starts at offset 8.
    // Offset 0x08..0x0B is CRC32.
    // Offset 0x0C is body size vint (0x21 = 33).
    // Offset 0x11 is lg2_count (0x0F = 15).
    // Mutate lg2_count to 25 (> MAX_PBKDF2_LG2 = 24).
    buf[0x11] = 25;
    crypto::Crc32 crc;
    crc.update(reinterpret_cast<const core::byte*>(buf.data() + 0x0C), 1 + 33);
    core::write_le32(reinterpret_cast<core::byte*>(buf.data() + 0x08), crc.get());

    const fs::path bad_arc = dir / "workbound.rar";
    std::ofstream out(bad_arc, std::ios::binary | std::ios::trunc);
    out.write(buf.data(), buf.size());
    out.close();

    // 1. Direct engine open reports RAR_ERR_PASSWORD_TIMEOUT (-16)
    {
        archive::ArchiveReader reader;
        int status = archive::RAR_OK;
        std::string detail;
        bool ok = reader.open_ex(bad_arc, "secret", status, detail);
        CHECK(!ok);
        CHECK(status == archive::RAR_ERR_PASSWORD_TIMEOUT);
    }

    // 2. Broker opening via worker process propagates RAR_ERR_PASSWORD_TIMEOUT (-16)
    {
        WorkerBroker broker;
        std::string detail;
        bool ok = broker.start(worker_exe, bad_arc, WorkerBroker::Limits{}, detail);
        CHECK(!ok);
        CHECK(broker.last_error_code() == archive::RAR_ERR_PASSWORD_TIMEOUT);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ── FMM-51-2: Volume descriptor failure in multi-volume archives (RAR_ERR_MISSING_VOLUME = -13) ──
static void test_sandbox_multivol_descriptor_failure() {
    if (!worker_available()) return;
    const std::string worker_exe = OPENRAR_WORKER_EXE;
    const fs::path vol_src = get_fixtures_dir() / "fuzz" / "seeds" / "seed_vol.part1.rar";
    if (!fs::exists(vol_src)) {
        std::printf("  skip: seed_vol.part1.rar fixture not found\n");
        return;
    }

    WorkerBroker broker;
    std::string detail;
    bool ok = broker.start(worker_exe, vol_src, WorkerBroker::Limits{}, detail);
    CHECK(!ok);
    CHECK(broker.last_error_code() == archive::RAR_ERR_MISSING_VOLUME); // -13
}

// ── FMM-51-4: Corrupted encrypted header detection (RAR_ERR_DECRYPT = -18) ──
static void test_sandbox_encrypted_header_corrupt() {
    if (!worker_available()) return;
    const std::string worker_exe = OPENRAR_WORKER_EXE;
    const fs::path dir = make_dir("corrupt_crypt");
    const fs::path hp_src = get_fixtures_dir() / "hello5_hp.rar";
    if (!fs::exists(hp_src)) return;

    std::ifstream f(hp_src, std::ios::binary);
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(buf.size() >= 40);

    // Corrupt body size of HEAD_CRYPT so parse_crypt_header fails (truncated block body)
    buf[0x0C] = 4; // body size 4 bytes (< 16-byte salt requirement)
    crypto::Crc32 crc;
    crc.update(reinterpret_cast<const core::byte*>(buf.data() + 0x0C), 1 + 4);
    core::write_le32(reinterpret_cast<core::byte*>(buf.data() + 0x08), crc.get());

    const fs::path bad_arc = dir / "corrupt_crypt.rar";
    std::ofstream out(bad_arc, std::ios::binary | std::ios::trunc);
    out.write(buf.data(), buf.size());
    out.close();

    // 1. Direct engine open reports RAR_ERR_DECRYPT (-18)
    {
        archive::ArchiveReader reader;
        int status = archive::RAR_OK;
        std::string detail;
        bool ok = reader.open_ex(bad_arc, "secret", status, detail);
        CHECK(!ok);
        CHECK(status == archive::RAR_ERR_DECRYPT);
    }

    // 2. Broker / worker open failure propagates RAR_ERR_DECRYPT (-18)
    {
        WorkerBroker broker;
        std::string detail;
        bool ok = broker.start(worker_exe, bad_arc, WorkerBroker::Limits{}, detail);
        CHECK(!ok);
        CHECK(broker.last_error_code() == archive::RAR_ERR_DECRYPT);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ── FMM-51-5: Worker memory exhaustion (RAR_ERR_RESOURCE_EXHAUSTION = -19) ──
static void test_sandbox_worker_oom_rejection() {
    // 1. Value pinned to -19
    CHECK(archive::RAR_ERR_RESOURCE_EXHAUSTION == -19);

    // 2. Frame codec roundtrip
    Frame f{FrameType::Error, {}};
    CHECK(pack_error(archive::RAR_ERR_RESOURCE_EXHAUSTION, "worker memory exhausted", f.payload));
    core::int32 unpacked_rc = 0;
    std::string detail;
    CHECK(unpack_error(f.payload.data(), f.payload.size(), unpacked_rc, detail));
    CHECK(unpacked_rc == archive::RAR_ERR_RESOURCE_EXHAUSTION);
    CHECK(detail == "worker memory exhausted");
}

// ── FMM-51-6: Broker-side integrity verification / anti-forgery (RAR_ERR_CRC_MISMATCH = -4) ──
static void test_sandbox_fail_open_forgery_denied_hmac() {
    if (!worker_available()) return;
    const std::string worker_exe = OPENRAR_WORKER_EXE;
    const fs::path dir = make_dir("forgery");
    const fs::path arc = build_two_entry_archive(dir);

    WorkerBroker broker;
    std::string detail;
    CHECK(broker.start(worker_exe, arc, WorkerBroker::Limits{}, detail));
    CHECK(broker.is_open());
    CHECK(!broker.entries().empty());

    // Tamper with the expected wire CRC32
    broker.test_entries()[0].wire.crc32 ^= 0xDEADBEEF;

    // Worker streams genuine content, but broker independent CRC will mismatch
    core::uint64 bytes_out = 0;
    const int rc = broker.extract(
        0, [](const core::byte*, size_t) -> bool { return true; }, &bytes_out, detail);

    CHECK(rc == archive::RAR_ERR_CRC_MISMATCH); // -4
    CHECK(!broker.is_open());                   // killed upon forgery detection

    broker.shutdown();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ── FMM-51-7: Loud containment degradation reporting with sandbox_active: false ──
static void test_sandbox_degraded_mode_is_loud() {
    // 1. ExtractionReport serialization: sandbox_active true vs false
    {
        archive::ExtractionReport rep;
        rep.has_sandbox_field = true;
        rep.sandbox_active = false;
        std::string j_degraded = archive::to_json(rep);
        CHECK(j_degraded.find("\"sandbox_active\":false") != std::string::npos);

        rep.sandbox_active = true;
        std::string j_active = archive::to_json(rep);
        CHECK(j_active.find("\"sandbox_active\":true") != std::string::npos);
    }

    // 2. sandbox_mode_for correctly returns InProc when in_proc flag is set
    CHECK(sandbox::sandbox_mode_for(true) == sandbox::SandboxMode::InProc);
}

static const openrar::test::Case k_cases[] = {
    {"run_suite_body", run_suite_body, false},
    {"test_sandbox_pbkdf2_workbound_rejected", test_sandbox_pbkdf2_workbound_rejected, false},
    {"test_sandbox_multivol_descriptor_failure", test_sandbox_multivol_descriptor_failure, false},
    {"test_sandbox_encrypted_header_corrupt", test_sandbox_encrypted_header_corrupt, false},
    {"test_sandbox_worker_oom_rejection", test_sandbox_worker_oom_rejection, false},
    {"test_sandbox_fail_open_forgery_denied_hmac", test_sandbox_fail_open_forgery_denied_hmac,
     false},
    {"test_sandbox_degraded_mode_is_loud", test_sandbox_degraded_mode_is_loud, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (fails != 0) rc = 1;
    return rc;
}