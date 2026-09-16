#include "../../src/core/types.hpp"
#include "../../src/recovery/rs16.hpp"
#include "../../src/recovery/recovery_record.hpp"
#include "../../src/recovery/recovery_writer.hpp"
#include "../../src/crypto/crc64.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/headers.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

#include "test_support.hpp"
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;
using namespace openrar::recovery;

void test_gf_arithmetic() {
    ReedSolomon16 rs;

    // Zero element
    assert(rs.gf_mul(0, 12345) == 0);
    assert(rs.gf_mul(54321, 0) == 0);

    // Identity element
    assert(rs.gf_mul(1, 12345) == 12345);
    assert(rs.gf_mul(54321, 1) == 54321);

    // Inverse property: a * inv(a) == 1
    for (core::uint32 i = 1; i < 1000; ++i) {
        core::uint32 inv = rs.gf_inv(i);
        assert(rs.gf_mul(i, inv) == 1);
    }

    std::cout << "[PASS] Galois Field GF(65536) Arithmetic\n";
}

void test_rs16_codec_roundtrip() {
    const core::uint32 ND = 5; // 5 data sectors
    const core::uint32 NR = 3; // 3 parity sectors
    const size_t SECTOR_SIZE = 512;

    std::vector<core::byte> original_data(ND * SECTOR_SIZE);
    for (size_t i = 0; i < original_data.size(); ++i) {
        original_data[i] = static_cast<core::byte>((i * 37 + 11) & 0xFF);
    }

    // Generate parity
    std::vector<core::byte> parity(NR * SECTOR_SIZE, 0);
    {
        ReedSolomon16 encoder;
        bool ok = encoder.init(ND, NR);
        assert(ok);

        for (core::uint32 d = 0; d < ND; ++d) {
            const core::byte* d_ptr = original_data.data() + d * SECTOR_SIZE;
            for (core::uint32 r = 0; r < NR; ++r) {
                core::byte* p_ptr = parity.data() + r * SECTOR_SIZE;
                encoder.update_ecc(d, r, d_ptr, p_ptr, SECTOR_SIZE);
            }
        }
    }

    // Simulate corruption of 2 sectors (sector 1 and sector 3)
    std::vector<core::byte> damaged_data = original_data;
    std::memset(damaged_data.data() + 1 * SECTOR_SIZE, 0xDE, SECTOR_SIZE);
    std::memset(damaged_data.data() + 3 * SECTOR_SIZE, 0xAD, SECTOR_SIZE);

    std::vector<core::byte> valid_flags(ND + NR, 1);
    valid_flags[1] = 0; // sector 1 damaged
    valid_flags[3] = 0; // sector 3 damaged

    // Decode and repair
    {
        ReedSolomon16 decoder;
        bool ok = decoder.init(ND, NR, valid_flags.data());
        assert(ok);

        // Build proc data: valid data, or replacement valid recovery
        std::vector<const core::byte*> proc(ND);
        core::uint32 r_scan = ND;
        for (core::uint32 i = 0; i < ND; ++i) {
            if (valid_flags[i]) {
                proc[i] = damaged_data.data() + i * SECTOR_SIZE;
            } else {
                while (!valid_flags[r_scan]) r_scan++;
                proc[i] = parity.data() + (r_scan - ND) * SECTOR_SIZE;
                r_scan++;
            }
        }

        // Reconstruct broken sectors
        std::vector<core::byte> recon1(SECTOR_SIZE, 0);
        std::vector<core::byte> recon3(SECTOR_SIZE, 0);

        for (core::uint32 j = 0; j < ND; ++j) {
            decoder.update_ecc(j, 0, proc[j], recon1.data(), SECTOR_SIZE);
            decoder.update_ecc(j, 1, proc[j], recon3.data(), SECTOR_SIZE);
        }

        // Verify reconstructed sectors match original byte-for-byte
        assert(std::memcmp(recon1.data(), original_data.data() + 1 * SECTOR_SIZE, SECTOR_SIZE) ==
               0);
        assert(std::memcmp(recon3.data(), original_data.data() + 3 * SECTOR_SIZE, SECTOR_SIZE) ==
               0);
    }

    std::cout << "[PASS] Cauchy Reed-Solomon RS(ND=5, NR=3) Erasure Decoding\n";
}

void test_recovery_manager() {
    const size_t PROTECTED_SIZE = 8192; // 16 sectors of 512 bytes
    std::vector<core::byte> original(PROTECTED_SIZE);
    for (size_t i = 0; i < PROTECTED_SIZE; ++i) {
        original[i] = static_cast<core::byte>((i * 73 + 19) & 0xFF);
    }

    auto params = RecoveryManager::calculate_params(PROTECTED_SIZE, 10); // 10% parity
    assert(params.data_sectors == 16);
    assert(params.recovery_sectors >= 2);

    auto parity = RecoveryManager::generate_parity(original.data(), params);
    assert(parity.size() == params.recovery_sectors * params.sector_size);

    // Corrupt 2 sectors in original data
    std::vector<core::byte> working_copy = original;
    std::memset(working_copy.data() + 2 * 512, 0xFF, 512);
    std::memset(working_copy.data() + 7 * 512, 0xEE, 512);

    std::vector<core::byte> valid(params.data_sectors + params.recovery_sectors, 1);
    valid[2] = 0;
    valid[7] = 0;

    // Repair
    bool repaired = RecoveryManager::repair_data(working_copy.data(), parity.data(), params, valid);
    assert(repaired);

    // Verify 100% byte-identical restoration
    assert(std::memcmp(working_copy.data(), original.data(), PROTECTED_SIZE) == 0);

    // Verify unrecoverable damage exceeding parity capacity
    std::vector<core::byte> hopeless = valid;
    for (core::uint32 i = 0; i < params.recovery_sectors + 2; ++i) {
        hopeless[i] = 0;
    }
    bool should_fail =
        RecoveryManager::repair_data(working_copy.data(), parity.data(), params, hopeless);
    assert(!should_fail);

    std::cout << "[PASS] RecoveryManager In-Archive Repair & Capacity Limits\n";
}

// Regression (report H4/L4): calculate_params computed the sector count and
// the data*percent product in uint32. At protected_size = 2 TiB the 512-byte
// sector count is exactly 2^32 and truncated to 0 (then forced to 1), and the
// percent product wrapped — either way a wrong tiny geometry came back and
// generate_parity/repair_data wrote out of bounds with it. Oversized inputs
// must now yield zero sector counts (an explicit failure signal that
// rs16.init rejects), and repair_data must clamp inconsistent writeback
// lengths instead of underflowing into a huge memcpy.
void test_recovery_params_overflow() {
    // 2 TiB: ceil(ps/512) == 2^32 — the exact uint32 truncation point.
    auto too_big = RecoveryManager::calculate_params(1ULL << 41, 5);
    assert(too_big.data_sectors == 0);
    assert(too_big.recovery_sectors == 0);
    // generate_parity must refuse the failure signal instead of computing.
    assert(RecoveryManager::generate_parity(nullptr, too_big).empty());

    // percent = 2^32-1 must not wrap the data*percent product into a bogus
    // fitting geometry.
    auto huge_pct = RecoveryManager::calculate_params(65530ULL * 512, 0xFFFFFFFFu);
    assert(huge_pct.data_sectors == 0);
    assert(huge_pct.recovery_sectors == 0);

    // A geometry that exactly fills the RS16 capacity at 512-byte sectors
    // (62409 data + ceil(62409*5/100) = 3121 recovery = 65530) still works.
    auto ok = RecoveryManager::calculate_params(62409ULL * 512, 5);
    assert(ok.sector_size == 512);
    assert(ok.data_sectors == 62409);
    assert(ok.recovery_sectors == 3121);

    // repair_data with params inconsistent with the buffer: damaged sector 3
    // starts at offset 1536 > protected_size 1024 — the writeback clamp must
    // skip it, not compute a huge copy_len from (protected_size - offset)
    // underflow (old code crashed inside memcpy here).
    RecoveryParams bad;
    bad.sector_size = 512;
    bad.data_sectors = 4;
    bad.recovery_sectors = 3;
    bad.protected_size = 1024;
    std::vector<core::byte> data(1024, 0xAB);
    std::vector<core::byte> parity(3 * 512, 0xCD);
    std::vector<core::byte> valid = {1, 1, 1, 0, 1, 1, 1}; // sector 3 damaged
    assert(RecoveryManager::repair_data(data.data(), parity.data(), bad, valid));

    std::cout << "[PASS] RecoveryManager geometry overflow refused (H4/L4)\n";
}

// Regression (report M2): the prot_size field of an inline RR shard header
// drives file_bytes.resize() in RecoveryWriter::repair. The CRC-64 that
// "authenticates" shards is unkeyed, so a crafted record could declare
// prot_size ~ 2^40 and force a terabyte allocation (uncaught bad_alloc).
// repair must now enforce the writer's geometry invariant
// prot_size <= D*group_count and fail the record before any large resize.
// Shape: a real archive with a legit inline RR, then patch shard 0's
// prot_size to 1 TiB and re-forge the shard CRC-64 (offsets are shard-format
// constants: total_size 0x0C, crc64 0x04, prot_size 0x22).
void test_repair_rejects_hostile_prot_size() {
    std::error_code ec;
    const char* arc = "build/i6_rr.rar";
    std::filesystem::remove(arc, ec);
    // A real minimal RAR5 archive: signature + main + stored file + end.
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        using namespace openrar::format;
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));
        FileBlock fb;
        fb.file_name = "payload.bin";
        fb.unp_size = 4096;
        fb.pack_size = 4096;
        fb.method = 0;
        fb.win_size = 0;
        fb.has_crc32 = true;
        core::byte payload[4096];
        for (size_t i = 0; i < sizeof(payload); ++i)
            payload[i] = static_cast<core::byte>(i * 37 + 11);
        crypto::Crc32 hc;
        hc.update(payload, sizeof(payload));
        fb.data_crc32 = hc.get();
        assert(HeaderWriter::write_file_block(out, fb));
        assert(out.write(payload, sizeof(payload)) == sizeof(payload));
        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }
    assert(RecoveryWriter::add_recovery_record(arc, 5));

    // Control: the intact record still repairs (and repair is a no-op on an
    // undamaged archive).
    assert(RecoveryWriter::repair(arc));

    // Locate shard 0 via its magic and patch prot_size to 1 TiB.
    io::FileStream f;
    assert(f.open(arc, io::FileMode::ReadOnly));
    std::vector<core::byte> buf(static_cast<size_t>(f.size()));
    assert(f.read(buf.data(), buf.size()) == buf.size());
    f.close();
    static const core::byte MAGIC[4] = {'{', 'R', 'B', '}'};
    size_t shard = std::string::npos;
    for (size_t i = 0; i + 4 <= buf.size(); ++i) {
        if (std::memcmp(buf.data() + i, MAGIC, 4) == 0) {
            shard = i;
            break;
        }
    }
    assert(shard != std::string::npos);
    core::uint32 shard_size = core::read_le32(buf.data() + shard + 0x0C);
    assert(shard_size >= 0x40 && shard + shard_size <= buf.size());
    core::write_le64(buf.data() + shard + 0x22, 1ULL << 40);
    core::write_le64(buf.data() + shard + 0x04,
                     crypto::Crc64Xz::compute(buf.data() + shard + 0x0C, shard_size - 0x0C));
    {
        io::FileStream g;
        assert(g.open(arc, io::FileMode::CreateAlways));
        assert(g.write(buf.data(), buf.size()) == buf.size());
    }

    // Hostile record must be refused before any prot_size-sized allocation
    // (old code attempted resize(1 TiB) here and terminated on bad_alloc).
    assert(!RecoveryWriter::repair(arc));

    std::filesystem::remove(arc, ec);
    std::cout << "[PASS] repair refuses hostile prot_size (M2)\n";
}

// Envelope roundtrip contract: create a real archive, attach an inline RR,
// corrupt one byte of file data, repair, extract a byte-identical payload.
//
// KNOWN ISSUE (currently red at HEAD, also reproducible via the CLI alone
// and matching tools/tests/recovery.tests.mjs failures): repair() reports
// success ("OK (RR structure verified)") but the flipped byte survives
// extraction — no damage is actually restored. The .mjs suite additionally
// fails the -rr5% creation/structure check. Until RecoveryWriter is fixed,
// this test reports the gap as a visible [KNOWN-ISSUE] instead of asserting;
// the final block must become a hard memcmp assert once repair works.
void test_recovery_writer_damage_roundtrip() {
    namespace fs = std::filesystem;
    fs::path dir = openrar::test::scratch_dir("recovery");
    fs::path src = dir / "payload.bin";
    fs::path arc = dir / "protected.rar";
    fs::path out = dir / "extracted.bin";

    constexpr size_t PAYLOAD_SIZE = 64 * 1024;
    std::vector<core::byte> payload(PAYLOAD_SIZE);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<core::byte>((i * 91 + 7) & 0xFF);
    }
    {
        io::FileStream f;
        assert(f.open(src, io::FileMode::CreateAlways));
        assert(f.write(payload.data(), payload.size()) == payload.size());
    }

    assert(archive::ArchiveMutator::add_file_to_archive(arc, src, "payload.bin",
                                                        /*method=*/0));
    assert(recovery::RecoveryWriter::add_recovery_record(arc, 5));

    // The stored file data starts at archive offset 67 (signature + main +
    // file header). Archive offset 100 flips payload byte 33.
    {
        io::FileStream f;
        assert(f.open(arc, io::FileMode::ReadWrite));
        f.seek(100, io::SeekOrigin::Begin);
        core::byte b = 0;
        assert(f.read(&b, 1) == 1);
        b = static_cast<core::byte>(b ^ 0xFF);
        f.seek(100, io::SeekOrigin::Begin);
        assert(f.write(&b, 1) == 1);
    }

    assert(recovery::RecoveryWriter::repair(arc));

    archive::ArchiveReader reader;
    assert(reader.open(arc));
    // entries() surfaces service headers too (the RR block sits in the
    // archive like the QO service in oracle-produced goldens); assert on
    // the single real file entry.
    size_t file_entries = 0;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service) continue;
        ++file_entries;
        assert(e.header.file_name == "payload.bin");
    }
    assert(file_entries == 1);
    assert(reader.extract_entry(reader.entries()[0], out));
    reader.close();
    io::FileStream f;
    assert(f.open(out, io::FileMode::ReadOnly));
    std::vector<core::byte> restored(static_cast<size_t>(f.size()));
    assert(f.read(restored.data(), restored.size()) == restored.size());

    const bool restored_ok = restored.size() == payload.size() &&
                             std::memcmp(restored.data(), payload.data(), payload.size()) == 0;
    assert(restored_ok);
    std::cout << "[PASS] RecoveryWriter damage -> repair -> byte-identical extract\n";
}

// Regression (report M2, splice contract): RecoveryWriter::repair() used to
// hand splice_repair() the whole-file buffer although splice_repair requires
// exactly the protected prefix, so the reconstruction path (missing_data > 0)
// always failed and any archive genuinely needing shard rebuild was
// unrepairable. Drive that path with a *plausible* geometry: patch shard 0's
// prot_size from the writer's exact prefix length up to the padded
// D*group_count (re-forging the shard CRC-64), making the partial last data
// shard count as erased. Reconstruction must rebuild it, the splice must
// write back exactly [0, header_offset), and repair must succeed with a
// byte-identical payload on extraction.
void test_repair_reconstructs_padded_prefix() {
    namespace fs = std::filesystem;
    fs::path arc = "build/i6_rr_pad.rar";
    std::error_code ec;
    fs::remove(arc, ec);
    constexpr size_t PAYLOAD_SIZE = 64 * 1024; // yields header_offset % group_count != 0
    {
        io::FileStream out_str;
        assert(out_str.open(arc, io::FileMode::CreateAlways));
        using namespace openrar::format;
        assert(HeaderWriter::write_signature(out_str));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out_str, mb));
        FileBlock fb;
        fb.file_name = "payload.bin";
        fb.unp_size = PAYLOAD_SIZE;
        fb.pack_size = PAYLOAD_SIZE;
        fb.method = 0;
        fb.win_size = 0;
        fb.has_crc32 = true;
        std::vector<core::byte> payload(PAYLOAD_SIZE);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<core::byte>(i * 91 + 7);
        crypto::Crc32 hc;
        hc.update(payload.data(), payload.size());
        fb.data_crc32 = hc.get();
        assert(HeaderWriter::write_file_block(out_str, fb));
        assert(out_str.write(payload.data(), payload.size()) == payload.size());
        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out_str, eb));
    }
    assert(RecoveryWriter::add_recovery_record(arc, 5));

    // Locate shard 0 and patch prot_size up to D*group_count. group_count is
    // the writer's ceil(prefix/D) rounded to even; read it from the record
    // itself (SHARD_GROUP_CNT = 0x2A) rather than recomputing.
    io::FileStream f;
    assert(f.open(arc, io::FileMode::ReadOnly));
    std::vector<core::byte> buf(static_cast<size_t>(f.size()));
    assert(f.read(buf.data(), buf.size()) == buf.size());
    f.close();
    static const core::byte MAGIC[4] = {'{', 'R', 'B', '}'};
    size_t shard = std::string::npos;
    for (size_t i = 0; i + 4 <= buf.size(); ++i) {
        if (std::memcmp(buf.data() + i, MAGIC, 4) == 0) {
            shard = i;
            break;
        }
    }
    assert(shard != std::string::npos);
    core::uint32 shard_size = core::read_le32(buf.data() + shard + 0x0C);
    core::uint64 group_count = core::read_le64(buf.data() + shard + 0x2A);
    core::uint32 header_size32 = core::read_le32(buf.data() + shard + 0x10);
    core::uint64 D = (header_size32 - 0x48) / 8; // writer: header_size = (D*8 + 0x48)*scale
    assert(D >= 1 && D <= 200);
    core::uint64 padded = D * group_count;
    core::write_le64(buf.data() + shard + 0x22, padded);
    core::write_le64(buf.data() + shard + 0x04,
                     crypto::Crc64Xz::compute(buf.data() + shard + 0x0C, shard_size - 0x0C));
    {
        io::FileStream g;
        assert(g.open(arc, io::FileMode::CreateAlways));
        assert(g.write(buf.data(), buf.size()) == buf.size());
    }

    // The padded-but-plausible record must now take the reconstruction path
    // and succeed end to end. Two writer bugs had to be fixed for this to
    // hold: the splice size contract (report M2) and the main-header locator
    // patch that used to invalidate already-folded parity bytes.
    assert(RecoveryWriter::repair(arc));

    archive::ArchiveReader reader;
    assert(reader.open(arc));
    bool extracted = false;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service) continue;
        assert(e.header.file_name == "payload.bin");
        std::vector<core::byte> got;
        assert(reader.read_packed_data(e, got));
        assert(got.size() == PAYLOAD_SIZE);
        for (size_t i = 0; i < PAYLOAD_SIZE; ++i)
            assert(got[i] == static_cast<core::byte>(i * 91 + 7));
        extracted = true;
    }
    reader.close();
    assert(extracted);
    fs::remove(arc, ec);
    std::cout
        << "[PASS] repair reconstructs padded prefix and splices byte-identical output (M2)\n";
}

// Parity bytes must not depend on how many threads folded them: add RR with
// 1 and 4 threads onto copies of the same base archive and require the
// results to be byte-identical (RR shards are geometry-derived and carry no
// timestamps, so a full-file comparison is exact).
void test_rr_thread_determinism() {
    namespace fs = std::filesystem;
    fs::path base = "build/rr_mt_base.rar";
    fs::path a1 = "build/rr_mt_t1.rar";
    fs::path a4 = "build/rr_mt_t4.rar";
    fs::path src = "build/rr_mt_src.bin";
    std::error_code ec;
    fs::remove(base, ec);
    fs::remove(a1, ec);
    fs::remove(a4, ec);
    fs::remove(src, ec);

    {
        std::ofstream f(src, std::ios::binary);
        std::mt19937 rng(1234);
        for (int i = 0; i < 96; ++i) {
            std::string block(4096, '\0');
            for (char& c : block) c = static_cast<char>(rng() & 0xFF);
            f.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(base, src, "payload.bin"));

    fs::copy_file(base, a1, fs::copy_options::overwrite_existing);
    fs::copy_file(base, a4, fs::copy_options::overwrite_existing);
    assert(RecoveryWriter::add_recovery_record(a1, 10, 1));
    assert(RecoveryWriter::add_recovery_record(a4, 10, 4));

    std::ifstream f1(a1, std::ios::binary), f4(a4, std::ios::binary);
    std::string d1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    std::string d4((std::istreambuf_iterator<char>(f4)), std::istreambuf_iterator<char>());
    assert(d1.size() == d4.size());
    assert(d1 == d4);

    // Both records must still verify.
    assert(RecoveryWriter::repair(a1));
    assert(RecoveryWriter::repair(a4));

    fs::remove(base, ec);
    fs::remove(a1, ec);
    fs::remove(a4, ec);
    fs::remove(src, ec);
    std::cout << "[PASS] RR parity byte-identical across 1 vs 4 fold threads\n";
}

void test_b10_calculate_parity_buffer_size() {
    using namespace openrar::recovery;
    // B10 overflow test: 65536 * 1048576 = 68719476736 (64 GiB) > 2 GiB cap
    auto overflow = calculate_parity_buffer_size(65536, 1048576);
    assert(!overflow.has_value());

    // Zero checks
    assert(!calculate_parity_buffer_size(0, 512).has_value());
    assert(!calculate_parity_buffer_size(512, 0).has_value());
    assert(!calculate_parity_buffer_size(0, 0).has_value());

    // Valid sizes within 2 GiB cap
    auto valid_small = calculate_parity_buffer_size(10, 512);
    assert(valid_small.has_value());
    assert(valid_small.value() == 5120);

    auto valid_max = calculate_parity_buffer_size(2048, 1024 * 1024); // 2 GiB exactly
    assert(valid_max.has_value());
    assert(valid_max.value() == 2ULL * 1024 * 1024 * 1024);

    auto over_cap = calculate_parity_buffer_size(2049, 1024 * 1024); // > 2 GiB
    assert(!over_cap.has_value());

    std::cout << "[PASS] calculate_parity_buffer_size overflow and bounds checks\n";
}

void test_b5_parity_only_corruption() {
    namespace fs = std::filesystem;
    fs::path dir = openrar::test::scratch_dir("recovery");
    fs::path src = dir / "p_only_src.bin";
    fs::path arc = dir / "p_only.rar";
    fs::path out = dir / "p_only_out.bin";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove(out, ec);

    constexpr size_t PAYLOAD_SIZE = 64 * 1024;
    std::vector<core::byte> payload(PAYLOAD_SIZE);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<core::byte>((i * 17 + 3) & 0xFF);
    }
    {
        io::FileStream f;
        assert(f.open(src, io::FileMode::CreateAlways));
        assert(f.write(payload.data(), payload.size()) == payload.size());
    }

    assert(archive::ArchiveMutator::add_file_to_archive(arc, src, "p_only.bin", /*method=*/0));
    assert(recovery::RecoveryWriter::add_recovery_record(arc, 5));

    // Confirm that the newly created archive entries are completely healthy
    {
        archive::ArchiveReader test_r;
        assert(test_r.open(arc));
        assert(test_r.test_entry(test_r.entries()[0]));
    }

    // Corrupt shard 0's stored CRC64 so it fails CRC validation while file data remains untouched
    {
        io::FileStream f;
        assert(f.open(arc, io::FileMode::ReadWrite));
        std::vector<core::byte> arc_bytes(static_cast<size_t>(f.size()));
        assert(f.read(arc_bytes.data(), arc_bytes.size()) == arc_bytes.size());

        static const core::byte MAGIC[4] = {'{', 'R', 'B', '}'};
        size_t shard_pos = std::string::npos;
        for (size_t i = 0; i + 4 <= arc_bytes.size(); ++i) {
            if (std::memcmp(arc_bytes.data() + i, MAGIC, 4) == 0) {
                shard_pos = i;
                break;
            }
        }
        assert(shard_pos != std::string::npos);

        f.seek(static_cast<core::int64>(shard_pos + 4), io::SeekOrigin::Begin);
        core::byte bad_crc[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};
        assert(f.write(bad_crc, 8) == 8);
    }

    // Repair should recognize that file data is 100% healthy, recompute parity, and rewrite RR
    assert(recovery::RecoveryWriter::repair(arc));

    // Verify extraction works byte-identically
    archive::ArchiveReader reader;
    assert(reader.open(arc));
    assert(reader.extract_entry(reader.entries()[0], out));
    reader.close();

    io::FileStream f;
    assert(f.open(out, io::FileMode::ReadOnly));
    std::vector<core::byte> restored(static_cast<size_t>(f.size()));
    assert(f.read(restored.data(), restored.size()) == restored.size());
    assert(restored.size() == payload.size());
    assert(std::memcmp(restored.data(), payload.data(), payload.size()) == 0);

    // Verify that the repaired archive's RR is now valid
    assert(recovery::RecoveryWriter::repair(arc));

    std::cout << "[PASS] Parity-only corruption: recomputed and restored cleanly\n";
}

void test_b5_unrecoverable_inconsistent_corruption() {
    namespace fs = std::filesystem;
    fs::path dir = openrar::test::scratch_dir("recovery");
    fs::path src = dir / "unrec_src.bin";
    fs::path arc = dir / "unrec.rar";

    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(arc, ec);

    constexpr size_t PAYLOAD_SIZE = 32 * 1024;
    std::vector<core::byte> payload(PAYLOAD_SIZE, 0xAA);
    {
        io::FileStream f;
        assert(f.open(src, io::FileMode::CreateAlways));
        assert(f.write(payload.data(), payload.size()) == payload.size());
    }

    assert(archive::ArchiveMutator::add_file_to_archive(arc, src, "unrec.bin", /*method=*/0));
    assert(recovery::RecoveryWriter::add_recovery_record(arc, 3));

    // Corrupt bytes across multiple widely separated shards exceeding recovery capacity
    {
        io::FileStream f;
        assert(f.open(arc, io::FileMode::ReadWrite));
        for (int k = 0; k < 10; ++k) {
            f.seek(100 + k * 2000, io::SeekOrigin::Begin);
            core::byte b = 0x55;
            (void)f.write(&b, 1);
        }
    }

    // Repair must return false instead of misreconstructing corrupt data
    assert(!recovery::RecoveryWriter::repair(arc));

    std::cout << "[PASS] Unrecoverable damage safely rejected without corrupting archive\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "Running Clean-Room Milestone 4 Recovery Verification...\n";
    test_gf_arithmetic();
    test_rs16_codec_roundtrip();
    test_recovery_manager();
    test_recovery_params_overflow();
    test_repair_rejects_hostile_prot_size();
    test_repair_reconstructs_padded_prefix();
    test_recovery_writer_damage_roundtrip();
    test_rr_thread_determinism();
    test_b10_calculate_parity_buffer_size();
    test_b5_parity_only_corruption();
    test_b5_unrecoverable_inconsistent_corruption();
    std::cout << "All Milestone 4 Recovery & Reed-Solomon Primitives PASSED!\n";
    return 0;
}
