#include "parallel_compressor.hpp"
#include "filters50.hpp"

#include <algorithm>
#include <deque>

namespace openrar::compress {

bool mt_should_use_parallel(const FilterConfig& filter_cfg, const core::byte* sample,
                            size_t sample_len) {
    // Filters explicitly disabled: nothing would be lost by chunking.
    if (filter_cfg.mode == FilterMode::DisableAll) return true;
    // No sample to judge with: take MT (the caller's sample read failed).
    if (sample == nullptr || sample_len == 0) return true;
    core::uint8 channels = 1;
    const FilterType detected = Filters50::detect_filter(sample, sample_len, channels, filter_cfg);
    return detected == FilterType::None;
}

ParallelBlockPipeline::ParallelBlockPipeline(const ParallelCompressConfig& cfg) : cfg_(cfg) {
    // win_size == 0 is the documented "use the default window" convention.
    // Normalize it here, before any geometry is derived: the seed-length
    // subtraction below is only underflow-safe for a non-zero window.
    if (cfg_.win_size == 0) cfg_.win_size = 0x200000;
    eff_threads_ = cfg_.threads == 0 ? core::hardware_thread_hint() : cfg_.threads;
    if (eff_threads_ == 0) eff_threads_ = 1;
    if (eff_threads_ > 16) eff_threads_ = 16; // Clamp to 16 workers to bound memory footprint
}

bool ParallelBlockPipeline::compress_buffer(const core::byte* src, size_t src_size,
                                            std::vector<core::byte>& dest, int method,
                                            size_t win_size, unsigned threads) {
    return Compressor50::compress_buffer_parallel(src, src_size, dest, method, win_size, {},
                                                  threads);
}

bool ParallelBlockPipeline::compress_stream(io::FileStream& src_stream, core::uint64 file_size,
                                            const ParallelSinkFn& sink_fn, core::uint32& out_crc32,
                                            core::uint64& out_packed_bytes) {
    out_packed_bytes = 0;
    out_crc32 = 0;
    if (file_size == 0) return true;

    constexpr size_t BLOCK_QUANTUM = 0x80000; // one 512 KiB block quantum
    size_t chunk_size = parallel_chunk_size(file_size, eff_threads_, cfg_.chunk_size);

    FilterConfig no_filters;
    no_filters.mode = FilterMode::DisableAll;

    // Sequential fallback for 1 thread or tiny files.
    if (eff_threads_ <= 1 || file_size <= chunk_size) {
        StreamEncoder encoder(cfg_.method, cfg_.win_size, no_filters);
        struct FlushSinkCtx {
            const ParallelSinkFn* sink;
            core::uint64* packed_count;
            bool ok;
        } fctx{&sink_fn, &out_packed_bytes, true};

        encoder.set_flush(
            [](void* user, const core::byte* data, size_t size) -> int {
                auto* ctx = static_cast<FlushSinkCtx*>(user);
                if (size > 0) {
                    if (!(*ctx->sink)(data, size)) {
                        ctx->ok = false;
                        return -1;
                    }
                    *(ctx->packed_count) += size;
                }
                return 0;
            },
            &fctx);

        crypto::Crc32 crc_c;
        // Cancellation is polled once per read, so the read must stay bounded
        // even when the chunk floor pushes chunk_size far above the file: with
        // a chunk-sized buffer a small member is a single read and the cancel
        // callback is never consulted twice. One block quantum matches the
        // encoder's own flush cadence and keeps the buffer footprint small.
        std::vector<core::byte> read_buf(std::min(chunk_size, BLOCK_QUANTUM));
        core::uint64 remaining = file_size;
        core::uint64 done = 0;
        while (remaining > 0) {
            if (cfg_.cancel_cb && cfg_.cancel_cb(cfg_.cancel_user) != 0) return false;
            size_t to_read =
                static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
            if (src_stream.read(read_buf.data(), to_read) != to_read) return false;
            crc_c.update(read_buf.data(), to_read);
            if (!encoder.feed(read_buf.data(), to_read) || !fctx.ok) return false;
            remaining -= to_read;
            done += to_read;
            if (cfg_.progress_cb) cfg_.progress_cb(cfg_.progress_user, done, file_size);
        }
        std::vector<core::byte> final_chunk;
        if (!encoder.finish(final_chunk) || !fctx.ok) return false;
        if (!final_chunk.empty()) {
            if (!sink_fn(final_chunk.data(), final_chunk.size())) return false;
            out_packed_bytes += final_chunk.size();
        }
        out_crc32 = crc_c.get();
        return true;
    }

    // High-throughput chunk pipeline with bounded in-flight memory
    core::uint64 total_chunks = (file_size + chunk_size - 1) / chunk_size;
    size_t max_in_flight = std::max<size_t>(2, eff_threads_ * 2);

    struct ChunkJob {
        core::uint64 index{0};
        bool is_last{false};
        size_t seed_len{0}; // leading history bytes, indexed but not emitted
        std::vector<core::byte> uncompressed;
        std::vector<core::byte> compressed;
        bool ready{false};
        bool failed{false};

        size_t data_len() const { return uncompressed.size() - seed_len; }
    };

    std::deque<std::shared_ptr<ChunkJob>> in_flight;
    std::vector<std::shared_ptr<ChunkJob>> free_jobs;
    std::vector<std::unique_ptr<Compressor50>> free_packers;
    free_jobs.reserve(max_in_flight + 1);
    free_packers.reserve(eff_threads_);
    std::mutex mu;
    std::condition_variable cv_ready;
    std::atomic<bool> abort_flag{false};
    std::exception_ptr captured_ex;

    core::ThreadPool pool(std::min<unsigned>(eff_threads_, static_cast<unsigned>(total_chunks)));
    crypto::Crc32 crc_calc;
    core::uint64 done_bytes = 0;
    core::uint64 next_chunk_to_read = 0;

    auto acquire_job = [&]() -> std::shared_ptr<ChunkJob> {
        std::lock_guard<std::mutex> lk(mu);
        if (!free_jobs.empty()) {
            auto j = std::move(free_jobs.back());
            free_jobs.pop_back();
            return j;
        }
        return std::make_shared<ChunkJob>();
    };

    auto drain_front = [&](bool wait_for_front) -> bool {
        std::shared_ptr<ChunkJob> job_to_write;
        {
            std::unique_lock<std::mutex> lk(mu);
            if (in_flight.empty()) return true;

            // Explicit post-abort contract: once cancellation is observed, no
            // further output is delivered to the sink — not even chunks that
            // already completed. Callers must discard partial sink output.
            if (abort_flag.load(std::memory_order_relaxed)) return false;

            if (wait_for_front && !in_flight.front()->ready &&
                !abort_flag.load(std::memory_order_relaxed)) {
                cv_ready.wait(lk, [&] {
                    return abort_flag.load(std::memory_order_relaxed) ||
                           (!in_flight.empty() && in_flight.front()->ready);
                });
            }

            if (abort_flag.load(std::memory_order_relaxed) &&
                (!in_flight.empty() && !in_flight.front()->ready)) {
                return false;
            }

            if (in_flight.empty() || !in_flight.front()->ready) {
                return true; // Front chunk not ready yet (non-blocking)
            }

            job_to_write = in_flight.front();
            in_flight.pop_front();
        }

        if (job_to_write->failed) {
            abort_flag = true;
            return false;
        }

        if (!job_to_write->compressed.empty()) {
            if (!sink_fn(job_to_write->compressed.data(), job_to_write->compressed.size())) {
                abort_flag = true;
                return false;
            }
            out_packed_bytes += job_to_write->compressed.size();
        }
        job_to_write->compressed.clear();
        job_to_write->uncompressed.clear();
        {
            std::lock_guard<std::mutex> lk(mu);
            if (free_jobs.size() <= max_in_flight) {
                free_jobs.push_back(std::move(job_to_write));
            }
        }

        if (cfg_.progress_cb) {
            cfg_.progress_cb(cfg_.progress_user, done_bytes, file_size);
        }
        return true;
    };

    std::shared_ptr<ChunkJob> next_job = acquire_job();
    {
        size_t first_len = static_cast<size_t>(std::min<core::uint64>(chunk_size, file_size));
        next_job->index = 0;
        next_job->is_last = (total_chunks == 1);
        next_job->seed_len = 0;
        next_job->ready = false;
        next_job->failed = false;
        next_job->compressed.clear();
        next_job->uncompressed.resize(first_len);
    }

    while (next_chunk_to_read < total_chunks) {
        // Poll cancellation and exception on main thread
        if (cfg_.cancel_cb && cfg_.cancel_cb(cfg_.cancel_user) != 0) {
            abort_flag = true;
            break;
        }
        if (abort_flag.load(std::memory_order_relaxed)) break;

        // Drain any chunks that have completed at the front
        for (;;) {
            bool front_ready = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                front_ready = !in_flight.empty() && in_flight.front()->ready;
            }
            if (!front_ready) break;
            if (!drain_front(false)) break;
        }

        // Throttle input to bound memory: wait for front chunk if in_flight is full
        while (in_flight.size() >= max_in_flight && !abort_flag.load(std::memory_order_relaxed)) {
            if (!drain_front(/*wait_for_front=*/true)) break;
        }
        if (abort_flag.load(std::memory_order_relaxed)) break;

        std::shared_ptr<ChunkJob> job = std::move(next_job);
        core::uint64 off = next_chunk_to_read * chunk_size;
        size_t len = static_cast<size_t>(std::min<core::uint64>(chunk_size, file_size - off));
        const size_t seed_len = job->seed_len;

        if (src_stream.read(job->uncompressed.data() + seed_len, len) != len) {
            abort_flag = true;
            break;
        }
        crc_calc.update(job->uncompressed.data() + seed_len, len);
        done_bytes += len;

        // Hand off the trailing history seed to the next chunk in memory before
        // submitting `job`, eliminating backward seeks and duplicate disk reads.
        if (next_chunk_to_read + 1 < total_chunks) {
            core::uint64 next_off = (next_chunk_to_read + 1) * chunk_size;
            size_t next_len =
                static_cast<size_t>(std::min<core::uint64>(chunk_size, file_size - next_off));
            const core::uint64 seed_avail = std::min<core::uint64>(cfg_.win_size, next_off);
            size_t next_seed_len = seed_avail > 0 ? static_cast<size_t>(seed_avail - 1) : 0;
            next_job = acquire_job();
            next_job->index = next_chunk_to_read + 1;
            next_job->is_last = (next_chunk_to_read + 1 == total_chunks - 1);
            next_job->seed_len = next_seed_len;
            next_job->ready = false;
            next_job->failed = false;
            next_job->compressed.clear();
            next_job->uncompressed.resize(next_seed_len + next_len);
            if (next_seed_len > 0) {
                std::memcpy(next_job->uncompressed.data(),
                            job->uncompressed.data() + job->uncompressed.size() - next_seed_len,
                            next_seed_len);
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu);
            in_flight.push_back(job);
        }

        pool.submit(
            [job, this, no_filters, &mu, &cv_ready, &abort_flag, &captured_ex, &free_packers] {
                std::unique_ptr<Compressor50> packer;
                try {
                    if (abort_flag.load(std::memory_order_relaxed)) {
                        std::lock_guard<std::mutex> lk(mu);
                        job->failed = true;
                    } else {
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            if (!free_packers.empty()) {
                                packer = std::move(free_packers.back());
                                free_packers.pop_back();
                            }
                        }
                        if (!packer) packer = std::make_unique<Compressor50>();

                        const size_t worker_win = std::max<size_t>(
                            1, std::min<size_t>(cfg_.win_size, job->seed_len + job->data_len()));
                        packer->begin_archive(nullptr, cfg_.method, worker_win);
                        packer->set_filter_config(no_filters);
                        packer->set_seeded_external_buffer(job->uncompressed.data(), job->seed_len,
                                                           job->data_len());
                        packer->set_memory_dest(&job->compressed);
                        core::int64 r = packer->compress(job->is_last);
                        std::lock_guard<std::mutex> lk(mu);
                        if (r < 0) {
                            job->failed = true;
                            abort_flag = true;
                        } else {
                            job->ready = true;
                        }
                        free_packers.push_back(std::move(packer));
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lk(mu);
                    job->failed = true;
                    abort_flag = true;
                    if (!captured_ex) captured_ex = std::current_exception();
                    if (packer) free_packers.push_back(std::move(packer));
                }
                cv_ready.notify_all();
            });

        ++next_chunk_to_read;
    }

    // Drain all remaining in-flight chunks
    while (!in_flight.empty()) {
        if (!drain_front(/*wait_for_front=*/true)) break;
    }

    if (abort_flag.load(std::memory_order_relaxed)) {
        if (captured_ex) std::rethrow_exception(captured_ex);
        return false;
    }

    out_crc32 = crc_calc.get();
    return true;
}

} // namespace openrar::compress
