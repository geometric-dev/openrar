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
            output_.insert(output_.end(), src, src + n);
        }
        total_out_ += n;
        if (progress_cb_) progress_cb_(progress_user_, total_out_, total_out_);
        return true;
    }

    in_queue_.insert(in_queue_.end(), src, src + n);
    return process_blocks();
}

bool StreamDecoder::process_blocks() {
    while (!in_queue_.empty() && !finished_) {
        if (cancel_cb_ && cancel_cb_(cancel_user_)) {
            aborted_ = true;
            return false;
        }

        // Need at least 3 bytes for a minimum block header: flags (1B), chk (1B), block_size (>=1B)
        if (in_queue_.size() < 3) break;

        core::uint32 flags = in_queue_[0];
        core::uint32 byte_cnt = ((flags >> 3) & 3) + 1;
        if (byte_cnt == 4) {
            aborted_ = true;
            return false; // Spec violation
        }
        size_t header_size = 2 + byte_cnt;
        if (in_queue_.size() < header_size) break;

        core::uint32 saved = in_queue_[1];
        core::uint32 block_size = 0;
        for (core::uint32 i = 0; i < byte_cnt; ++i) {
            block_size |= static_cast<core::uint32>(in_queue_[2 + i]) << (i * 8);
        }

        core::uint32 chk =
            (0x5A ^ flags ^ block_size ^ (block_size >> 8) ^ (block_size >> 16)) & 0xFF;
        if (chk != saved) {
            aborted_ = true;
            return false; // Corrupt block header checksum
        }

        size_t total_block_bytes = header_size + block_size;
        if (in_queue_.size() < total_block_bytes) {
            // Need more data before this block can be safely decompressed
            break;
        }

        // We have the entire block contiguous in in_queue_!
        size_t written = 0;
        bool block_finished = false;

        auto sink = [&](const core::byte* data, size_t size) -> bool {
            if (flush_cb_) {
                if (flush_cb_(flush_user_, data, size) != 0) {
                    return false;
                }
            } else {
                output_.insert(output_.end(), data, data + size);
            }
            total_out_ += size;
            return true;
        };

        bool ok = decompressor_.decompress_block(in_queue_.data(), total_block_bytes, sink,
                                                 &written, &block_finished, has_decoded_block_);
        if (!ok) {
            aborted_ = true;
            return false;
        }

        has_decoded_block_ = true;
        if (block_finished || (flags & 0x40) != 0) {
            finished_ = true;
        }

        // Advance past consumed block
        in_queue_.erase(in_queue_.begin(), in_queue_.begin() + total_block_bytes);

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
    if (!in_queue_.empty()) {
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
