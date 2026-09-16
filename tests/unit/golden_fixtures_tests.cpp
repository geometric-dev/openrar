// Golden-fixture coverage: exercises the header/reader paths against
// archives that were NOT produced by this codebase — the checked-in
// tests/*.rar files. This is the only unit-level guarantee that the
// reader accepts bytes written by other tools.
//
//   hello5.rar    — hand-built RAR5 (make_archives.ps1), stored, 2 files.
//   hello5_p.rar  — file-data encrypted (-p secret), headers in clear.
//   hello5_hp.rar — headers + file data encrypted (-hp secret).
//   hello4.rar    — RAR4; read support is an explicit non-goal, so the
//                   contract here is graceful rejection (no crash, no
//                   misparse).
//
// Encrypted-entry CRC note (per RAR5 format specification): the header
// CRC32 field of encrypted entries does NOT hold the plaintext CRC, and
// is not verified on encrypted entries (openrar's test_entry mirrors this by
// skipping encrypted entries). Content correctness for the encrypted
// goldens is therefore proven by byte-comparing the decrypted data.bin
// against hello5.rar's CRC-verified copy of the same fixture file.
//
// Fixture location is injected by CMake as OPENRAR_SOURCE_DIR. The
// buffer-archive section is active in every configuration: when
// OPENRAR_INMEM_ARCHIVE=ON the symbols come from openrar_core, otherwise
// CMake compiles buffer_archive.cpp directly into this target.

#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/core/types.hpp"

#ifdef OPENRAR_INMEM_ARCHIVE
#include "../../src/archive/buffer_archive.hpp"
#endif

#include <cassert>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace openrar;

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

namespace {

const std::filesystem::path FIXTURES_DIR = std::filesystem::path(OPENRAR_SOURCE_DIR) / "tests";
const char* PASSWORD = "secret";
const std::string HELLO = "Hello from open-rar!\r\n";
constexpr size_t DATA_BIN_SIZE = 100 * 1024;

std::vector<core::byte> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    assert(f);
    return std::vector<core::byte>(std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>());
}

const archive::ArchiveEntry* find_entry(const archive::ArchiveReader& r, const std::string& name) {
    for (const auto& e : r.entries()) {
        if (e.header.file_name == name) return &e;
    }
    return nullptr;
}

std::filesystem::path out_dir() {
    std::filesystem::path d = std::filesystem::temp_directory_path() / "openrar_golden_fixtures";
    std::filesystem::create_directories(d);
    return d;
}

// Structural assertions. Oracle-produced goldens carry a QuickOpen
// service header that entries() surfaces (the CLI filters services), so
// the expected service count is a parameter.
struct Expected {
    const archive::ArchiveEntry* hello;
    const archive::ArchiveEntry* data;
};

Expected expect_files(archive::ArchiveReader& reader, size_t expected_services) {
    const auto& entries = reader.entries();
    size_t files = 0, services = 0;
    bool saw_qo = false;
    Expected e{nullptr, nullptr};
    for (const auto& en : entries) {
        if (en.header.is_service) {
            ++services;
            if (en.header.service_type == "QO") saw_qo = true;
            continue;
        }
        if (en.header.file_name == "hello.txt") e.hello = &en;
        if (en.header.file_name == "data.bin") e.data = &en;
        ++files;
    }
    assert(files == 2 && e.hello && e.data);
    assert(services == expected_services);
    if (expected_services > 0) assert(saw_qo);
    assert(e.hello->header.unp_size == HELLO.size());
    assert(e.data->header.unp_size == DATA_BIN_SIZE);
    return e;
}

// data.bin is the same fixture content in all three RAR5 goldens. hello5.rar
// stores it unencrypted with a verifiable header CRC, so its extraction is
// the reference plaintext for the encrypted goldens.
std::vector<core::byte> reference_data_bin() {
    archive::ArchiveReader reader;
    assert(reader.open(FIXTURES_DIR / "hello5.rar"));
    const archive::ArchiveEntry* data = find_entry(reader, "data.bin");
    assert(data);
    assert(reader.test_entry(*data)); // header CRC32 verified (unencrypted)

    std::filesystem::path out = out_dir() / "data_ref.bin";
    assert(reader.extract_entry(*data, out));
    std::vector<core::byte> ref = read_file(out);
    assert(ref.size() == DATA_BIN_SIZE);
    assert(crypto::crc32(ref.data(), ref.size()) == data->header.data_crc32);
    return ref;
}

void extract_and_verify_hello(archive::ArchiveReader& reader, const std::string& password) {
    std::filesystem::path out = out_dir() / "hello.txt";
    assert(reader.extract_entry(*find_entry(reader, "hello.txt"), out, password));
    std::vector<core::byte> hello = read_file(out);
    assert(std::string(hello.begin(), hello.end()) == HELLO);
}

void extract_and_verify_data(archive::ArchiveReader& reader, const std::string& password,
                             const std::vector<core::byte>& reference) {
    std::filesystem::path out = out_dir() / "data.bin";
    assert(reader.extract_entry(*find_entry(reader, "data.bin"), out, password));
    std::vector<core::byte> data = read_file(out);
    assert(data.size() == DATA_BIN_SIZE);
    assert(data == reference);
}

#ifdef OPENRAR_INMEM_ARCHIVE
void buffer_archive_checks(const std::filesystem::path& fixture) {
    std::vector<core::byte> bytes = read_file(fixture);
    archive::BufferArchive ba;
    std::vector<archive::BufferArchiveEntry> entries;
    assert(ba.list(bytes.data(), bytes.size(), entries) == archive::RAR_OK);
    assert(entries.size() == 2);
    assert(entries[0].path == "hello.txt");
    assert(entries[1].path == "data.bin");
    assert(entries[0].size == HELLO.size());
    assert(entries[1].size == DATA_BIN_SIZE);

    std::vector<core::byte> hello;
    assert(ba.extract(bytes.data(), bytes.size(), 0, hello) == archive::RAR_OK);
    assert(std::string(hello.begin(), hello.end()) == HELLO);
}
#endif

} // namespace

void test_golden_hello5() {
    std::filesystem::path arc = FIXTURES_DIR / "hello5.rar";
    archive::ArchiveReader reader;
    assert(reader.open(arc));
    Expected e = expect_files(reader, /*expected_services=*/0);
    assert(reader.test_entry(*e.hello));
    assert(reader.test_entry(*e.data));
    extract_and_verify_hello(reader, /*password=*/"");
    extract_and_verify_data(reader, "", reference_data_bin());

#ifdef OPENRAR_INMEM_ARCHIVE
    buffer_archive_checks(arc);
#else
    std::cout << "  (BufferArchive golden checks skipped: OPENRAR_INMEM_ARCHIVE=OFF)\n";
#endif
    std::cout << "[PASS] golden hello5.rar (hand-built RAR5)\n";
}

void test_golden_hello5_p() {
    std::filesystem::path arc = FIXTURES_DIR / "hello5_p.rar";

    // Headers are in the clear: listable without a password. The oracle
    // golden carries a QO service header.
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc));
        expect_files(reader, /*expected_services=*/1);
        // Data stays locked: extraction without a password must fail and
        // flag the bad password on the reader.
        const archive::ArchiveEntry* hello = find_entry(reader, "hello.txt");
        assert(hello);
        std::filesystem::path out = out_dir() / "p_nopwd.bin";
        assert(!reader.extract_entry(*hello, out));
        assert(reader.has_bad_password());
    }

    // Correct password: decrypted content must match the reference copy.
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc, PASSWORD));
        expect_files(reader, 1);
        extract_and_verify_hello(reader, PASSWORD);
        extract_and_verify_data(reader, PASSWORD, reference_data_bin());
    }
    std::cout << "[PASS] golden hello5_p.rar (file-data encryption)\n";
}

void test_golden_hello5_hp() {
    std::filesystem::path arc = FIXTURES_DIR / "hello5_hp.rar";

    // Header-encrypted: without a password the scan cannot even read the
    // headers — open must fail cleanly (BADPSW), never misparse.
    {
        archive::ArchiveReader reader;
        assert(!reader.open(arc));
    }

    // With the password: headers decrypt, content must match the reference.
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc, PASSWORD));
        expect_files(reader, 1);
        extract_and_verify_hello(reader, PASSWORD);
        extract_and_verify_data(reader, PASSWORD, reference_data_bin());
    }
    std::cout << "[PASS] golden hello5_hp.rar (header encryption)\n";
}

void test_golden_hello5_p_streaming_verify() {
    // Tweaked-checksum regression (0x0002, encryption extra): hello5_p was
    // built with the tweaked-checksum flag set, so its header data_crc32 is
    // key-dependent, NOT the plaintext CRC. The streaming verify path used to
    // compare it against the decoded plaintext and failed valid archives
    // with RAR_ERR_CRC_MISMATCH (fail-closed false failure). With the 0x0002
    // exception it must accept the entry — the PswCheck authenticated the key.
    std::filesystem::path arc = FIXTURES_DIR / "hello5_p.rar";
    archive::ArchiveReader reader;
    assert(reader.open(arc, PASSWORD));
    const archive::ArchiveEntry* hello = find_entry(reader, "hello.txt");
    assert(hello);
    assert(hello->header.is_encrypted);
    assert((hello->header.crypt_flags & 0x0002) != 0); // fixture really is tweaked
    const size_t hello_index =
        static_cast<size_t>(hello - reader.entries().data());
    archive::ReaderHooks hooks{};
    assert(reader.test_entry_stream(hello_index, hooks) == archive::RAR_OK);

    // Negative control on the SAME path: an archive whose writer stored the
    // plaintext CRC for an encrypted entry (our writer, crypt_flags 0x01)
    // must still be fully verified — corrupting its payload yields
    // RAR_ERR_CRC_MISMATCH, not a silent accept.
    namespace fs = std::filesystem;
    fs::path src = fs::temp_directory_path() / "openrar_tweak_src.bin";
    fs::path plain = fs::temp_directory_path() / "openrar_tweak_plain.rar";
    fs::path corrupt = fs::temp_directory_path() / "openrar_tweak_corrupt.rar";
    std::error_code ec;
    fs::remove(plain, ec);
    fs::remove(corrupt, ec);
    {
        std::ofstream f(src, std::ios::binary);
        f << std::string(2048, 'K') << "ENCRYPTED-CRC-CONTROL";
    }
    assert(archive::ArchiveMutator::add_file_to_archive(plain, src, "ctl.bin", 0, {}, 0,
                                                        PASSWORD));
    {
        archive::ArchiveReader probe;
        assert(probe.open(plain, PASSWORD));
        const archive::ArchiveEntry* e = find_entry(probe, "ctl.bin");
        assert(e && e->header.is_encrypted);
        assert((e->header.crypt_flags & 0x0002) == 0); // our writer: plaintext CRC
        std::ifstream in(plain, std::ios::binary);
        std::vector<core::byte> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        in.close();
        const core::uint64 flip = e->data_offset + e->data_size / 2;
        bytes[static_cast<size_t>(flip)] ^= 0xFF;
        std::ofstream out(corrupt, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    {
        archive::ArchiveReader broken;
        assert(broken.open(corrupt, PASSWORD));
        const archive::ArchiveEntry* e = find_entry(broken, "ctl.bin");
        assert(e);
        const size_t idx = static_cast<size_t>(e - broken.entries().data());
        archive::ReaderHooks hooks{};
        assert(broken.test_entry_stream(idx, hooks) == archive::RAR_ERR_CRC_MISMATCH);
    }
    fs::remove(src, ec);
    fs::remove(plain, ec);
    fs::remove(corrupt, ec);
    std::cout << "[PASS] golden hello5_p streaming verify (tweaked checksums accepted, "
                 "plaintext CRC still enforced)\n";
}

void test_golden_hello4_rejected() {
    std::filesystem::path arc = FIXTURES_DIR / "hello4.rar";
    archive::ArchiveReader reader;
    // RAR4 (7-byte signature) is not supported; the contract is a clean
    // rejection, not a crash or a misparse into garbage entries.
    assert(!reader.open(arc));
    assert(reader.entries().empty());

#ifdef OPENRAR_INMEM_ARCHIVE
    std::vector<core::byte> bytes = read_file(arc);
    archive::BufferArchive ba;
    std::vector<archive::BufferArchiveEntry> entries;
    assert(ba.list(bytes.data(), bytes.size(), entries) != archive::RAR_OK);
#endif
    std::cout << "[PASS] golden hello4.rar gracefully rejected (RAR4 non-goal)\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "Running golden-fixture tests...\n";
    test_golden_hello5();
    test_golden_hello5_p();
    test_golden_hello5_hp();
    test_golden_hello5_p_streaming_verify();
    test_golden_hello4_rejected();
    std::cout << "All golden-fixture tests PASSED!\n";
    return 0;
}
