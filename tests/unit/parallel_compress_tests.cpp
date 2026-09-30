#include "../../src/core/types.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/parallel_compressor.hpp"
#include "../../src/io/file_stream.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <random>
#include <filesystem>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;
using namespace openrar::compress;

static std::vector<core::byte> generate_payload(size_t size, uint32_t seed = 42) {
    std::vector<core::byte> data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);

    size_t pos = 0;
    while (pos < size) {
        size_t block_len = std::min(size - pos, static_cast<size_t>(1024 + (dist(rng) * 64)));
        if ((dist(rng) % 3) == 0) {
            core::byte b = static_cast<core::byte>(dist(rng));
            std::fill_n(data.data() + pos, block_len, b);
        } else {
            for (size_t i = 0; i < block_len; ++i) {
                data[pos + i] = static_cast<core::byte>(dist(rng) % 64 + 32);
            }
        }
        pos += block_len;
    }
    return data;
}

static bool decompress_buffer(const std::vector<core::byte>& compressed, size_t original_size,
                              size_t win_size, std::vector<core::byte>& out_decompressed) {
    out_decompressed.clear();
    Decompressor50 decomp(win_size);
    bool ok =
        decomp.decompress_to_vector(compressed.data(), compressed.size(), out_decompressed, false);
    return ok && (out_decompressed.size() == original_size);
}

// 1. Framing Spike Test: 16 KiB chunks with explicit LastBlock flags roundtripping through Decompressor50
static void test_framing_spike() {
    std::cout << "[+] test_framing_spike: verifying sequential chunk framing" << std::endl;
    const size_t chunk_size = 16 * 1024;
    const size_t num_chunks = 4;
    auto data = generate_payload(chunk_size * num_chunks, 101);

    std::vector<core::byte> full_compressed;

    for (size_t i = 0; i < num_chunks; ++i) {
        Compressor50 packer;
        packer.begin_archive(nullptr, 3, 128 * 1024);
        packer.set_external_buffer(data.data() + i * chunk_size, chunk_size);
        packer.set_memory_dest(&full_compressed);

        bool is_last = (i == num_chunks - 1);
        core::int64 packed = packer.compress(is_last);
        assert(packed > 0);
    }

    std::vector<core::byte> decompressed;
    bool ok = decompress_buffer(full_compressed, data.size(), 128 * 1024, decompressed);
    assert(ok && "Decompression of framed chunks failed");
    assert(decompressed == data && "Payload mismatch");
    std::cout << "    - Framing spike: OK (decompressed " << decompressed.size() << " bytes)"
              << std::endl;
}

// 2. Roundtrip Tests across methods 1..5 using Compressor50::compress_buffer_parallel
static void test_parallel_roundtrip_methods() {
    std::cout << "[+] test_parallel_roundtrip_methods: testing methods 1..5 with 4 threads"
              << std::endl;
    const size_t payload_size = 5 * 1024 * 1024; // 5 MB payload (spans multiple chunks)
    auto original = generate_payload(payload_size, 202);

    for (int method = 1; method <= 5; ++method) {
        size_t dict_size = 4 * 1024 * 1024;
        std::vector<core::byte> compressed;
        bool comp_ok = Compressor50::compress_buffer_parallel(
            original.data(), original.size(), compressed, method, dict_size, {}, /*threads=*/4);

        assert(comp_ok && !compressed.empty() && "compress_buffer_parallel failed");

        std::vector<core::byte> decompressed;
        bool dec_ok = decompress_buffer(compressed, original.size(), dict_size, decompressed);

        assert(dec_ok && "Decompression failed");
        assert(decompressed == original && "Content mismatch");
        std::cout << "    - Method " << method << ": OK (ratio "
                  << (compressed.size() * 100.0 / original.size()) << "%)" << std::endl;
    }
}

// 3. Determinism Test: Byte-identical compressed stream across repeated runs
static void test_parallel_determinism() {
    std::cout << "[+] test_parallel_determinism: checking byte-identical outputs at -mt4"
              << std::endl;
    const size_t payload_size = 4 * 1024 * 1024;
    auto data = generate_payload(payload_size, 303);

    std::vector<core::byte> run1, run2, run3;
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run1, 3,
                                                  2 * 1024 * 1024, {}, 4));
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run2, 3,
                                                  2 * 1024 * 1024, {}, 4));
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run3, 3,
                                                  2 * 1024 * 1024, {}, 4));

    assert(run1.size() == run2.size() && "Size differs between run1 and run2");
    assert(run1.size() == run3.size() && "Size differs between run1 and run3");
    assert(run1 == run2 && "Bitstream differs between run1 and run2");
    assert(run1 == run3 && "Bitstream differs between run1 and run3");
    std::cout << "    - Determinism: OK (3 runs byte-identical, " << run1.size() << " bytes)"
              << std::endl;
}

// 4. ParallelBlockPipeline Streaming Test
static void test_parallel_pipeline_streaming() {
    std::cout << "[+] test_parallel_pipeline_streaming: testing ParallelBlockPipeline streaming"
              << std::endl;
    const size_t total_size = 6 * 1024 * 1024;
    auto data = generate_payload(total_size, 404);

    // Create a temporary input file
    std::filesystem::path temp_in = "test_parallel_in.tmp";
    {
        io::FileStream out;
        assert(out.open(temp_in, io::FileMode::CreateAlways));
        assert(out.write(data.data(), data.size()) == data.size());
    }

    ParallelCompressConfig cfg;
    cfg.method = 3;
    cfg.win_size = 2 * 1024 * 1024;
    cfg.threads = 4;
    cfg.chunk_size = 2 * 1024 * 1024;

    std::vector<core::byte> compressed_stream;
    core::uint32 out_crc = 0;
    core::uint64 out_packed = 0;

    {
        io::FileStream in_stream;
        assert(in_stream.open(temp_in, io::FileMode::ReadOnly));

        ParallelBlockPipeline pipeline(cfg);
        bool ok = pipeline.compress_stream(
            in_stream, total_size,
            [&](const core::byte* chunk_bytes, size_t chunk_len) {
                compressed_stream.insert(compressed_stream.end(), chunk_bytes,
                                         chunk_bytes + chunk_len);
                return true;
            },
            out_crc, out_packed);

        assert(ok && "compress_stream returned false");
    }

    std::error_code ec;
    std::filesystem::remove(temp_in, ec);

    assert(!compressed_stream.empty());
    assert(out_packed == compressed_stream.size());

    // Verify CRC
    crypto::Crc32 expected_crc;
    expected_crc.update(data.data(), data.size());
    assert(out_crc == expected_crc.get());

    std::vector<core::byte> decompressed;
    bool dec_ok = decompress_buffer(compressed_stream, total_size, cfg.win_size, decompressed);
    assert(dec_ok && "Decompression failed");
    assert(decompressed == data && "Content mismatch");
    std::cout << "    - Pipeline streaming: OK (" << total_size << " bytes streamed and verified)"
              << std::endl;
}

// 5. Cancellation Test
static void test_parallel_pipeline_cancellation() {
    std::cout << "[+] test_parallel_pipeline_cancellation: testing mid-stream cancellation"
              << std::endl;
    const size_t total_size = 4 * 1024 * 1024;
    auto data = generate_payload(total_size, 501);

    std::filesystem::path temp_in = "test_parallel_cancel.tmp";
    {
        io::FileStream out;
        assert(out.open(temp_in, io::FileMode::CreateAlways));
        assert(out.write(data.data(), data.size()) == data.size());
    }

    ParallelCompressConfig cfg;
    cfg.method = 3;
    cfg.win_size = 1024 * 1024;
    cfg.threads = 4;
    cfg.chunk_size = 512 * 1024;

    struct CancelCtx {
        int calls{0};
    } cancel_ctx;

    cfg.cancel_user = &cancel_ctx;
    cfg.cancel_cb = [](void* user) -> int {
        auto* ctx = static_cast<CancelCtx*>(user);
        ctx->calls++;
        return ctx->calls > 1 ? 1 : 0; // Abort after first query
    };

    io::FileStream in_stream;
    assert(in_stream.open(temp_in, io::FileMode::ReadOnly));

    ParallelBlockPipeline pipeline(cfg);
    core::uint32 out_crc = 0;
    core::uint64 out_packed = 0;

    bool ok = pipeline.compress_stream(
        in_stream, total_size, [&](const core::byte* /*chunk*/, size_t /*len*/) { return true; },
        out_crc, out_packed);

    assert(!ok && "compress_stream should have aborted upon cancellation");

    in_stream.close();
    std::error_code ec;
    std::filesystem::remove(temp_in, ec);

    std::cout << "    - Cancellation: OK (handled cleanly without deadlocks)" << std::endl;
}

// 6. Small File Bypass Smoke Tests (< chunk threshold)
static void test_small_file_bypass() {
    std::cout << "[+] test_small_file_bypass: testing small payloads (< chunk threshold)"
              << std::endl;
    std::vector<size_t> small_sizes = {100, 4096, 65536, 512 * 1024, 1024 * 1024};

    for (size_t sz : small_sizes) {
        auto data = generate_payload(sz, static_cast<uint32_t>(sz));
        std::vector<core::byte> comp;
        assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), comp, 3, 128 * 1024,
                                                      {}, 4));
        assert(!comp.empty());

        std::vector<core::byte> decompressed;
        assert(decompress_buffer(comp, sz, 128 * 1024, decompressed));
        assert(decompressed == data && "Content mismatch");
    }
    std::cout << "    - Small file bypass: OK" << std::endl;
}

// E8-dense PE-like content that triggers the Auto filter heuristic.
static std::vector<core::byte> generate_e8_dense(size_t size) {
    std::vector<core::byte> data(size, 0x90);
    data[0] = 'M';
    data[1] = 'Z';
    core::write_le32(data.data() + 0x3C, 64);
    data[64] = 'P';
    data[65] = 'E';
    data[66] = 0;
    data[67] = 0;
    core::write_le16(data.data() + 68, 0x8664);
    for (size_t off = 128; off + 5 <= size; off += 5) {
        data[off] = 0xE8;
        core::int32 rel = static_cast<core::int32>((off * 7919) % 0x01000000u);
        core::write_le32(data.data() + off + 1, static_cast<core::uint32>(rel));
    }
    return data;
}

// Filter-free content with LZ structure but nothing a filter would catch:
// word-like text drawn from a large vocabulary, so no E8/Delta signature and
// no long-range redundancy either.
static std::vector<core::byte> generate_text_like(size_t size) {
    std::vector<std::string> words;
    for (int i = 0; i < 4000; ++i) words.push_back("tok" + std::to_string(i));
    std::mt19937_64 rng(0x5EED1234ull);
    std::vector<core::byte> data;
    data.reserve(size + 128);
    while (data.size() < size) {
        for (int w = 0; w < 12; ++w) {
            if (w) data.push_back(' ');
            const std::string& word = words[rng() % words.size()];
            data.insert(data.end(), word.begin(), word.end());
        }
        data.push_back('\n');
    }
    data.resize(size);
    return data;
}

// Filter parity (v1.21.1): for content whose detection triggers a filter, the
// chunked pipeline must fall back to the sequential path so the filter is
// honored — and the output must be byte-identical to a plain compress_buffer
// call. Without this, -mt1 and -mt4 produced different archives for the same
// input (and -mc was silently dropped under -mt>1).
static void test_parallel_filter_parity() {
    std::cout
        << "[+] test_parallel_filter_parity: filter-triggering content matches sequential output"
        << std::endl;
    auto data = generate_e8_dense(6 * 1024 * 1024);

    std::vector<core::byte> sequential;
    assert(Compressor50::compress_buffer(data.data(), data.size(), sequential, 3, 4 * 1024 * 1024));

    for (unsigned threads : {2u, 4u, 8u}) {
        std::vector<core::byte> parallel;
        assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), parallel, 3,
                                                      4 * 1024 * 1024, {}, threads));
        assert(parallel == sequential &&
               "parallel output diverged from sequential for filter-triggering content");
        std::vector<core::byte> decompressed;
        assert(decompress_buffer(parallel, data.size(), 4 * 1024 * 1024, decompressed));
        assert(decompressed == data && "Content mismatch");
    }
    std::cout << "    - Filter parity across -mt2/-mt4/-mt8: OK" << std::endl;
}

// T-M2: pin the MT path-selection decision for BOTH branches.
//
// This test exists because the decision used to be an inverted inline
// condition inside ArchiveMutator::prepare_add_file. With the default
// FilterMode::Auto it evaluated to `use_parallel = (detected != None)`,
// which meant MT engaged only on filter-triggering input - and then ran with
// the filter it had just detected discarded, because the MT pipeline forces
// filters off - while all filter-free input (text, code, most data) silently
// fell back to the sequential path and got no MT at all. Every other test in
// this file exercises the pipeline directly, so none of them could see it.
static void test_parallel_path_selection() {
    std::cout << "[+] test_parallel_path_selection: MT engages only when no filter would fire"
              << std::endl;

    FilterConfig auto_cfg; // default: Auto
    assert(auto_cfg.mode == FilterMode::Auto && "default must be Auto for this test to mean "
                                                "anything");

    FilterConfig off_cfg;
    off_cfg.mode = FilterMode::DisableAll;

    // Filter-free content: MT must be allowed.
    {
        auto data = generate_text_like(256 * 1024);
        assert(mt_should_use_parallel(auto_cfg, data.data(), data.size()) &&
               "filter-free input must take the MT path (it used to fall back to sequential)");
    }
    // E8-dense content: a filter WOULD fire, so MT must be declined so the
    // sequential path honors -mc.
    {
        auto data = generate_e8_dense(256 * 1024);
        assert(!mt_should_use_parallel(auto_cfg, data.data(), data.size()) &&
               "filter-triggering input must take the sequential path to preserve -mc parity");
    }
    // Filters explicitly off: chunking loses nothing, so MT is allowed even
    // for content a filter would otherwise have caught.
    {
        auto data = generate_e8_dense(256 * 1024);
        assert(mt_should_use_parallel(off_cfg, data.data(), data.size()) &&
               "with filters disabled, MT must be allowed unconditionally");
    }
    // No sample available (read failed / empty): take MT, do not silently
    // fall back to sequential.
    assert(mt_should_use_parallel(auto_cfg, nullptr, 0) &&
           "no sample must not silently disable MT");
    std::cout << "    - Both selection branches + no-sample fallback: OK" << std::endl;
}

// T-M3: the chunking probe must DECLINE MT on input whose redundancy spans
// chunk boundaries, and must NOT decline it on clean input.
//
// The hazard being bounded: MT compresses fixed-size chunks independently, so
// cross-chunk redundancy is lost, and the loss is unbounded. Measured on a
// 4 MiB region repeated N times, MT's archive grew by almost exactly N
// relative to sequential (8x = +698%) while also getting ~3x slower. Two
// distinct shapes have to be caught, and neither probe alone catches both:
//   - repeated CHUNKS (the region lines up with the chunk grid) — a head-only
//     redundancy probe is blind to this, because the first chunks are
//     self-consistent and the copies only collide across the file;
//   - a repeat whose period is NOT a multiple of the chunk size, so no two
//     chunks are identical but every boundary still lands mid-copy.
static void test_parallel_chunking_probe() {
    std::cout << "[+] test_parallel_chunking_probe: declines MT on cross-chunk redundancy"
              << std::endl;
    // A small chunk keeps the probe's own compression cheap enough for a unit
    // test: the probe only runs when size >= 8 * chunk, so 256 KiB chunks let
    // the shapes below be exercised with a few MiB of data instead of tens.
    const size_t kChunk = 256 * 1024;

    // A self-consistent region with no repetition: MT is safe.
    {
        auto clean = generate_text_like(24 * kChunk);
        assert(!mt_chunking_costs_ratio(clean.data(), clean.size(), kChunk) &&
               "probe declined MT on non-redundant input");
    }
    // Incompressible input has no redundancy to lose either.
    {
        std::vector<core::byte> noise(24 * kChunk);
        std::mt19937 rng(1234);
        for (auto& b : noise) b = static_cast<core::byte>(rng());
        assert(!mt_chunking_costs_ratio(noise.data(), noise.size(), kChunk) &&
               "probe declined MT on incompressible input");
    }
    // Shape 1: the region is an exact multiple of the chunk size, so chunks
    // repeat verbatim.
    {
        auto unit = generate_text_like(4 * kChunk);
        std::vector<core::byte> repeated;
        for (int i = 0; i < 4; ++i) repeated.insert(repeated.end(), unit.begin(), unit.end());
        assert(mt_chunking_costs_ratio(repeated.data(), repeated.size(), kChunk) &&
               "probe missed repeated chunks (region aligned to the chunk grid)");
    }
    // Shape 2: deliberately NOT chunk-aligned, so no two chunks are equal but
    // every boundary lands inside a copy.
    {
        const size_t region = 100 * 1024 + 37; // coprime-ish to the 256 KiB chunk
        auto unit = generate_text_like(region);
        std::vector<core::byte> repeated;
        while (repeated.size() < 24 * kChunk) {
            repeated.insert(repeated.end(), unit.begin(), unit.end());
        }
        assert(mt_chunking_costs_ratio(repeated.data(), repeated.size(), kChunk) &&
               "probe missed cross-boundary redundancy at a non-chunk-aligned period");
    }
    std::cout << "    - Both duplication shapes declined, clean/incompressible kept: OK"
              << std::endl;
}

// KNOWN GAP, pinned so it cannot regress silently.
//
// The probe detects repeated 64 KiB regions and cross-boundary redundancy it
// can afford to measure by compression. It CANNOT see long-range matches that
// span chunk boundaries in input that contains no repeated 64 KiB window and
// is too small for the compression probe to be worth running.
//
// Measured on the benchmark payload's code third alone (18 MB, 4 MiB chunks):
// 290 distinct 64 KiB windows, zero duplicates, and 428,242 B sequential vs
// 3,855,663 B chunked — 9x. Compressed as part of the whole 50 MB corpus the
// probe does decline it, so the corpus row is safe; the single-file case is
// not.
//
// The real fix is not more detection: it is to stop losing the matches, by
// giving each chunk a dictionary-only prefix of its predecessor. That changes
// emitted bytes and is scoped to a follow-up arc. This test documents the
// limit so the next person sees it as a known, measured gap rather than
// discovering it from a user's archive.
static void test_parallel_probe_known_gap() {
    std::cout << "[+] test_parallel_probe_known_gap: documents the undetectable case" << std::endl;
    // Distinct windows, so signal 1 cannot fire.
    auto data = generate_text_like(6 * 1024 * 1024);
    for (size_t i = 0; i + 65536 <= data.size(); i += 65536) {
        data[i] = static_cast<core::byte>(i / 65536); // break any window equality
    }
    const size_t kChunk = 4 * 1024 * 1024;
    // Signal 1 must not fire on genuinely distinct content.
    assert(!mt_chunking_costs_ratio(data.data(), data.size(), kChunk) &&
           "probe false-positived on non-repeating input");
    std::cout << "    - Non-repeating input still engages MT (gap is detection, not a "
                 "false positive)"
              << std::endl;
}

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    std::cout << "=== Running Parallel Compression Tests ===" << std::endl;
    test_framing_spike();
    test_parallel_roundtrip_methods();
    test_parallel_determinism();
    test_parallel_pipeline_streaming();
    test_parallel_pipeline_cancellation();
    test_small_file_bypass();
    test_parallel_filter_parity();
    test_parallel_path_selection();
    test_parallel_chunking_probe();
    test_parallel_probe_known_gap();
    std::cout << "All Parallel Compression Tests PASSED!" << std::endl;
    return 0;
}
