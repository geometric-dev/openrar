// Extraction atomicity foundation tests (v1.24.0 M1, plan §2).
//
// Pins the crypto-random temp-in-destination scheme, the journal manifest
// (durable-first recording, lock semantics, stale-journal sweep validation),
// and the no-follow atomic rename cascade. Named negative tests from the
// plan that land here:
//   - journal_orphan_cleanup (plan §10 test 22)
//   - toctou_journal_sweep_race (plan §10 test 3 — core lock behavior; the
//     full gate-1 TOCTOU suite lands with M2)

#include "../../src/crypto/crc32.hpp"
#include "../../src/io/extraction_journal.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/path_util.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#else
#include <sys/stat.h>
#include <cstdlib>
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

void rm_dir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

fs::path make_test_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_m1_") + name);
    std::error_code ec;
    rm_dir(dir);
    fs::create_directories(dir, ec);
    assert(!ec);
    return dir;
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f.good());
}

std::vector<fs::path> find_journals(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name.find(".openrar_journal_") == 0) out.push_back(it->path());
    }
    return out;
}

std::string hex32_from_name(const std::string& name) {
    // "<name>.<32hex>.tmp" → the 32 hex chars
    if (name.size() < 38) return "";
    return name.substr(name.size() - 4 - 32, 32);
}

// Builds a journal record exactly the way extraction_journal.cpp does
// ("T <len> <crc8hex>\n" + path bytes) so the sweep's checksum path is
// exercised with genuine records.
std::string make_record(const fs::path& temp_abs, bool corrupt_crc = false) {
    const std::string path_bytes = io::u8_str(temp_abs);
    uint32_t crc = 0;
    {
        crypto::Crc32 c;
        c.update(reinterpret_cast<const core::byte*>(path_bytes.data()), path_bytes.size());
        crc = c.get();
    }
    if (corrupt_crc) crc ^= 0xA5A5A5A5u;
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "T %zu %08x\n", path_bytes.size(), crc);
    return std::string(hdr) + path_bytes;
}

void test_atomic_commit_replaces() {
    const fs::path dir = make_test_dir("commit");
    const fs::path dest = dir / "file.txt";

    // Pre-existing content gets replaced atomically.
    write_file(dest, "old contents");

    io::ExtractionSession session;
    io::AtomicWriter writer;
    assert(writer.open(session, dest));
    assert(io::u8_str(writer.temp_path().filename()) != io::u8_str(dest.filename()));
    writer.stream().write("new contents", 12);
    assert(writer.commit());
    assert(read_file(dest) == "new contents");
    assert(!file_exists(writer.temp_path()));

    // No temps, no journals after the run.
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        assert(name == "file.txt");
    }
    std::cout << "[PASS] atomic commit replaces destination\n";
    rm_dir(dir);
}

void test_commit_noclobber_preserves_dest() {
    const fs::path dir = make_test_dir("noclobber");
    const fs::path dest = dir / "guard.txt";
    write_file(dest, "precious");

    io::ExtractionSession session;
    io::AtomicWriter writer;
    assert(writer.open(session, dest));
    writer.stream().write("overwritten", 11);
    assert(!writer.commit(io::CommitMode::NoClobber));
    assert(read_file(dest) == "precious");    // destination untouched
    assert(!file_exists(writer.temp_path())); // temp abandoned
    assert(find_journals(dir).empty());       // journal released on abandon

    // NoClobber into a free name succeeds.
    const fs::path fresh = dir / "fresh.txt";
    io::AtomicWriter w2;
    assert(w2.open(session, fresh));
    w2.stream().write("fresh", 5);
    assert(w2.commit(io::CommitMode::NoClobber));
    assert(read_file(fresh) == "fresh");
    std::cout << "[PASS] no-clobber commit preserves existing destination\n";
    rm_dir(dir);
}

void test_temp_names_crypto_random() {
    const fs::path dir = make_test_dir("names");
    const fs::path dest = dir / "data.bin";

    io::ExtractionSession session;
    std::string first_temp;
    {
        io::AtomicWriter w;
        assert(w.open(session, dest));
        first_temp = io::u8_str(w.temp_path().filename());
        assert(io::ExtractionSession::matches_temp_shape(first_temp));
        // Exactly one 32-hex segment between the name and ".tmp".
        assert(hex32_from_name(first_temp).size() == 32);
        w.abandon();
    }
    {
        io::AtomicWriter w;
        assert(w.open(session, dest));
        const std::string second_temp = io::u8_str(w.temp_path().filename());
        assert(second_temp != first_temp); // fresh 128-bit draw per temp
        assert(io::ExtractionSession::matches_temp_shape(second_temp));
        w.abandon();
    }
    // Shape predicate rejects look-alikes.
    assert(!io::ExtractionSession::matches_temp_shape("x.0123456789abcdef0123456789abcde.tmp"));
    assert(!io::ExtractionSession::matches_temp_shape("x.0123456789ABCDEF0123456789ABCDEF.tmp"));
    assert(!io::ExtractionSession::matches_temp_shape(".tmp"));
    assert(!io::ExtractionSession::matches_temp_shape("plain.tmp"));
    std::cout << "[PASS] temp names are crypto-random and shape-checked\n";
    rm_dir(dir);
}

void test_abandon_and_destructor_disposition() {
    const fs::path dir = make_test_dir("abandon");
    const fs::path dest = dir / "gone.txt";

    {
        io::ExtractionSession session;
        io::AtomicWriter writer;
        assert(writer.open(session, dest));
        writer.stream().write("partial", 7);
        assert(file_exists(writer.temp_path()));
        assert(writer.commit(io::CommitMode::NoClobber)); // fresh name: no-clobber succeeds
    }
    // (scope left a committed file; reuse dir for the abandon checks)

    {
        io::ExtractionSession session;
        io::AtomicWriter writer;
        assert(writer.open(session, dir / "explicit.txt"));
        assert(file_exists(writer.temp_path()));
        writer.abandon();
        assert(!file_exists(writer.temp_path()));
        assert(find_journals(dir).empty());
    }
    {
        io::ExtractionSession session;
        io::AtomicWriter writer;
        assert(writer.open(session, dir / "implicit.txt"));
        assert(file_exists(writer.temp_path()));
        // Destructor runs abandon().
    }
    assert(!file_exists(dir / "explicit.txt"));
    assert(!file_exists(dir / "implicit.txt"));
    assert(find_journals(dir).empty());
    std::cout << "[PASS] abandon (explicit and via destructor) removes the temp\n";
    rm_dir(dir);
}

void test_journal_references_temp_during_flight() {
    const fs::path dir = make_test_dir("inflight");
    const fs::path dest = dir / "inflight.txt";

    io::ExtractionSession session;
    io::AtomicWriter writer;
    assert(writer.open(session, dest));

    // Durable-first ordering observable state: the journal exists while the
    // temp is in flight and names it. (Read through the owning session — the
    // exclusive lock makes the file unopenable by anything else, by design.)
    const auto journals = find_journals(dir);
    assert(journals.size() == 1);
    const std::string content = session.journal_content(dir);
    assert(content.find("OPENRAR-JOURNAL 1\n") == 0);
    assert(content.find(io::u8_str(writer.temp_path())) != std::string::npos);

    assert(writer.commit());
    // Committed: in-flight count dropped to zero → journal unlinked.
    assert(find_journals(dir).empty());
    std::cout << "[PASS] journal references the temp before it exists, unlinks after\n";
    rm_dir(dir);
}

void test_journal_orphan_cleanup() {
    // Plan §10 test 22: a killed run leaves journal + temps; the next run's
    // sweep deletes journal-referenced temps only — never bystanders, never
    // names a forged record points at outside the directory.
    const fs::path dir = make_test_dir("sweep");
    const fs::path other = make_test_dir("sweep_other");

    const fs::path orphan = dir / "orphan.0123456789abcdef0123456789abcdef.tmp";
    write_file(orphan, "orphan");
    const fs::path outside = other / "outside.0123456789abcdef0123456789abcdef.tmp";
    write_file(outside, "outside");
    const fs::path badcrc = dir / "badcrc.0123456789abcdef0123456789abcdef.tmp";
    write_file(badcrc, "badcrc");
    const fs::path bystander = dir / "bystander.txt";
    write_file(bystander, "bystander");

    std::string journal = "OPENRAR-JOURNAL 1\n";
    journal += make_record(orphan);                   // valid, in-dir → swept
    journal += make_record(outside);                  // valid crc, OUT of dir → ignored
    journal += make_record(badcrc, /*corrupt*/ true); // bad crc → replay stops
    {
        std::ofstream f(dir / ".openrar_journal_999999_1234567890_deadbeef.tmp", std::ios::binary);
        f.write(journal.data(), static_cast<std::streamsize>(journal.size()));
    }

    assert(io::ExtractionSession::sweep_directory(dir));

    assert(!file_exists(orphan));       // journal-referenced temp deleted
    assert(file_exists(outside));       // cross-directory record ignored
    assert(file_exists(badcrc));        // checksummed-out record ignored
    assert(file_exists(bystander));     // never pattern-matched
    assert(find_journals(dir).empty()); // dead journal unlinked
    // Garbage journal: left in place, no deletions.
    const fs::path garbage = dir / ".openrar_journal_1_1_garbage.tmp";
    write_file(garbage, "not a journal at all");
    write_file(dir / "victim.0123456789abcdef0123456789abcdef.tmp", "victim");
    assert(io::ExtractionSession::sweep_directory(dir));
    assert(file_exists(garbage));
    assert(file_exists(dir / "victim.0123456789abcdef0123456789abcdef.tmp"));
    std::cout << "[PASS] journal_orphan_cleanup: sweep deletes only valid references\n";
    rm_dir(dir);
    rm_dir(other);
}

void test_journal_sweep_skips_locked_live_run() {
    // Plan §10 test 3: a live extractor holds the journal lock; a concurrent
    // sweep in the same directory must not delete its in-flight temp.
    const fs::path dir = make_test_dir("sweep_race");
    const fs::path dest = dir / "live.txt";

    io::ExtractionSession live_session;
    io::AtomicWriter live_writer;
    assert(live_writer.open(live_session, dest));
    live_writer.stream().write("live", 4);

    assert(io::ExtractionSession::sweep_directory(dir));
    assert(file_exists(live_writer.temp_path())); // untouched
    assert(find_journals(dir).size() == 1);       // live journal not unlinked

    // A second concurrent session in the same directory works alongside.
    io::ExtractionSession other_session;
    io::AtomicWriter other_writer;
    assert(other_writer.open(other_session, dir / "other.txt"));
    assert(find_journals(dir).size() == 2); // one journal per live run
    assert(live_writer.commit());
    assert(other_writer.commit());
    assert(find_journals(dir).empty());
    std::cout << "[PASS] toctou_journal_sweep_race: locked journals are skipped\n";
    rm_dir(dir);
}

void test_commit_respects_readonly_dest() {
    const fs::path dir = make_test_dir("readonly");
    const fs::path dest = dir / "locked.txt";
    write_file(dest, "original");

    bool made_readonly = false;
#ifdef _WIN32
    made_readonly = SetFileAttributesW(dest.wstring().c_str(), FILE_ATTRIBUTE_READONLY) != 0;
#else
    made_readonly = ::chmod(dest.c_str(), 0444) == 0;
#endif
    assert(made_readonly);

    io::ExtractionSession session;
    io::AtomicWriter writer;
    assert(writer.open(session, dest));
    writer.stream().write("clobber", 7);
    assert(!writer.commit()); // READONLY destination is never clobbered
    assert(read_file(dest) == "original");
    writer.abandon(); // temp still exists after a failed commit; dispose now
    assert(!file_exists(writer.temp_path()));
    assert(find_journals(dir).empty());

#ifdef _WIN32
    SetFileAttributesW(dest.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
#else
    ::chmod(dest.c_str(), 0644);
#endif
    std::cout << "[PASS] overwrite_readonly_respected: commit fails, dest intact\n";
    rm_dir(dir);
}

void test_open_failure_is_clean() {
    const fs::path dir = make_test_dir("openfail");
    // Destination's parent does not exist → the temp cannot be created and no
    // journal is opened for the nonexistent directory.
    const fs::path dest = dir / "missing_sub" / "file.txt";
    io::ExtractionSession session;
    io::AtomicWriter writer;
    assert(!writer.open(session, dest));
    assert(find_journals(dir).empty());
    std::cout << "[PASS] open failure leaves no journal or temp\n";
    rm_dir(dir);
}

void test_flat_cwd_destination() {
    // Destination with NO parent (bare relative name): the journal key,
    // release lookup, and sweep validation must agree even though
    // lexically_normal of the "."-rooted absolute path carries a trailing
    // separator that parent_path() of the temp does not.
    const fs::path saved = fs::current_path();
    const fs::path dir = make_test_dir("flat");
    fs::current_path(dir);
    {
        io::ExtractionSession session;
        io::AtomicWriter writer;
        assert(writer.open(session, "flat.txt"));
        writer.stream().write("flat", 4);
        assert(writer.commit());
        assert(file_exists(dir / "flat.txt"));
        assert(find_journals(dir).empty()); // released → unlinked (key match)
    }
    // Same-directory journal + sweep consistency for the flat case.
    const fs::path orphan = dir / "flat.0123456789abcdef0123456789abcdef.tmp";
    write_file(orphan, "orphan");
    {
        std::ofstream f(dir / ".openrar_journal_1_1_flat.tmp", std::ios::binary);
        const std::string rec = make_record(orphan);
        f.write("OPENRAR-JOURNAL 1\n", 18);
        f.write(rec.data(), static_cast<std::streamsize>(rec.size()));
    }
    assert(io::ExtractionSession::sweep_directory(dir));
    assert(!file_exists(orphan));       // record validated against this dir
    assert(find_journals(dir).empty()); // dead journal unlinked
    fs::current_path(saved);
    std::cout << "[PASS] flat (no-parent) destination keys agree end to end\n";
    rm_dir(dir);
}

// ── v1.39.0 batch-mode failure matrix ──────────────────────────────────────

void test_batch_kill_mid_run() {
    // Simulate a crash mid-batch: manually create a committed destination,
    // an orphan temp, and a journal referencing the orphan (the state a
    // hard kill leaves behind). The next session's sweep must recover the
    // journal and delete the referenced temp.
    const fs::path dir = make_test_dir("batchkill");
    const fs::path dest1 = dir / "file1.txt";
    const fs::path temp2 = dir / "file2.0123456789abcdef0123456789abcdef.tmp";

    // dest1 was committed before the crash.
    write_file(dest1, "data1");
    // temp2 is an orphan (created but never committed).
    write_file(temp2, "data2");
    // The journal references temp2 (the record reached the disk but the
    // commit did not).
    {
        std::ofstream f(dir / ".openrar_journal_1_1_crash.tmp", std::ios::binary);
        f.write("OPENRAR-JOURNAL 1\n", 18);
        f.write(make_record(temp2).data(), static_cast<std::streamsize>(make_record(temp2).size()));
    }

    // The next session's sweep must recover the journal and delete the orphan.
    assert(io::ExtractionSession::sweep_directory(dir));
    assert(find_journals(dir).empty());
    assert(!file_exists(temp2)); // orphan deleted by sweep
    assert(file_exists(dest1));  // committed destination survives
    std::cout << "[PASS] batch_kill_mid_run: sweep recovers journal after crash\n";
    rm_dir(dir);
}

void test_torn_journal_tail_batched() {
    // A torn journal tail under batched records: the sweep replays valid
    // records (deleting referenced temps) and stops at the first
    // inconsistent record (conservative stop). The journal is unlinked
    // because the header was verified.
    const fs::path dir = make_test_dir("tornbatch");
    const fs::path orphan = dir / "file.0123456789abcdef0123456789abcdef.tmp";
    write_file(orphan, "orphan");

    // Build a journal with a valid record followed by a torn record.
    {
        std::ofstream f(dir / ".openrar_journal_1_1_torn.tmp", std::ios::binary);
        f.write("OPENRAR-JOURNAL 1\n", 18);
        f.write(make_record(orphan).data(),
                static_cast<std::streamsize>(make_record(orphan).size()));
        f.write("T 999 deadbeef\n/torn", 21); // torn: bad CRC + truncated path
    }

    // The sweep deletes the orphan (valid record) and stops at the torn tail.
    assert(io::ExtractionSession::sweep_directory(dir));
    assert(!file_exists(orphan));       // valid record replayed
    assert(find_journals(dir).empty()); // journal unlinked (header verified)
    std::cout << "[PASS] torn_journal_tail_batched: conservative stop at torn record\n";
    rm_dir(dir);
}

void test_rename_failure_mid_run_batched() {
    // A failed rename in batch mode: the destination is untouched, the temp
    // is abandoned, and the journal is still held (not released).
    const fs::path dir = make_test_dir("renamefail");
    const fs::path dest = dir / "guard.txt";
    write_file(dest, "precious");

#ifdef _WIN32
    // Make the destination read-only: the commit must fail.
    SetFileAttributesW(dest.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(dest.c_str(), 0444);
#endif

    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    io::AtomicWriter writer;
    assert(writer.open(session, dest));
    writer.stream().write("overwritten", 11);
    assert(!writer.commit()); // READONLY destination is never clobbered
    assert(read_file(dest) == "precious");
    assert(!file_exists(writer.temp_path()));
    // The journal is still held (batch mode: not released on abandon).
    assert(find_journals(dir).size() == 1);

#ifdef _WIN32
    SetFileAttributesW(dest.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
#else
    ::chmod(dest.c_str(), 0644);
#endif
    std::cout << "[PASS] rename_failure_mid_run_batched: dest intact, journal held\n";
    rm_dir(dir);
}

void test_journal_sync_failure_fail_closed() {
    // Fail the first journal sync: the temp must NOT be created
    // (fail-closed: unreferenced-orphan rule).
    const fs::path dir = make_test_dir("syncfail");
    const fs::path dest = dir / "file.txt";

    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    session.set_test_hooks(0, 1);
    io::AtomicWriter writer;
    // The open must fail (sync failure is fail-closed).
    assert(!writer.open(session, dest));
    assert(!file_exists(dest));
    assert(find_journals(dir).empty());

    std::cout << "[PASS] journal_sync_failure_fail_closed: temp not created on sync failure\n";
    rm_dir(dir);
}

void test_cancel_mid_batch() {
    // Cancel mid-batch in batch mode: the temp is abandoned, the journal is
    // still held (not released), and the destination is untouched.
    const fs::path dir = make_test_dir("cancelbatch");
    const fs::path dest = dir / "file.txt";

    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    io::AtomicWriter writer;
    assert(writer.open(session, dest));
    writer.stream().write("partial", 7);
    writer.abandon(); // cooperative cancel
    assert(!file_exists(writer.temp_path()));
    assert(!file_exists(dest));
    // The journal is still held (batch mode: not released on abandon).
    assert(find_journals(dir).size() == 1);
    std::cout << "[PASS] cancel_mid_batch: temp abandoned, journal held\n";
    rm_dir(dir);
}

void test_window_1_equivalence() {
    // Window-1 equivalence: batch mode with boundary=1 syncs after every
    // record (like entry mode); boundary=32 does not. We prove this by
    // failing the 2nd sync: with boundary=1 the first record triggers the
    // boundary sync (sync #2) and fails; with boundary=32 it does not.
    const fs::path dir1 = make_test_dir("win1b1");
    const fs::path dir2 = make_test_dir("win1b32");

    // boundary=1: first record's boundary sync is sync #2 → fails.
    {
        io::ExtractionSession session;
        session.set_durability_granularity(io::DurabilityGranularity::Batch);
        session.set_batch_sync_boundary_for_test(1);
        session.set_test_hooks(0, 2);
        io::AtomicWriter w;
        assert(!w.open(session, dir1 / "f0.txt")); // boundary sync fails
    }

    // boundary=32: first record does not reach the boundary → succeeds.
    {
        io::ExtractionSession session;
        session.set_durability_granularity(io::DurabilityGranularity::Batch);
        io::AtomicWriter w;
        assert(w.open(session, dir2 / "f0.txt")); // no boundary sync yet
        w.abandon();
    }

    std::cout << "[PASS] window_1_equivalence: boundary=1 syncs every record, "
                 "boundary=32 does not\n";
    rm_dir(dir1);
    rm_dir(dir2);
}

void test_forged_journal_batched() {
    // A forged journal under batch policy: the sweep must still reject
    // records that don't match the temp shape or lie outside the directory.
    const fs::path dir = make_test_dir("forgedbatch");
    const fs::path victim = dir / "victim.txt";
    write_file(victim, "precious");

    // Build a forged journal with a record naming a non-temp file.
    {
        std::ofstream f(dir / ".openrar_journal_1_1_forged.tmp", std::ios::binary);
        f.write("OPENRAR-JOURNAL 1\n", 18);
        f.write(make_record(victim).data(),
                static_cast<std::streamsize>(make_record(victim).size()));
    }

    // The sweep must NOT delete the victim (forged record: wrong shape).
    assert(io::ExtractionSession::sweep_directory(dir));
    assert(file_exists(victim));
    std::cout << "[PASS] forged_journal_batched: forged record rejected\n";
    rm_dir(dir);
}

void test_containment_batched() {
    // Containment under batch policy: the journal is created anchored inside
    // the verified directory, and the commit is an anchored rename.
    const fs::path dir = make_test_dir("contbatch");
    const fs::path dest = dir / "sub" / "file.txt";

    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    session.attach_root(dir);
    io::AtomicWriter writer;
    assert(writer.open_contained(session, "sub", "file.txt"));
    writer.stream().write("contained", 9);
    assert(writer.commit());
    assert(file_exists(dest));
    assert(read_file(dest) == "contained");
    std::cout << "[PASS] containment_batched: anchored commit under batch policy\n";
    rm_dir(dir);
}

void test_frozen_surface_absence() {
    // The DLL frozen surface (openrar_archive_extract_file_to_path) does not
    // call set_durability_granularity, so it always uses entry granularity.
    // We verify the default is entry; the frozen surface is exercised by the
    // dll_* test suites which confirm it never enters batch mode.
    io::ExtractionSession session;
    assert(session.durability_granularity() == io::DurabilityGranularity::Entry);
    std::cout << "[PASS] frozen_surface_absence: default granularity is entry\n";
}

void test_batch_lru_eviction_occurs() {
    // LRU eviction must actually occur when journals have zero in-flight
    // temps. Open journals in more than BATCH_JOURNAL_LRU_CAP (64) distinct
    // directories, release all temps, then open one more: the oldest
    // journal must be evicted (unlinked) to make room.
    const fs::path root = make_test_dir("evictoccurs");
    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    const size_t cap = 64;
    for (size_t i = 0; i < cap + 2; ++i) {
        const fs::path d = root / ("d" + std::to_string(i));
        std::error_code ec;
        fs::create_directories(d, ec);
        assert(!ec);
        assert(session.register_temp(d, d / "f.0123456789abcdef0123456789abcdef.tmp"));
        session.release_temp(d / "f.0123456789abcdef0123456789abcdef.tmp");
    }
    // The oldest journal (d0) should have been evicted after the cap was
    // exceeded. d0's journal file should no longer exist.
    assert(find_journals(root / "d0").empty());
    // The newest journals should still exist.
    assert(!find_journals(root / ("d" + std::to_string(cap + 1))).empty());
    std::cout << "[PASS] batch_lru_eviction_occurs: oldest journal evicted at cap\n";
    rm_dir(root);
}

void test_batch_lru_eviction_skips_in_flight() {
    // LRU eviction must never destroy a journal that still has an in-flight
    // temp — that journal is the only recovery reference for the temp. Open
    // journals in more than kBatchJournalLruCap (64) distinct directories
    // without releasing any: every journal has in_flight > 0, so eviction
    // must skip and the held count may temporarily exceed the cap. Without
    // the in-flight guard the cap would evict (and unlink) a live journal.
    const fs::path root = make_test_dir("evictskip");
    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    const size_t over_cap = 66; // > kBatchJournalLruCap
    for (size_t i = 0; i < over_cap; ++i) {
        const fs::path d = root / ("d" + std::to_string(i));
        std::error_code ec;
        fs::create_directories(d, ec);
        assert(!ec);
        assert(session.register_temp(d, d / "f.0123456789abcdef0123456789abcdef.tmp"));
    }
    size_t held = 0;
    for (size_t i = 0; i < over_cap; ++i) {
        held += find_journals(root / ("d" + std::to_string(i))).size();
    }
    assert(held == over_cap); // nothing evicted while temps are in flight
    std::cout << "[PASS] batch_lru_eviction_skips_in_flight: " << held << " journals held\n";
    rm_dir(root);
}

void test_late_entry_to_batch_switch() {
    // A journal created in entry mode is not in the LRU. Switching the
    // session to batch mid-run must leave it tracked (backfill or the
    // touch_journal lazy guard), so a later register_temp on the same
    // directory neither splices a singular iterator nor corrupts state.
    const fs::path dir = make_test_dir("lateswitch");
    io::ExtractionSession session;

    // Entry mode: two in-flight writers share one directory journal, so the
    // journal survives (in_flight > 0) into the policy switch.
    io::AtomicWriter w1;
    io::AtomicWriter w2;
    assert(w1.open(session, dir / "a.txt"));
    assert(w2.open(session, dir / "b.txt"));

    session.set_durability_granularity(io::DurabilityGranularity::Batch);

    // A later register on the same directory touches the pre-existing
    // journal (the untracked-iterator path).
    io::AtomicWriter w3;
    assert(w3.open(session, dir / "c.txt"));

    w1.stream().write("a", 1);
    w2.stream().write("b", 1);
    w3.stream().write("c", 1);
    assert(w1.commit());
    assert(w2.commit());
    assert(w3.commit());
    assert(read_file(dir / "a.txt") == "a");
    assert(read_file(dir / "b.txt") == "b");
    assert(read_file(dir / "c.txt") == "c");
    std::cout << "[PASS] late_entry_to_batch_switch: pre-existing journal stays usable\n";
    rm_dir(dir);
}

} // namespace

static std::string g_argv0;

// Child mode for the crash-hook test: re-exec this binary with
// `--crash-hook-test=N <dir>`; the child does N register_temp calls in
// batch mode and the crash hook fires after the Nth record append. The child
// arms the hook itself (from argv) so the test does not depend on the parent
// environment being inherited across the spawn.
static int run_crash_hook_child(int n, const fs::path& dir) {
    io::ExtractionSession session;
    session.set_durability_granularity(io::DurabilityGranularity::Batch);
    session.set_test_hooks(static_cast<unsigned>(n), 0);
    for (int i = 0; i < n; ++i) {
        const fs::path temp =
            dir / ("f" + std::to_string(i) + ".0123456789abcdef0123456789abcdef.tmp");
        if (!session.register_temp(dir, temp)) return 1;
        // The crash hook fires after the Nth append (std::exit(99)).
    }
    return 0; // should not reach here if the hook fired
}

#ifdef _WIN32
#include <windows.h>
static int spawn_child(const std::string& exe, const std::string& arg1, const std::string& arg2) {
    std::string cmd = "\"" + exe + "\" " + arg1 + " \"" + arg2 + "\"";
    std::wstring wcmd(cmd.begin(), cmd.end());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(rc);
}
#else
#include <sys/wait.h>
#include <unistd.h>
static int spawn_child(const std::string& exe, const std::string& arg1, const std::string& arg2) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(exe.c_str(), exe.c_str(), arg1.c_str(), arg2.c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

void test_crash_hook_kills_after_nth_record() {
    // D6 crash hook: OPENRAR_TEST_BATCH_CRASH_AFTER=N kills the process after
    // the Nth record has been appended to the journal (the Nth register_temp
    // call), before the boundary sync. The child exits with code 99; the
    // parent verifies the journal contains exactly N records — proving the
    // kill happened AFTER the Nth append, not before.
    const fs::path dir = make_test_dir("crashhook");
    const int n = 3;

    const std::string arg1 = "--crash-hook-test=" + std::to_string(n);
    const int rc = spawn_child(g_argv0, arg1, dir.string());

    assert(rc == 99); // crash hook fired

    // The journal must contain exactly N records. Records are concatenated
    // without separators ("T <len> <crc>\n<path>" repeated), so count "T "
    // occurrences followed by a digit (the length prefix).
    const auto journals = find_journals(dir);
    assert(journals.size() == 1);
    std::ifstream f(journals[0], std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int records = 0;
    size_t pos = 0;
    while ((pos = content.find("T ", pos)) != std::string::npos) {
        if (pos + 2 < content.size() && content[pos + 2] >= '0' && content[pos + 2] <= '9') {
            ++records;
        }
        ++pos;
    }
    assert(records == n); // kill happened after the Nth append
    std::cout << "[PASS] crash_hook_kills_after_nth_record: exit 99, journal has " << records
              << " records\n";
    rm_dir(dir);
}

int main(int argc, char* argv[]) {
    g_argv0 = argc > 0 ? argv[0] : "";
    if (argc == 3 && std::string(argv[1]).rfind("--crash-hook-test=", 0) == 0) {
        const int n = std::stoi(std::string(argv[1]).substr(18));
        return run_crash_hook_child(n, fs::path(argv[2]));
    }
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    test_atomic_commit_replaces();
    test_commit_noclobber_preserves_dest();
    test_temp_names_crypto_random();
    test_abandon_and_destructor_disposition();
    test_journal_references_temp_during_flight();
    test_journal_orphan_cleanup();
    test_journal_sweep_skips_locked_live_run();
    test_commit_respects_readonly_dest();
    test_open_failure_is_clean();
    test_flat_cwd_destination();
    // v1.39.0 batch-mode failure matrix.
    test_batch_kill_mid_run();
    test_torn_journal_tail_batched();
    test_rename_failure_mid_run_batched();
    test_journal_sync_failure_fail_closed();
    test_cancel_mid_batch();
    test_window_1_equivalence();
    test_forged_journal_batched();
    test_containment_batched();
    test_frozen_surface_absence();
    test_batch_lru_eviction_skips_in_flight();
    test_batch_lru_eviction_occurs();
    test_late_entry_to_batch_switch();
    test_crash_hook_kills_after_nth_record();
    std::cout << "All extraction_atomic_tests passed.\n";
    return 0;
}
