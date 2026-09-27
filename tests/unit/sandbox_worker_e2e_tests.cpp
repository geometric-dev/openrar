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
#include "../../src/core/types.hpp"
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

#ifndef OPENRAR_WORKER_EXE
#define OPENRAR_WORKER_EXE ""
#endif

using namespace openrar;
using namespace openrar::archive;
using namespace openrar::sandbox;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

namespace {

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_wk2e_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

fs::path build_two_entry_archive(const fs::path& dir) {
    const fs::path arc = dir / "e2e.rar";
    const fs::path blob = dir / "blob.bin";
    {
        std::ofstream f(blob, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 2048; ++i) f << "stored-" << i << "\n";
    }
    assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, "a/stored.bin", 0));
    {
        std::ofstream f(blob, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 4096; ++i) f << "content-" << i << "\n";
    }
    assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, "b/deflated.bin", 3));
    std::error_code ec;
    fs::remove(blob, ec);
    return arc;
}

bool worker_available() {
    return std::string(OPENRAR_WORKER_EXE)[0] != '\0' && fs::exists(OPENRAR_WORKER_EXE);
}

} // namespace

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    if (!worker_available()) {
        std::printf("[SKIP] sandbox_worker_e2e: openrar_worker not built\n");
        return 0;
    }
    const std::string worker_exe = OPENRAR_WORKER_EXE;

    // ── Lifecycle + list + extract parity ─────────────────────────────────
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
    std::cout << "[+] worker_broker_enforces_caps\n";
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
        assert(archive::ArchiveMutator::add_file_to_archive(big, blob, "big.bin", 0));
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
        CHECK(rc != RAR_OK);      // a dead worker is a clean failure, never a hang
        CHECK(!broker.is_open()); // torn down
        broker.shutdown();        // idempotent
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (fails == 0) std::printf("[worker-e2e] broker/worker harness: OK\n");
    return fails == 0 ? 0 : 1;
}
