#ifndef OPENRAR_COMPRESS_PARALLEL_COMPRESSOR_HPP
#define OPENRAR_COMPRESS_PARALLEL_COMPRESSOR_HPP

#include "../core/types.hpp"
#include "../core/thread_pool.hpp"
#include "../crypto/crc32.hpp"
#include "../io/file_stream.hpp"
#include "compressor50.hpp"
#include "stream_encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace openrar::compress {

// Callback types for streaming parallel compression
using ParallelSinkFn = std::function<bool(const core::byte* data, size_t size)>;
using ParallelProgressCb = void (*)(void* user, core::uint64 done, core::uint64 total);
using ParallelCancelCb = int (*)(void* user);

struct ParallelCompressConfig {
    int method{3};
    size_t win_size{0x800000};
    unsigned threads{0};          // 0 = auto-detect
    size_t chunk_size{2 * 1024 * 1024}; // 2 MiB default chunk
    ParallelProgressCb progress_cb{nullptr};
    void* progress_user{nullptr};
    ParallelCancelCb cancel_cb{nullptr};
    void* cancel_user{nullptr};
};

class ParallelBlockPipeline {
public:
    explicit ParallelBlockPipeline(const ParallelCompressConfig& cfg);
    ~ParallelBlockPipeline() = default;

    ParallelBlockPipeline(const ParallelBlockPipeline&) = delete;
    ParallelBlockPipeline& operator=(const ParallelBlockPipeline&) = delete;

    // Stream-compresses uncompressed data from src_stream and emits completed RAR5 blocks
    // in strict sequential order to sink_fn.
    // Computes unpacked CRC32 on-the-fly.
    // Returns true on success, false on I/O error, compression error, or cancellation.
    bool compress_stream(io::FileStream& src_stream, core::uint64 file_size,
                         const ParallelSinkFn& sink_fn, core::uint32& out_crc32,
                         core::uint64& out_packed_bytes);

    // In-memory buffer parallel compression
    static bool compress_buffer(const core::byte* src, size_t src_size,
                                std::vector<core::byte>& dest, int method = 3,
                                size_t win_size = 0x800000, unsigned threads = 0);

private:
    ParallelCompressConfig cfg_;
    unsigned eff_threads_{1};
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_PARALLEL_COMPRESSOR_HPP
