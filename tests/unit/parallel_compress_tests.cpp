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

static bool decompress_buffer(const std::vector<core::byte>& compressed,
                              size_t original_size,
                              size_t win_size,
                              std::vector<core::byte>& out_decompressed) {
    out_decompressed.clear();
    Decompressor50 decomp(win_size);
    bool ok = decomp.decompress_to_vector(compressed.data(), compressed.size(), out_decompressed, false);
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
    std::cout << "    - Framing spike: OK (decompressed " << decompressed.size() << " bytes)" << std::endl;
}

// 2. Roundtrip Tests across methods 1..5 using Compressor50::compress_buffer_parallel
static void test_parallel_roundtrip_methods() {
    std::cout << "[+] test_parallel_roundtrip_methods: testing methods 1..5 with 4 threads" << std::endl;
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
    std::cout << "[+] test_parallel_determinism: checking byte-identical outputs at -mt4" << std::endl;
    const size_t payload_size = 4 * 1024 * 1024;
    auto data = generate_payload(payload_size, 303);

    std::vector<core::byte> run1, run2, run3;
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run1, 3, 2 * 1024 * 1024, {}, 4));
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run2, 3, 2 * 1024 * 1024, {}, 4));
    assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), run3, 3, 2 * 1024 * 1024, {}, 4));

    assert(run1.size() == run2.size() && "Size differs between run1 and run2");
    assert(run1.size() == run3.size() && "Size differs between run1 and run3");
    assert(run1 == run2 && "Bitstream differs between run1 and run2");
    assert(run1 == run3 && "Bitstream differs between run1 and run3");
    std::cout << "    - Determinism: OK (3 runs byte-identical, " << run1.size() << " bytes)" << std::endl;
}

// 4. ParallelBlockPipeline Streaming Test
static void test_parallel_pipeline_streaming() {
    std::cout << "[+] test_parallel_pipeline_streaming: testing ParallelBlockPipeline streaming" << std::endl;
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
                compressed_stream.insert(compressed_stream.end(), chunk_bytes, chunk_bytes + chunk_len);
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
    std::cout << "    - Pipeline streaming: OK (" << total_size << " bytes streamed and verified)" << std::endl;
}

// 5. Cancellation Test
static void test_parallel_pipeline_cancellation() {
    std::cout << "[+] test_parallel_pipeline_cancellation: testing mid-stream cancellation" << std::endl;
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
        in_stream, total_size,
        [&](const core::byte* /*chunk*/, size_t /*len*/) {
            return true;
        },
        out_crc, out_packed);

    assert(!ok && "compress_stream should have aborted upon cancellation");

    in_stream.close();
    std::error_code ec;
    std::filesystem::remove(temp_in, ec);

    std::cout << "    - Cancellation: OK (handled cleanly without deadlocks)" << std::endl;
}

// 6. Small File Bypass Smoke Tests (< chunk threshold)
static void test_small_file_bypass() {
    std::cout << "[+] test_small_file_bypass: testing small payloads (< chunk threshold)" << std::endl;
    std::vector<size_t> small_sizes = {100, 4096, 65536, 512 * 1024, 1024 * 1024};

    for (size_t sz : small_sizes) {
        auto data = generate_payload(sz, static_cast<uint32_t>(sz));
        std::vector<core::byte> comp;
        assert(Compressor50::compress_buffer_parallel(data.data(), data.size(), comp, 3, 128 * 1024, {}, 4));
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
    data[64] = 'P'; data[65] = 'E'; data[66] = 0; data[67] = 0;
    core::write_le16(data.data() + 68, 0x8664);
    for (size_t off = 128; off + 5 <= size; off += 5) {
        data[off] = 0xE8;
        core::int32 rel = static_cast<core::int32>((off * 7919) % 0x01000000u);
        core::write_le32(data.data() + off + 1, static_cast<core::uint32>(rel));
    }
    return data;
}

// Filter parity (v1.21.1): for content whose detection triggers a filter, the
// chunked pipeline must fall back to the sequential path so the filter is
// honored — and the output must be byte-identical to a plain compress_buffer
// call. Without this, -mt1 and -mt4 produced different archives for the same
// input (and -mc was silently dropped under -mt>1).
static void test_parallel_filter_parity() {
    std::cout << "[+] test_parallel_filter_parity: filter-triggering content matches sequential output" << std::endl;
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
    std::cout << "All Parallel Compression Tests PASSED!" << std::endl;
    return 0;
}
