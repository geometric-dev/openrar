#ifndef OPENRAR_TESTS_TRACKING_ALLOCATOR_HPP
#define OPENRAR_TESTS_TRACKING_ALLOCATOR_HPP

#include <cstddef>

namespace openrar::test {

// Scoped allocation monitor for native unit tests (Part of v2.3 Chunk 1, §2.3.1).
// Nesting-safe: inner scopes track only their own allocations, and restoring the
// previous scope preserves outer counts.
class ScopedAllocTracker {
public:
    ScopedAllocTracker() noexcept : prev_(current_) { current_ = this; }

    ~ScopedAllocTracker() { current_ = prev_; }

    ScopedAllocTracker(const ScopedAllocTracker&) = delete;
    ScopedAllocTracker& operator=(const ScopedAllocTracker&) = delete;
    ScopedAllocTracker(ScopedAllocTracker&&) = delete;
    ScopedAllocTracker& operator=(ScopedAllocTracker&&) = delete;

    static void record_alloc(size_t bytes) noexcept {
        if (current_) {
            ++current_->alloc_count_;
            current_->bytes_allocated_ += bytes;
        }
    }

    static void record_free(size_t /*bytes*/) noexcept {}

    void reset() noexcept {
        alloc_count_ = 0;
        bytes_allocated_ = 0;
    }

    size_t alloc_count() const noexcept { return alloc_count_; }
    size_t bytes_allocated() const noexcept { return bytes_allocated_; }

private:
    ScopedAllocTracker* prev_{nullptr};
    size_t alloc_count_{0};
    size_t bytes_allocated_{0};
    inline static thread_local ScopedAllocTracker* current_ = nullptr;
};

} // namespace openrar::test

#endif // OPENRAR_TESTS_TRACKING_ALLOCATOR_HPP
