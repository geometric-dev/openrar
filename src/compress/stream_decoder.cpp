#include "stream_decoder.hpp"
#include <algorithm>
#include <cstring>

namespace openrar::compress {

StreamDecoder::StreamDecoder(size_t win_size, int method)
    : win_size_(win_size ? win_size : 4 * 1024 * 1024), method_(method),
      decompressor_(win_size_ ? win_size_ : 4 * 1024 * 1024) {
    // Clamp before the member-init list builds the decompressor: a zero window
    // previously reached Decompressor50 with win_size_ == 0 (undefined window
    // arithmetic). Decompressor50 self-clamps too; this keeps win_size_
    // consistent with the decompressor's view.
#if defined(__EMSCRIPTEN__)
    // Enforce 64 MiB allocation ceiling in 32-bit WASM to prevent heap exhaustion.
    if (win_size_ > 64ULL * 1024 * 1024) {
        init_failed_ = true;
    }
#endif
    if (decompressor_.last_error() != DecompressErrorCode::Ok) {
        init_failed_ = true;
    }
}

bool StreamDecoder::feed(const core::byte* src, size_t n) {
    if (init_failed_ || aborted_ || finished_) return false;
    if (n == 0) return true;
    if (!src) return false;
    if (cancel_cb_ && cancel_cb_(cancel_user_)) {
        aborted_ = true;
        return false;
    }

    total_in_ += n;

    if (method_ == 0) {
        // STORE method passthrough
        if (flush_cb_) {
            if (flush_cb_(flush_user_, src, n) != 0) {
                aborted_ = true;
                return false;
            }
        } else {
            if (!retain_output(src, n)) return false;
        }
        total_out_ += n;
        if (progress_cb_) progress_cb_(progress_user_, total_out_, total_out_);
        return true;
    }

    // M-08: with nothing queued, decode every complete block straight from
    // the caller's buffer and stage only the incomplete tail. The queue copy
    // used to be paid on every byte of input.
    if (in_head_ == in_queue_.size()) {
        in_queue_.clear();
        in_head_ = 0;
        size_t consumed = 0;
        if (!process_buffer(src, n, consumed)) return false;
        if (consumed < n) {
            in_queue_.insert(in_queue_.end(), src + consumed, src + n);
            // Defensive: process any queued complete blocks immediately;
            // without this a queued tail could stall (bug #2, stream stall
            // at 32768 bytes, nightly divergence a=32768).
            process_blocks();
        }
        return true;
    }

    in_queue_.insert(in_queue_.end(), src, src + n);
    return process_blocks();
}

// H-17: without a flush sink every decoded byte is retained in output_ until
// take_output()/finish(). Cap the retained bytes at the same ceiling
// decompress_to_vector() enforces, so a small decompression bomb fed through
// the streaming API (wasm openrar_stream_decompress_feed) fails closed
// instead of exhausting the heap.
bool StreamDecoder::retain_output(const core::byte* data, size_t size) {
    if (static_cast<core::uint64>(output_.size()) + size > MAX_RETAINED_OUTPUT) {
        aborted_ = true;
        return false;
    }
    output_.insert(output_.end(), data, data + size);
    return true;
}

bool StreamDecoder::process_blocks() {
    size_t consumed = 0;
    const bool ok =
        process_buffer(in_queue_.data() + in_head_, in_queue_.size() - in_head_, consumed);
    in_head_ += consumed;
    if (in_head_ == in_queue_.size()) {
        in_queue_.clear();
        in_head_ = 0;
    } else if (in_head_ >= 0x40000) {
        in_queue_.erase(in_queue_.begin(), in_queue_.begin() + in_head_);
        in_head_ = 0;
    }
    return ok;
}

// Decode every complete block in [buf, buf + len). `consumed` reports the
// bytes of whole blocks decoded; the remainder is an incomplete block the
// caller must stage. buf must stay valid only for the duration of the call.
bool StreamDecoder::process_buffer(const core::byte* buf, size_t len, size_t& consumed) {
    consumed = 0;
    // M-08: one OutputCallback for the whole batch instead of one
    // std::function conversion per block.
    const Decompressor50::OutputCallback sink = [this](const core::byte* data,
                                                       size_t size) -> bool {
        if (flush_cb_) {
            if (flush_cb_(flush_user_, data, size) != 0) return false;
        } else if (!retain_output(data, size)) {
            return false;
        }
        total_out_ += size;
        return true;
    };

    while (consumed < len && !finished_) {
        if (cancel_cb_ && cancel_cb_(cancel_user_)) {
            aborted_ = true;
            return false;
        }

        const size_t avail = len - consumed;
        const core::byte* q = buf + consumed;

        // Need at least 3 bytes for a minimum block header: flags (1B), chk (1B), block_size (>=1B)
        if (avail < 3) break;

        core::uint32 flags = q[0];
        core::uint32 byte_cnt = ((flags >> 3) & 3) + 1;
        if (byte_cnt == 4) {
            aborted_ = true;
            return false; // Spec violation
        }
        size_t header_size = 2 + byte_cnt;
        if (avail < header_size) break;

        core::uint32 saved = q[1];
        core::uint32 block_size = 0;
        for (core::uint32 i = 0; i < byte_cnt; ++i) {
            block_size |= static_cast<core::uint32>(q[2 + i]) << (i * 8);
        }

        core::uint32 chk =
            (0x5A ^ flags ^ block_size ^ (block_size >> 8) ^ (block_size >> 16)) & 0xFF;
        if (chk != saved) {
            aborted_ = true;
            return false; // Corrupt block header checksum
        }

        size_t total_block_bytes = header_size + block_size;
        if (avail < total_block_bytes) {
            // Need more data before this block can be safely decompressed
            break;
        }

        // The entire block is contiguous in buf.
        size_t written = 0;
        bool block_finished = false;

        bool ok = decompressor_.decompress_block(q, total_block_bytes, sink, &written,
                                                 &block_finished, has_decoded_block_);
        if (!ok) {
            aborted_ = true;
            return false;
        }

        has_decoded_block_ = true;
        if (block_finished || (flags & 0x40) != 0) {
            finished_ = true;
        }

        // Advance read cursor past consumed block
        consumed += total_block_bytes;

        if (progress_cb_) {
            progress_cb_(progress_user_, total_out_, total_out_);
        }
    }
    return true;
}

bool StreamDecoder::finish(std::vector<core::byte>& out) {
    out.clear();
    if (init_failed_ || aborted_) return false;
    if (cancel_cb_ && cancel_cb_(cancel_user_)) {
        aborted_ = true;
        return false;
    }

    if (method_ == 0) {
        finished_ = true;
        out = std::move(output_);
        output_.clear();
        return true;
    }

    // Process any remaining complete blocks
    if (!process_blocks()) return false;

    // A valid finished stream must not have leftover unparsed bytes
    if (in_head_ != in_queue_.size()) {
        aborted_ = true;
        return false;
    }

    // A valid stream ends with the LastBlock flag (the decompressor sets
    // finished_ only then). Accepting a stream that merely ran out of input
    // would silently pass truncated payloads through.
    if (!finished_) {
        aborted_ = true;
        return false;
    }

    finished_ = true;
    out = std::move(output_);
    output_.clear();
    return true;
}

bool StreamDecoder::take_output(std::vector<core::byte>& out) {
    if (init_failed_ || aborted_) return false;
    out = std::move(output_);
    output_.clear();
    return true;
}

void StreamDecoder::reset() {
    in_queue_.clear();
    in_head_ = 0;
    output_.clear();
    decompressor_ = Decompressor50(win_size_);
    started_ = false;
    finished_ = false;
    aborted_ = false;
    has_decoded_block_ = false;
    total_in_ = 0;
    total_out_ = 0;
}

} // namespace openrar::compress
