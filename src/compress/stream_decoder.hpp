#ifndef OPENRAR_COMPRESS_STREAM_DECODER_HPP
#define OPENRAR_COMPRESS_STREAM_DECODER_HPP

#include "decompressor50.hpp"
#include "../core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openrar::compress {

// Incremental block-codec decoder. Counterpart to StreamEncoder.
// Wraps Decompressor50 with a stateful input staging buffer.
// Incoming compressed bytes are fed via feed() in arbitrary chunks (including
// micro-chunks); blocks are parsed and decoded as soon as a complete block
// is available, emitting uncompressed data to flush_cb (or accumulating into output_).
// Memory is bounded: input buffer never exceeds one block, and output is
// produced incrementally without retaining the accumulated uncompressed stream in RAM.
//
// Thread-compatible per-instance (no internal locking; one caller at a time).
class StreamDecoder {
public:
    explicit StreamDecoder(size_t win_size = 4 * 1024 * 1024, int method = 3);
    ~StreamDecoder() = default;

    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    // Feed a compressed chunk. Returns false on invalid args, corrupt stream or cancel.
    bool feed(const core::byte* src, size_t n);

    // Finish the stream. On success, out holds every emitted byte unless a
    // flush sink is set (then out is empty and all bytes went to the sink).
    bool finish(std::vector<core::byte>& out);

    // Drain any accumulated output bytes decoded so far into out.
    bool take_output(std::vector<core::byte>& out);

    void reset();

    size_t total_input() const { return total_in_; }
    size_t total_output() const { return total_out_; }
    bool is_finished() const { return finished_; }
    bool is_valid() const { return !init_failed_; }

    void set_progress(void (*cb)(void*, core::uint64, core::uint64), void* user) {
        progress_cb_ = cb;
        progress_user_ = user;
    }

    void set_cancel(int (*cb)(void*), void* user) {
        cancel_cb_ = cb;
        cancel_user_ = user;
    }

    void set_flush(int (*cb)(void*, const core::byte*, size_t), void* user) {
        flush_cb_ = cb;
        flush_user_ = user;
    }

private:
    bool process_blocks();

    size_t win_size_;
    int method_;
    bool init_failed_{false};
    bool started_{false};
    bool finished_{false};
    bool aborted_{false};
    bool has_decoded_block_{false};

    core::uint64 total_in_{0};
    core::uint64 total_out_{0};

    std::vector<core::byte> in_queue_;
    std::vector<core::byte> output_;

    Decompressor50 decompressor_;

    void (*progress_cb_)(void*, core::uint64, core::uint64){nullptr};
    void* progress_user_{nullptr};
    int (*cancel_cb_)(void*){nullptr};
    void* cancel_user_{nullptr};
    int (*flush_cb_)(void*, const core::byte*, size_t){nullptr};
    void* flush_user_{nullptr};
};

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_STREAM_DECODER_HPP
