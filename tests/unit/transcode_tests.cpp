// Transcode (`cv`) tests — v1.29 M5 (plan §5 named negatives):
// cv_zip_roundtrip_identity, cv_vs_a_differential_control,
// df_delete_only_after_verified_roundtrip, df_no_delete_on_partial,
// cv_exit_taxonomy rows, cv_switch_parity, cv_json_summary_v2_fields,
// cv_non_tty_zero_escape_bytes, cv_staging_cleanup_on_abort,
// cv_dest_equals_source_refused, cv_legacy_rar_refused.
// CLI-level rows run the real binary via OPENRAR_CLI_EXE (the
// extraction_report_tests pattern).
#include "../../src/archive/foreign_reader.hpp"
#include "../../src/archive/foreign_transcode.hpp"
#include "../../src/archive/foreign_zip.hpp"
#include "../../src/archive/foreign_tar.hpp"
#include "../../src/archive/foreign_gzip.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/compress/inflate.hpp"
#include "../../src/crypto/blake2sp.hpp"
#include "../../src/crypto/crc32.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "test_support.hpp"

using namespace openrar;
using namespace openrar::archive;
using namespace openrar::archive::foreign;

#include "deflate_kats.inc"
#include "foreign_fixtures.inc"

#ifdef OPENRAR_CLI_EXE
#include <cstdlib>
static std::string cli_path() {
    return OPENRAR_CLI_EXE;
}
// Quote-free command per the cli_tests convention: cmd /c strips the
// outer quote pair, so paths here must be space-free (build + temp dirs).
static int run_cli(const std::string& args, const std::string& redirect) {
    const std::string cmd = cli_path() + " " + args + " " + redirect;
    const int raw = std::system(cmd.c_str());
#ifdef _WIN32
    return static_cast<int>(static_cast<unsigned>(raw) & 0xFF); // wait status
#else
    return raw;
#endif
}
#endif

namespace {

// Deterministic input refiller over a byte vector (fixture decode use).
struct VecInput {
    const std::vector<core::byte>* data;
    size_t pos = 0;
    size_t operator()(core::byte* buf, size_t max_size) {
        size_t left = data->size() - pos;
        size_t take = left < max_size ? left : max_size;
        for (size_t i = 0; i < take; ++i) buf[i] = (*data)[pos + i];
        pos += take;
        return take;
    }
};

void write_bytes(const std::filesystem::path& p, const std::vector<core::byte>& data) {
    std::ofstream f(p, std::ios::binary);
    assert(f);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    assert(f);
}

std::vector<core::byte> read_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<core::byte>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

foreign::TranscodeOptions base_opts() {
    foreign::TranscodeOptions o;
    o.limits.max_member_output_bytes = 16ULL * 1024 * 1024 * 1024;
    o.limits.max_total_output_bytes = 1024ULL * 1024 * 1024 * 1024;
    o.limits.max_header_count = 1000000;
    o.limits.max_header_bytes = 256ULL * 1024 * 1024;
    return o;
}

// The differential control (plan §0 leg d): payload bytes extracted from the
// cv output equal the source payloads — asserted through a fresh reader.
void verify_output(const std::filesystem::path& arc,
                   const std::vector<std::vector<core::byte>>& want_payloads) {
    ArchiveReader r;
    assert(r.open(arc));
    std::vector<size_t> payload_idx;
    for (size_t i = 0; i < r.entries().size(); ++i) {
        if (r.entries()[i].header.is_service) continue;
        if (r.entries()[i].header.pack_size < 0) continue;
        payload_idx.push_back(i);
    }
    assert(payload_idx.size() == want_payloads.size());
    for (size_t i = 0; i < payload_idx.size(); ++i) {
        std::vector<core::byte> out;
        assert(r.extract_entry_to_memory(payload_idx[i], out, ~core::uint64(0), ReaderHooks{}) ==
               0);
        assert(out == want_payloads[i]);
    }
}


// Decodes the RLE KAT stream to its plaintext (the fixture pins the
// compressed bytes + length + CRC, not the raw payload).
std::vector<core::byte> decode_kat_rle() {
    VecInput vin{&kat_rle_stream};
    compress::Inflate inf;
    std::vector<core::byte> plain;
    auto sink = [&](const core::byte* d, size_t n) {
        plain.insert(plain.end(), d, d + n);
        return true;
    };
    assert(inf.decode(vin, sink) == compress::InflateError::Ok);
    assert(plain.size() == kat_rle_plain_len);
    return plain;
}

} // namespace

static void test_transcode_zip_identity() {
    std::cout << "[+] test_transcode_zip_identity" << std::endl;
    const auto dir = openrar::test::scratch_dir("transcode");

    core::uint32 crc_txt = 0;
    const auto txt = stored_payload("migrate me\n", crc_txt);
    core::uint32 crc_bin = 0;
    const auto bin = stored_payload(std::string(50000, '\0'), crc_bin);

    std::vector<ZipMember> members;
    ZipMember a;
    a.name = "docs/readme.txt";
    a.flags = 0x0800;
    a.packed = txt;
    a.crc = crc_txt;
    a.unp_size = txt.size();
    members.push_back(a);
    ZipMember b;
    b.name = "data.bin";
    b.flags = 0x0800;
    b.packed = bin;
    b.crc = crc_bin;
    b.unp_size = bin.size();
    members.push_back(b);
    ZipMember d;
    d.name = "emptydir/";
    d.flags = 0x0800;
    d.dos_attrs = 0x10;
    members.push_back(d);
    ZipMember big;
    big.name = "kat.bin";
    big.flags = 0x0800;
    big.method = 8;
    big.packed = kat_rle_stream;
    big.crc = kat_rle_plain_crc;
    big.unp_size = kat_rle_plain_len;
    members.push_back(big);

    const auto src = dir / "src.zip";
    write_bytes(src, build_zip(members, "archive comment"));

    TranscodeOptions opts = base_opts();
    TranscodeResult result;
    ExtractionReport report;
    ReaderHooks hooks;
    const ForeignStatus st = transcode(src, dir / "out.rar", opts, result, report, hooks);
    assert(st == ForeignStatus::Ok && result.detail.c_str());
    assert(result.verified);
    assert(result.format == "zip");
    assert(result.migrated_files == 3 && result.migrated_dirs == 1);
    assert(result.archive_comment_migrated);
    assert(result.skipped == 0);

    // Bit-for-bit extraction equivalence (store + deflate + dir + comment).
    verify_output(dir / "out.rar", {txt, bin, decode_kat_rle()});
    std::cout << "    - zip -> rar5 identity: 3 files + dir, comment migrated, verified"
              << std::endl;
}

static void test_transcode_tar_gzip_identity() {
    std::cout << "[+] test_transcode_tar_gzip_identity" << std::endl;
    const auto dir = openrar::test::scratch_dir("transcode");

    std::vector<TarMember> members;
    TarMember t;
    t.name = "a/hello.txt";
    t.data = std::vector<core::byte>(2000, core::byte('T'));
    members.push_back(t);
    TarMember d;
    d.name = "a";
    d.typeflag = '5';
    d.mode = 0755;
    members.push_back(d);
    const auto tar_src = dir / "src.tar";
    write_bytes(tar_src, build_tar(members));

    TranscodeOptions opts = base_opts();
    TranscodeResult result;
    ExtractionReport report;
    const ForeignStatus st =
        transcode(tar_src, dir / "tar_out.rar", opts, result, report, ReaderHooks{});
    assert(st == ForeignStatus::Ok && result.detail.c_str());
    assert(result.verified && result.format == "tar");
    verify_output(dir / "tar_out.rar", {t.data});

    // GZIP: one logical entry, concatenated members.
    const auto gz_src = dir / "src.gz";
    write_bytes(gz_src, build_gzip({&kat_text_stream, &kat_rle_stream},
                                   {kat_text_plain_crc, kat_rle_plain_crc},
                                   {static_cast<core::uint32>(kat_text_plain_len),
                                    static_cast<core::uint32>(kat_rle_plain_len)}));
    TranscodeResult gz_result;
    ExtractionReport gz_report;
    const ForeignStatus gst =
        transcode(gz_src, dir / "gz_out.rar", opts, gz_result, gz_report, ReaderHooks{});
    assert(gst == ForeignStatus::Ok && gz_result.detail.c_str());
    assert(gz_result.verified && gz_result.format == "gzip");
    std::vector<core::byte> want(kat_text_plain.begin(), kat_text_plain.end());
    const std::vector<core::byte> rle_plain = decode_kat_rle();
    want.insert(want.end(), rle_plain.begin(), rle_plain.end()); // member concatenation
    verify_output(dir / "gz_out.rar", {want});
    std::cout << "    - tar + gzip -> rar5 identity verified" << std::endl;
}

static void test_transcode_refusals_and_collisions() {
    std::cout << "[+] test_transcode_refusals_and_collisions" << std::endl;
    const auto dir = openrar::test::scratch_dir("transcode");

    // RAR5 source: usage refusal.
    {
        const auto src = dir / "r5.rar";
        write_bytes(src, std::vector<core::byte>{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00});
        TranscodeOptions opts = base_opts();
        TranscodeResult result;
        ExtractionReport report;
        const ForeignStatus st = transcode(src, dir / "o.rar", opts, result, report, ReaderHooks{});
        assert(st == ForeignStatus::Unparseable && result.usage_refused);
    }
    // Legacy RAR: explicit refusal (never "not an archive").
    {
        const auto src = dir / "legacy.rar";
        write_bytes(src, std::vector<core::byte>{'R', 'a', 'r', '!', 0x1a, 0x07, 0x00});
        TranscodeOptions opts = base_opts();
        TranscodeResult result;
        ExtractionReport report;
        const ForeignStatus st = transcode(src, dir / "o.rar", opts, result, report, ReaderHooks{});
        assert(st == ForeignStatus::Unparseable && result.usage_refused);
        assert(result.detail.find("legacy RAR") != std::string::npos);
    }
    // Archive-internal collision over TRANSLATED names: abort pre-output.
    {
        core::uint32 crc = 0;
        const auto data = stored_payload("x", crc);
        std::vector<ZipMember> members;
        ZipMember m1;
        m1.name = "same.txt";
        m1.flags = 0x0800;
        m1.packed = data;
        m1.crc = crc;
        m1.unp_size = data.size();
        members.push_back(m1);
        ZipMember m2 = m1;
        m2.name = "SAME.txt"; // case-fold collision
        members.push_back(m2);
        const auto src = dir / "coll.zip";
        write_bytes(src, build_zip(members));
        TranscodeOptions opts = base_opts();
        TranscodeResult result;
        ExtractionReport report;
        const ForeignStatus st =
            transcode(src, dir / "coll.rar", opts, result, report, ReaderHooks{});
        assert(st == ForeignStatus::StructuralMismatch);
        assert(!std::filesystem::exists(dir / "coll.rar")); // nothing written
    }
    std::cout << "    - RAR5/legacy refusals + collision abort pre-output" << std::endl;
}

static void test_df_and_staging() {
    std::cout << "[+] test_df_and_staging" << std::endl;
    const auto dir = openrar::test::scratch_dir("transcode");

    core::uint32 crc = 0;
    const auto data = stored_payload("delete me", crc);
    std::vector<ZipMember> members;
    ZipMember m;
    m.name = "f.txt";
    m.flags = 0x0800;
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();
    members.push_back(m);

    // -df with a full verified migration: source deleted.
    {
        const auto src = dir / "df1.zip";
        write_bytes(src, build_zip(members));
        TranscodeOptions opts = base_opts();
        opts.delete_source = true;
        TranscodeResult result;
        ExtractionReport report;
        const ForeignStatus st =
            transcode(src, dir / "df1.rar", opts, result, report, ReaderHooks{});
        assert(st == ForeignStatus::Ok && result.verified);
        assert(result.source_deleted);
        assert(!std::filesystem::exists(src));
    }
    // -df with a skipped entry (encrypted): source KEPT.
    {
        core::uint32 crc2 = 0;
        const auto data2 = stored_payload("s", crc2);
        std::vector<ZipMember> members2 = members;
        ZipMember enc;
        enc.name = "enc.txt";
        enc.flags = 0x0801; // encrypted
        enc.packed = data2;
        enc.crc = crc2;
        enc.unp_size = data2.size();
        members2.push_back(enc);
        const auto src = dir / "df2.zip";
        write_bytes(src, build_zip(members2));
        TranscodeOptions opts = base_opts();
        opts.delete_source = true;
        TranscodeResult result;
        ExtractionReport report;
        const ForeignStatus st =
            transcode(src, dir / "df2.rar", opts, result, report, ReaderHooks{});
        assert(st == ForeignStatus::Ok);
        assert(!result.source_deleted);
        assert(std::filesystem::exists(src));
        assert(result.encrypted_refused_present);
    }
    // Staging sweep on abort: a cancel hook mid-decode leaves no staging.
    {
        const auto src = dir / "cancel.zip";
        write_bytes(src, build_zip(members));
        TranscodeOptions opts = base_opts();
        TranscodeResult result;
        ExtractionReport report;
        struct CancelState {
            int calls = 0;
        } cs;
        ReaderHooks hooks;
        hooks.cancel = [](void* user) -> int {
            auto* c = static_cast<CancelState*>(user);
            return ++c->calls >= 2 ? 1 : 0; // cancel at the second poll
        };
        hooks.cancel_user = &cs;
        const ForeignStatus st = transcode(src, dir / "cancel.rar", opts, result, report, hooks);
        assert(st == ForeignStatus::Aborted);
        assert(!std::filesystem::exists(dir / "cancel.rar"));
        // No openrar-cv-* staging dirs remain in the temp root.
        for (const auto& it :
             std::filesystem::directory_iterator(std::filesystem::temp_directory_path())) {
            const std::string n = it.path().filename().string();
            assert(n.rfind("openrar-cv-", 0) != 0 || !std::filesystem::is_directory(it.path()));
        }
    }
    std::cout << "    - df deletes only on verified full migration; staging swept" << std::endl;
}

#ifdef OPENRAR_CLI_EXE
static void test_cv_cli_contracts() {
    std::cout << "[+] test_cv_cli_contracts" << std::endl;
    const auto dir = openrar::test::scratch_dir("transcode");

    core::uint32 crc = 0;
    const auto data = stored_payload("cli payload", crc);
    std::vector<ZipMember> members;
    ZipMember m;
    m.name = "f.txt";
    m.flags = 0x0800;
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();
    members.push_back(m);
    const auto src = dir / "cli.zip";
    write_bytes(src, build_zip(members));

    const std::filesystem::path devnull =
        std::filesystem::temp_directory_path() / "openrar_cv_devnull.txt";
    const std::string nul = ">" + devnull.string() + " 2>&1";

    // Exit 0 + JSON v2 fields (with --json-summary=path).
    const auto json_path = dir / "summary.json";
    int rc = -1;
    rc = run_cli("cv " + src.string() + " " + (dir / "cli_out.rar").string() +
                     " --json-summary=" + json_path.string(),
                 nul);
    assert(rc == 0);
    const std::string json = read_bytes(json_path).size() ? [&] {
        std::ifstream f(json_path);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }()
                                                          : "";
    assert(json.find("\"schema_version\":2") != std::string::npos);
    assert(json.find("\"format\":\"zip\"") != std::string::npos);
    assert(json.find("\"verified\":true") != std::string::npos);
    assert(json.find("\"source_deleted\":false") != std::string::npos);
    // v1 key order preserved (schema_version first, archive second).
    assert(json.find("\"schema_version\":2,\"archive\":\"") != std::string::npos);

    // dest == source → 7.
    rc = run_cli("cv " + src.string() + " " + src.string(), nul);
    assert(rc == 7);

    // Non-TTY: zero ESC bytes on stdout (hostile-name control included via
    // the already-pinned v1.28 corpus; here the plain payload path).
    const auto cap = dir / "cap.txt";
    rc = run_cli("cv " + src.string() + " " + (dir / "nt.rar").string(),
                 ">" + cap.string() + " 2>&1");
    assert(rc == 0);
    const auto out_bytes = read_bytes(cap);
    for (const core::byte b : out_bytes) assert(b != core::byte(0x1B));

    // -v refused → 7; unknown-but-inert switch warns and exits 0.
    rc = run_cli("cv -v " + src.string(), nul);
    assert(rc == 7);
    rc = run_cli("cv " + src.string() + " " + (dir / "inert.rar").string() + " -ams", nul);
    assert(rc == 7); // -ams is on the explicit refuse list

    // RAR5 source → usage 7 with the named message.
    const auto r5 = dir / "r5.rar";
    write_bytes(r5, std::vector<core::byte>{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00});
    rc = run_cli("cv " + r5.string(), nul);
    assert(rc == 7);

    // Garbage source → 13.
    const auto junk = dir / "junk.bin";
    write_bytes(junk, std::vector<core::byte>(64, core::byte('z')));
    rc = run_cli("cv " + junk.string(), nul);
    assert(rc == 13);

    // Nothing migrated → 10, no output archive.
    const auto enc_src = dir / "enc.zip";
    core::uint32 crc2 = 0;
    const auto data2 = stored_payload("e", crc2);
    std::vector<ZipMember> enc_members;
    ZipMember enc;
    enc.name = "e.txt";
    enc.flags = 0x0801;
    enc.packed = data2;
    enc.crc = crc2;
    enc.unp_size = data2.size();
    enc_members.push_back(enc);
    write_bytes(enc_src, build_zip(enc_members));
    rc = run_cli("cv " + enc_src.string() + " " + (dir / "never.rar").string(), nul);
    assert(rc == 11); // encrypted-refused class per D1
    assert(!std::filesystem::exists(dir / "never.rar"));

    // -df e2e: verified migration deletes the source.
    const auto df_src = dir / "df.zip";
    write_bytes(df_src, build_zip(members));
    rc = run_cli("cv -df " + df_src.string() + " " + (dir / "df_out.rar").string(), nul);
    assert(rc == 0);
    assert(!std::filesystem::exists(df_src));
    assert(std::filesystem::exists(dir / "df_out.rar"));

    std::cout << "    - exit taxonomy, JSON v2, zero-ESC, refusals, -df: pinned" << std::endl;
}
#endif

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    test_transcode_zip_identity();
    std::cout << std::flush;
    test_transcode_tar_gzip_identity();
    std::cout << std::flush;
    test_transcode_refusals_and_collisions();
    std::cout << std::flush;
    test_df_and_staging();
    std::cout << std::flush;
#ifdef OPENRAR_CLI_EXE
    test_cv_cli_contracts();
    std::cout << std::flush;
#endif
    std::cout << "All Transcode (v1.29 M5) tests PASSED!" << std::endl;
    return 0;
}
