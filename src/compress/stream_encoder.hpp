#ifndef OPENRAR_COMPRESS_STREAM_ENCODER_HPP
#define OPENRAR_COMPRESS_STREAM_ENCODER_HPP

#include "compressor50.hpp"
#include "../core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>

namespace openrar::compress {

// Incremental block-codec encoder. Wraps Compressor50 in Path-A style
// (docs/streaming-considerations.md §7): each feed() copies bytes into the
// compressor's window and emits completed blocks; finish() emits the final
// block with last_block=true. Memory stays bounded: output is produced
// block-by-block (≤ ~16 MiB per block) and either accumulated in output_
// or handed to the flush sink as blocks complete.
//
// The concatenation of all emitted bytes byte-identically equals
// Compressor50::compress_buffer() over the same concatenated input
// (same method / win_size) — enforced by tests/unit/stream_encoder_tests.cpp.
// Methods 1..5 stream; method 0 (STORE) buffers and passes through at
// finish() (the block codec cannot frame stored bytes).
//
// Thread-compatible per-instance (no internal locking; one caller at a time).
class StreamEncoder {
public:
    StreamEncoder(int method = 3, size_t win_size = 4 * 1024 * 1024);
    ~StreamEncoder() = default;

    StreamEncoder(const StreamEncoder&) = delete;
    StreamEncoder& operator=(const StreamEncoder&) = delete;

    // Feed a chunk. Returns false on invalid args, internal error or cancel.
    bool feed(const core::byte* src, size_t n);

    // Finish the stream. On success, out holds every emitted byte unless a
    // flush sink is set (then out is empty and all bytes went to the sink).
    bool finish(std::vector<core::byte>& out);

    // Pull accumulated output blocks produced so far (for incremental chunk streaming).
    bool take_output(std::vector<core::byte>& out) {
        out = std::move(output_);
        output_.clear();
        return true;
    }

    void reset();

    size_t total_input() const { return total_in_; }
    bool is_finished() const { return finished_; }

    // Progress callback: (done = total input fed so far, total = same value).
    // A stream's final size is unknowable up front, so `total` tracks bytes
    // ingested (== done); finish() re-emits (total_in, total_in) exactly
    // once. Monotonic in `done`. This matches the DLL streaming contract.
    void set_progress(void (*cb)(void*, core::uint64, core::uint64), void* user) {
        progress_cb_ = cb;
        progress_user_ = user;
    }
    // Polled once per feed(); return non-zero to abort (feed returns false).
    void set_cancel(int (*cb)(void*), void* user) {
        cancel_cb_ = cb;
        cancel_user_ = user;
    }
    // Output sink: invoked as completed blocks become available
    // (may be several times per feed, once per block). Return 0 to accept;
    // non-zero aborts the stream.
    void set_flush(int (*cb)(void*, const core::byte*, size_t), void* user) {
        flush_cb_ = cb;
        flush_user_ = user;
    }

private:
    int method_;
    size_t win_size_;
    std::vector<core::byte> output_;    // accumulated output when no flush sink
    std::vector<core::byte> scratch_;   // per-feed block bytes from the packer
    Compressor50 packer_;
    bool started_{false}; // packer session open (methods 1..5)
    bool finished_{false};
    // Set when the flush sink rejected output: finish() must report failure
    // instead of returning partial output as success (sweep finding L8).
    bool aborted_{false};
    core::uint64 total_in_{0};
    void (*progress_cb_)(void*, core::uint64, core::uint64){nullptr};
    void* progress_user_{nullptr};
    int (*cancel_cb_)(void*){nullptr};
    void* cancel_user_{nullptr};
    int (*flush_cb_)(void*, const core::byte*, size_t){nullptr};
    void* flush_user_{nullptr};

    void drain();
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_STREAM_ENCODER_HPP
