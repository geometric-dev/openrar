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
#else
#include <sys/stat.h>
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

} // namespace

int main() {
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
    std::cout << "All extraction_atomic_tests passed.\n";
    return 0;
}
