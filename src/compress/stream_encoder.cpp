#include "stream_encoder.hpp"
#include <cstring>

namespace openrar::compress {

StreamEncoder::StreamEncoder(int method, size_t win_size, const FilterConfig& filter_cfg)
    : method_(method), win_size_(win_size), filter_cfg_(filter_cfg) {
    if (method_ < 0) method_ = 3;
    if (method_ > 5) method_ = 5;
    if (win_size_ == 0) win_size_ = 4 * 1024 * 1024;
}

// Route completed block bytes to the sink (or accumulate). Called after every
// feed and after finish_stream; scratch_ never grows beyond one block.
void StreamEncoder::drain() {
    if (scratch_.empty()) return;
    if (flush_cb_) {
        if (flush_cb_(flush_user_, scratch_.data(), scratch_.size()) != 0) {
            // Sink rejection aborts the stream: mark finished so further
            // feeds fail, and remember the abort so finish() reports failure
            // instead of returning partial output as success (L8).
            scratch_.clear();
            finished_ = true;
            aborted_ = true;
            return;
        }
    } else {
        output_.insert(output_.end(), scratch_.begin(), scratch_.end());
    }
    scratch_.clear();
}

bool StreamEncoder::feed(const core::byte* src, size_t n) {
    if (finished_) return false;
    if (n == 0) return true;
    if (!src) return false;
    if (cancel_cb_ && cancel_cb_(cancel_user_)) return false;

    if (method_ == 0) {
        total_in_ += n;
        if (flush_cb_) {
            if (flush_cb_(flush_user_, src, n) != 0) {
                finished_ = true;
                aborted_ = true;
                return false;
            }
        } else {
            output_.insert(output_.end(), src, src + n);
        }
        if (progress_cb_) progress_cb_(progress_user_, total_in_, total_in_);
        return true;
    }

    if (!started_) {
        if (!packer_.begin_stream(method_, win_size_)) return false;
        packer_.set_filter_config(filter_cfg_);
        packer_.set_memory_dest(&scratch_);
        started_ = true;
    }
    if (packer_.feed(src, n) < 0) return false;
    total_in_ += n;
    drain();
    if (finished_) return false; // sink aborted during drain
    if (progress_cb_) progress_cb_(progress_user_, total_in_, total_in_);
    return true;
}

bool StreamEncoder::finish(std::vector<core::byte>& out) {
    out.clear();
    if (finished_) {
        if (aborted_) return false; // sink aborted mid-stream: not a success
        out = output_;
        return true;
    }
    if (cancel_cb_ && cancel_cb_(cancel_user_)) return false;

    if (method_ == 0) {
        // STORE bytes were already routed to flush_cb_ or accumulated in output_ during feed().
    } else if (started_) {
        if (packer_.finish_stream() < 0) return false;
        drain();
        if (finished_) return false; // sink aborted during drain
    }
    // A stream that never received a feed produces empty output — matching
    // compress_buffer(src, 0) semantics.
    finished_ = true;
    if (flush_cb_ && !output_.empty()) {
        if (flush_cb_(flush_user_, output_.data(), output_.size()) != 0) {
            output_.clear();
            return false;
        }
        output_.clear();
    }
    out = output_;
    if (progress_cb_) progress_cb_(progress_user_, total_in_, total_in_);
    return true;
}

void StreamEncoder::reset() {
    output_.clear();
    scratch_.clear();
    packer_ = Compressor50();
    started_ = false;
    finished_ = false;
    aborted_ = false;
    total_in_ = 0;
}

} // namespace openrar::compress
