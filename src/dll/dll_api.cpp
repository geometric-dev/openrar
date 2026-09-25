#include "openrar_dll.h"

#include "../api/abi_contract.hpp"
#include "../archive/buffer_archive.hpp"
#include "../archive/archive_reader.hpp"
#include "../archive/archive_mutator.hpp"
#include "../io/path_util.hpp"
#include "../io/extraction_journal.hpp"
#include "../compress/compressor50.hpp"
#include "../compress/decompressor50.hpp"
#include "../compress/stream_encoder.hpp"
#include "../recovery/recovery_writer.hpp"
#include "../archive/volume.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// ── Compile-time equivalence against the shared ABI contract ────────────────
// openrar_dll.h re-declares the contract in C-compatible form for C hosts;
// this TU pins it to the canonical definitions in src/api/abi_contract.hpp
// so the two can never drift apart silently.
//
// ── ABI exception policy (audited after B2) ─────────────────────────────────
// Every export that runs C++ logic wraps its body in try/catch and maps
// std::exception to an error return — an exception must never cross the
// C ABI. Consequences for new code:
//   1. A new export is not done until it has the try/catch wrapper.
//   2. Do not ADD throwing std::filesystem calls here or in code reachable
//      from here (archive_mutator, recovery_writer, archive_reader) without
//      either an ec overload or certainty that the export wrapper covers the
//      call path; prefer the ec overload unconditionally (B2 residual sweep
//      keeps mutator/recovery stragglers safe only because of the wrappers).
//   3. Allocation math from archive-controlled values goes through an
//      overflow-checked helper or SIZE_MAX pre-check (see B10 /
//      calculate_parity_buffer_size); sizing vectors from header fields
//      needs a cap (e.g. the comment-payload cap in handle_info).
static_assert(static_cast<int>(RAR_OK) == static_cast<int>(openrar::api::RAR_OK));
static_assert(static_cast<int>(RAR_ERR_PARTIAL_OK) ==
              static_cast<int>(openrar::api::RAR_ERR_PARTIAL_OK));
static_assert(static_cast<int>(RAR_ERR_NOT_RAR) == static_cast<int>(openrar::api::RAR_ERR_NOT_RAR));
static_assert(static_cast<int>(RAR_ERR_UNSUPPORTED_FEATURE) ==
              static_cast<int>(openrar::api::RAR_ERR_UNSUPPORTED_FEATURE));
static_assert(static_cast<int>(RAR_ERR_TRUNCATED) ==
              static_cast<int>(openrar::api::RAR_ERR_TRUNCATED));
static_assert(static_cast<int>(RAR_ERR_CRC_MISMATCH) ==
              static_cast<int>(openrar::api::RAR_ERR_CRC_MISMATCH));
static_assert(static_cast<int>(RAR_ERR_NOMEM) == static_cast<int>(openrar::api::RAR_ERR_NOMEM));
static_assert(static_cast<int>(RAR_ERR_IO) == static_cast<int>(openrar::api::RAR_ERR_IO));
static_assert(static_cast<int>(RAR_ERR_BAD_PASSWORD) ==
              static_cast<int>(openrar::api::RAR_ERR_BAD_PASSWORD));
static_assert(static_cast<int>(RAR_ERR_INVALID_ARG) ==
              static_cast<int>(openrar::api::RAR_ERR_INVALID_ARG));
static_assert(static_cast<int>(RAR_ERR_ABORTED) == static_cast<int>(openrar::api::RAR_ERR_ABORTED));
static_assert(static_cast<int>(RAR_ERR_ENCRYPTED) ==
              static_cast<int>(openrar::api::RAR_ERR_ENCRYPTED));
static_assert(static_cast<int>(RAR_ERR_MISSING_VOLUME) ==
              static_cast<int>(openrar::api::RAR_ERR_MISSING_VOLUME));
static_assert(static_cast<int>(RAR_ERR_BUSY) == static_cast<int>(openrar::api::RAR_ERR_BUSY));
static_assert(sizeof(openrar_archive_entry_t) == sizeof(openrar::api::ArchiveEntryOut),
              "openrar_archive_entry_t layout drifted from api::ArchiveEntryOut");
static_assert(sizeof(openrar_entry_ex_t) == 48, "openrar_entry_ex_t must stay 48 bytes");
static_assert(sizeof(openrar_archive_info_t) == 24, "openrar_archive_info_t must stay 24 bytes");
static_assert(sizeof(openrar_entry_owner_t) == 20, "openrar_entry_owner_t must stay 20 bytes");
#define OPENRAR_DLL_ENTRY_FIELD(field)                                                             \
    static_assert(offsetof(openrar_archive_entry_t, field) ==                                      \
                      offsetof(openrar::api::ArchiveEntryOut, field),                              \
                  "openrar_archive_entry_t." #field " offset drifted")
OPENRAR_DLL_ENTRY_FIELD(path_offset);
OPENRAR_DLL_ENTRY_FIELD(path_len);
OPENRAR_DLL_ENTRY_FIELD(is_dir);
OPENRAR_DLL_ENTRY_FIELD(method);
OPENRAR_DLL_ENTRY_FIELD(is_encrypted);
OPENRAR_DLL_ENTRY_FIELD(crc32);
OPENRAR_DLL_ENTRY_FIELD(size);
OPENRAR_DLL_ENTRY_FIELD(packed_size);
OPENRAR_DLL_ENTRY_FIELD(mtime);
#undef OPENRAR_DLL_ENTRY_FIELD

namespace {
// Thread-local error state from the shared contract. The DLL surface reports
// messages only (no numeric getter), so set_error() leaves the code alone.
thread_local openrar::api::ThreadError g_err;
void set_error(const std::string& msg) {
    g_err.set_message(msg);
}

// Archive handle map — shared contract table (shared_ptr pinning, report L12).
// Polymorphic since v1.3.0: buffer-backed handles keep the frozen in-memory
// MVP behavior; file-backed handles (open_file) run on the streaming reader
// and unlock encrypted/solid/multi-volume archives (openrar_dll.h "File-mode
// handles").
struct ArchiveHandleBase {
    virtual ~ArchiveHandleBase() = default;
    // Path of the archive this handle holds open, empty for buffer handles
    // (which hold no file). The mutation exports' RAR_ERR_BUSY pre-check
    // compares mutation targets against these.
    virtual std::filesystem::path path() const { return {}; }
    virtual int list(uint32_t* count, void** entries_out, void** paths_out,
                     size_t* paths_size_out) = 0;
    virtual int extract(uint32_t entry_index, uint8_t** out_ptr, size_t* out_len) = 0;
    virtual int extract_all(uint8_t** buf_out_ptr, size_t* buf_size_out, uint64_t** offsets_out_ptr,
                            uint32_t* offsets_count_out) = 0;
    virtual int extract_to_path(uint32_t entry_index, const char* dest_path,
                                openrar_progress_cb progress, openrar_cancel_cb cancel,
                                void* user) = 0;
    virtual int test(uint32_t entry_index, openrar_progress_cb progress, openrar_cancel_cb cancel,
                     void* user) = 0;
    // Extended metadata (v1.5.0): file-mode handles only — the buffer MVP
    // has no streaming reader behind it, so the default refuses.
    virtual int entry_ex(uint32_t /*entry_index*/, openrar_entry_ex_t* /*out*/,
                         void** /*extra_out*/, size_t* /*extra_size_out*/) {
        set_error("extended metadata requires a file-mode handle");
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    virtual int info(openrar_archive_info_t* /*out*/, void** /*comment_out*/,
                     size_t* /*comment_size_out*/) {
        set_error("archive info requires a file-mode handle");
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    virtual int entry_owner(uint32_t /*entry_index*/, openrar_entry_owner_t* /*owner_out*/,
                            char** /*username_out*/, char** /*groupname_out*/) {
        set_error("owner metadata requires a file-mode handle");
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    std::atomic<bool> busy{false};
    virtual int set_limits(uint64_t max_member_bytes, uint64_t max_total_bytes,
                           uint64_t max_header_count, uint64_t max_header_bytes) = 0;
};

// Claims the handle's busy flag atomically (CAS). The old unconditional
// store made every extraction path check-then-act: a concurrent set_limits
// could slip between its busy check and its non-atomic limit writes, and a
// callback re-entering the same handle could interleave two operations.
// Construction now either claims exclusively or reports claimed() == false.
struct BusyGuard {
    std::atomic<bool>& flag;
    bool claimed{false};
    explicit BusyGuard(std::atomic<bool>& f) : flag(f) {
        bool expected = false;
        claimed = flag.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }
    ~BusyGuard() {
        if (claimed) flag.store(false, std::memory_order_release);
    }
};
openrar::api::HandleTable<ArchiveHandleBase> g_handles;

// Adapter between the DLL callback convention (user, done, total) and the
// core convention (done, total, user). One shared user pointer for both
// callbacks, per the _ex listing contract. Thunks are passed to the core
// unconditionally; they no-op when the host did not register that callback.
struct ListCallbackCtx {
    openrar_progress_cb progress;
    openrar_cancel_cb cancel;
    void* user;
};
void list_progress_thunk(uint64_t done, uint64_t total, void* user) {
    auto* ctx = static_cast<ListCallbackCtx*>(user);
    if (ctx->progress) ctx->progress(ctx->user, done, total);
}
int list_cancel_thunk(void* user) {
    auto* ctx = static_cast<ListCallbackCtx*>(user);
    return ctx->cancel ? ctx->cancel(ctx->user) : 0;
}

// Human-readable detail for the listing failure codes hosts act on.
const char* list_error_message(int rc) {
    switch (rc) {
    case RAR_ERR_ENCRYPTED:
        return "archive headers are encrypted (password required to list)";
    case RAR_ERR_BAD_PASSWORD:
        return "wrong password for encrypted headers";
    case RAR_ERR_MISSING_VOLUME:
        return "a volume of the multi-volume set is missing";
    case RAR_ERR_ABORTED:
        return "listing aborted";
    default:
        return "list failed";
    }
}

// Pack parsed entries into the flat host buffers (malloc'd; freed with
// openrar_archive_list_free). Shared by every list export. Returns RAR_OK or
// RAR_ERR_NOMEM with the thread-local error set.
int pack_list_outputs(const std::vector<openrar::archive::BufferArchiveEntry>& parsed,
                      uint32_t* count, void** entries_out, void** paths_out,
                      size_t* paths_size_out) {
    std::vector<uint8_t> entries, paths;
    if (!openrar::api::pack_entries(parsed, entries, paths)) {
        set_error("pack failed");
        return RAR_ERR_NOMEM;
    }
    uint8_t* he = static_cast<uint8_t*>(std::malloc(entries.size() ? entries.size() : 1));
    if (!he) {
        set_error("oom entries");
        return RAR_ERR_NOMEM;
    }
    if (!entries.empty()) std::memcpy(he, entries.data(), entries.size());
    uint8_t* hp = static_cast<uint8_t*>(std::malloc(paths.size() ? paths.size() : 1));
    if (!hp) {
        std::free(he);
        set_error("oom paths");
        return RAR_ERR_NOMEM;
    }
    if (!paths.empty()) std::memcpy(hp, paths.data(), paths.size());
    *count = static_cast<uint32_t>(parsed.size());
    *entries_out = he;
    *paths_out = hp;
    *paths_size_out = paths.size();
    return RAR_OK;
}

// Adapter for the file-handle streaming APIs: same ListCallbackCtx thunks the
// listing exports use, wrapped in the reader's hook shape.
openrar::archive::ReaderHooks make_reader_hooks(ListCallbackCtx& ctx) {
    openrar::archive::ReaderHooks h;
    h.progress = list_progress_thunk;
    h.progress_user = &ctx;
    h.cancel = list_cancel_thunk;
    h.cancel_user = &ctx;
    return h;
}

// Destination guard (docs/invariants.md §5): true when a and b refer to the
// same file. exact/strong equivalence when both exist, else a normalized
// (case-insensitive on Windows) path comparison.
bool paths_same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec) &&
        std::filesystem::equivalent(a, b, ec)) {
        return true;
    }
    auto norm = [](const std::filesystem::path& p) {
        std::error_code ec2;
        std::filesystem::path c = std::filesystem::weakly_canonical(p, ec2);
        std::wstring s = c.wstring();
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
#endif
        return s;
    };
    return norm(a) == norm(b);
}

// Durability wrapper (docs/invariants.md §5; v1.24 M1): opens a
// crypto-random `.tmp` in the destination directory, records it in the
// per-directory journal (fsynced BEFORE the temp exists), runs `body` against
// the temp stream, and on RAR_OK flushes (FlushFileBuffers / fsync) and
// commits with the no-follow atomic rename cascade (POSIX-semantics rename on
// Windows, rename/link cascade on POSIX). Any other rc — or a commit
// failure — closes and deletes the temp, leaving dest untouched; the journal
// is closed+unlinked when the call ends. The caller owns the destination
// guard.
int durable_write_to(const std::filesystem::path& dest,
                     const std::function<int(openrar::io::FileStream&)>& body) {
    std::error_code ec;
    if (dest.has_parent_path() && !dest.parent_path().empty()) {
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) return RAR_ERR_IO; // Fail fast if parent directory cannot be created
    }
    // v1.24 M1: crypto-random temp in the destination directory, referenced
    // in a per-directory journal before it is created (durable-first), and
    // committed with the no-follow atomic rename cascade. Per-call session:
    // each DLL extraction call is its own run (plan §6.3).
    openrar::io::ExtractionSession session;
    openrar::io::AtomicWriter writer;
    if (!writer.open(session, dest)) return RAR_ERR_IO;
    int rc = body(writer.stream());
    if (rc != RAR_OK) {
        writer.abandon();
        return rc;
    }
    if (!writer.commit(openrar::io::CommitMode::ReplaceExisting)) return RAR_ERR_IO;
    return RAR_OK;
}

// ── Buffer-backed handle: the frozen in-memory MVP behavior ─────────────────
struct BufferArchiveHandle : ArchiveHandleBase {
    std::vector<uint8_t> data;
    openrar::archive::BufferArchive ba;
    std::vector<openrar::archive::BufferArchiveEntry> entries;
    openrar::archive::ExtractionLimits limits;
    openrar::archive::LimitState limit_state;
    bool has_limits{false};

    int set_limits(uint64_t max_member_bytes, uint64_t max_total_bytes, uint64_t max_header_count,
                   uint64_t max_header_bytes) override {
        // Atomic claim: closes the check-then-act window where a concurrent
        // extraction could start between the old busy.load() and these
        // non-atomic limit writes (v1.21.1 fix).
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        limits.max_member_output_bytes = max_member_bytes;
        limits.max_total_output_bytes = max_total_bytes;
        limits.max_header_count = max_header_count;
        limits.max_header_bytes = max_header_bytes;
        has_limits = true;
        busy.store(false, std::memory_order_release);
        return RAR_OK;
    }

    int list(uint32_t* count, void** entries_out, void** paths_out,
             size_t* paths_size_out) override {
        return pack_list_outputs(entries, count, entries_out, paths_out, paths_size_out);
    }
    int extract(uint32_t entry_index, uint8_t** out_ptr, size_t* out_len) override {
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        std::vector<uint8_t> out;
        int rc = ba.extract(data.data(), data.size(), entry_index, out,
                            has_limits ? &limits : nullptr, &limit_state);
        if (rc != RAR_OK) {
            if (rc == RAR_ERR_LIMIT_EXCEEDED) set_error("resource limit exceeded");
            return rc;
        }
        *out_ptr = openrar::api::heap_dup(out.data(), out.size());
        if (!*out_ptr && !out.empty()) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        *out_len = out.size();
        return RAR_OK;
    }
    int extract_all(uint8_t** buf_out_ptr, size_t* buf_size_out, uint64_t** offsets_out_ptr,
                    uint32_t* offsets_count_out) override {
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = ba.extract_all(data.data(), data.size(), files, nullptr, nullptr, nullptr, nullptr,
                                has_limits ? &limits : nullptr, &limit_state);
        if (rc != RAR_OK) {
            if (rc == RAR_ERR_LIMIT_EXCEEDED) set_error("resource limit exceeded");
            return rc;
        }

        if (files.empty()) {
            *buf_out_ptr = nullptr;
            *buf_size_out = 0;
            *offsets_out_ptr = nullptr;
            *offsets_count_out = 0;
            return RAR_OK;
        }

        // Checked 64-bit multiplication to prevent integer overflow
        if (files.size() > SIZE_MAX / (sizeof(uint64_t) * 2)) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }

        size_t total = 0;
        for (auto& f : files) {
            if (f.second.size() > SIZE_MAX - total) {
                set_error("oom");
                return RAR_ERR_NOMEM;
            }
            total += f.second.size();
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(total ? total : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        uint64_t* offs = static_cast<uint64_t*>(std::malloc(files.size() * sizeof(uint64_t) * 2));
        if (!offs) {
            std::free(buf);
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        size_t off = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            auto& p = files[i].second;
            if (!p.empty()) std::memcpy(buf + off, p.data(), p.size());
            offs[i * 2] = off;
            offs[i * 2 + 1] = p.size();
            off += p.size();
        }
        *buf_out_ptr = buf;
        *buf_size_out = total;
        *offsets_out_ptr = offs;
        *offsets_count_out = static_cast<uint32_t>(files.size());
        return RAR_OK;
    }
    int extract_to_path(uint32_t entry_index, const char* dest_path, openrar_progress_cb progress,
                        openrar_cancel_cb cancel, void* user) override {
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        ListCallbackCtx ctx{progress, cancel, user};
        if (ctx.cancel && ctx.cancel(ctx.user)) return RAR_ERR_ABORTED;
        const auto& e = entries[entry_index];
        const uint64_t total = e.is_dir ? 0 : e.size;
        if (e.is_dir) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(dest_path), ec);
            if (ctx.progress) ctx.progress(ctx.user, 0, 0);
            return RAR_OK;
        }
        std::vector<uint8_t> out;
        int rc = ba.extract(data.data(), data.size(), entry_index, out,
                            has_limits ? &limits : nullptr, &limit_state);
        if (rc != RAR_OK) {
            if (rc == RAR_ERR_LIMIT_EXCEEDED) set_error("resource limit exceeded");
            return rc;
        }
        if (ctx.progress) ctx.progress(ctx.user, 0, total);
        rc = durable_write_to(std::filesystem::u8path(dest_path), [&](openrar::io::FileStream& f) {
            if (!out.empty() && f.write(out.data(), out.size()) != out.size()) return RAR_ERR_IO;
            return RAR_OK;
        });
        if (rc != RAR_OK) return rc;
        if (ctx.progress) ctx.progress(ctx.user, total, total);
        return RAR_OK;
    }
    int test(uint32_t, openrar_progress_cb, openrar_cancel_cb, void*) override {
        set_error("test is not supported on buffer handles");
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
};

// ── File-backed handle: streaming reader, encrypted/solid/volume capable ────
struct FileArchiveHandle : ArchiveHandleBase {
    std::unique_ptr<openrar::archive::ArchiveReader> reader;
    // DLL-visible entries: file entries only (service headers are internal
    // blocks), mapped to the 64-byte contract shape.
    std::vector<openrar::archive::BufferArchiveEntry> entries;
    // DLL entry index → reader entries() index.
    std::vector<size_t> reader_index;
    openrar::archive::ExtractionLimits limits;
    openrar::archive::LimitState limit_state;
    bool has_limits{false};

    int set_limits(uint64_t max_member_bytes, uint64_t max_total_bytes, uint64_t max_header_count,
                   uint64_t max_header_bytes) override {
        // Atomic claim: closes the check-then-act window where a concurrent
        // extraction could start between the old busy.load() and these
        // non-atomic limit writes (v1.21.1 fix).
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        limits.max_member_output_bytes = max_member_bytes;
        limits.max_total_output_bytes = max_total_bytes;
        limits.max_header_count = max_header_count;
        limits.max_header_bytes = max_header_bytes;
        has_limits = true;
        busy.store(false, std::memory_order_release);
        return RAR_OK;
    }

    std::filesystem::path path() const override { return reader->path(); }

    // Unix seconds (+ optional ns) / FILETIME → FILETIME (UTC, 100ns).
    // 0 when the field is absent from the archive.
    static uint64_t to_filetime(uint64_t win, uint32_t unix_sec, uint32_t ns, bool has_ns) {
        if (win != 0) return win;
        if (unix_sec != 0) {
            uint64_t ft = (static_cast<uint64_t>(unix_sec) + 11644473600ULL) * 10000000ULL;
            if (has_ns) ft += ns / 100;
            return ft;
        }
        return 0;
    }

    void build_entry_map() {
        entries.clear();
        reader_index.clear();
        for (size_t i = 0; i < reader->entries().size(); ++i) {
            const auto& re = reader->entries()[i];
            if (re.header.is_service) continue;
            openrar::archive::BufferArchiveEntry e;
            e.path = re.header.file_name;
            e.is_dir = (re.header.file_flags & openrar::format::FHFL_DIRECTORY) != 0;
            e.size = re.header.unp_size;
            e.packed_size = re.data_size; // extent sum (merged across volumes)
            e.method = static_cast<int>(re.header.method);
            e.win_size = re.header.win_size;
            e.is_encrypted = re.header.is_encrypted;
            e.crc32 = re.header.has_crc32 ? re.header.data_crc32 : 0;
            e.mtime = openrar::archive::dos_time_to_unix(re.header.utime_unix);
            entries.push_back(std::move(e));
            reader_index.push_back(i);
        }
    }

    int list(uint32_t* count, void** entries_out, void** paths_out,
             size_t* paths_size_out) override {
        return pack_list_outputs(entries, count, entries_out, paths_out, paths_size_out);
    }
    int extract(uint32_t entry_index, uint8_t** out_ptr, size_t* out_len) override {
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        std::vector<uint8_t> out;
        int rc = reader->extract_entry_to_memory(reader_index[entry_index], out,
                                                 OPENRAR_MAX_HEAP_EXTRACT_SIZE, {},
                                                 has_limits ? &limits : nullptr, &limit_state);
        if (rc != RAR_OK) {
            if (rc == RAR_ERR_LIMIT_EXCEEDED)
                set_error("resource limit exceeded");
            else
                set_error(rc == RAR_ERR_NOMEM
                              ? "entry exceeds the 256 MiB in-memory extract cap; use "
                                "openrar_archive_handle_extract_to_path"
                              : "extract failed");
            return rc;
        }
        *out_ptr = openrar::api::heap_dup(out.data(), out.size());
        if (!*out_ptr && !out.empty()) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        *out_len = out.size();
        return RAR_OK;
    }
    int extract_all(uint8_t**, size_t*, uint64_t**, uint32_t*) override {
        set_error("extract_all is not supported on file handles; extract per entry with "
                  "openrar_archive_handle_extract_to_path");
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    int extract_to_path(uint32_t entry_index, const char* dest_path, openrar_progress_cb progress,
                        openrar_cancel_cb cancel, void* user) override {
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        ListCallbackCtx ctx{progress, cancel, user};
        openrar::archive::ReaderHooks hooks = make_reader_hooks(ctx);
        const auto& re = reader->entries()[reader_index[entry_index]];

        // Directory: materialize through the reader's contained directory
        // walk (v1.24 M2) — no unanchored create_directories. Single (0, 0)
        // progress callback.
        if (re.header.file_flags & openrar::format::FHFL_DIRECTORY) {
            reader->extract_entry(re, std::filesystem::u8path(dest_path), "");
            if (ctx.progress) ctx.progress(ctx.user, 0, 0);
            return RAR_OK;
        }
        // Links: delegate to the reader's safe-link rules (no bulk payload).
        if (re.header.redir_type != 0) {
            int rc = reader->extract_entry(re, std::filesystem::u8path(dest_path), "") ? RAR_OK
                                                                                       : RAR_ERR_IO;
            if (rc == RAR_OK && ctx.progress) ctx.progress(ctx.user, 0, 0);
            return rc;
        }

        // Destination guard: never clobber the archive (or any volume of its set).
        const std::filesystem::path dest = std::filesystem::u8path(dest_path);
        for (const auto& vol : reader->volume_paths()) {
            if (paths_same_file(dest, vol)) {
                set_error("destination path cannot be the archive file");
                return RAR_ERR_INVALID_ARG;
            }
        }
        return durable_write_to(dest, [&](openrar::io::FileStream& f) {
            int rc = reader->extract_entry_stream(reader_index[entry_index], f, hooks,
                                                  has_limits ? &limits : nullptr, &limit_state);
            if (rc == RAR_ERR_LIMIT_EXCEEDED) set_error("resource limit exceeded");
            return rc;
        });
    }
    int test(uint32_t entry_index, openrar_progress_cb progress, openrar_cancel_cb cancel,
             void* user) override {
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        BusyGuard bg(busy);
        if (!bg.claimed) {
            set_error("handle is busy with active operation");
            return RAR_ERR_BUSY;
        }
        ListCallbackCtx ctx{progress, cancel, user};
        openrar::archive::ReaderHooks hooks = make_reader_hooks(ctx);
        int rc = reader->test_entry_stream(reader_index[entry_index], hooks,
                                           has_limits ? &limits : nullptr, &limit_state);
        if (rc == RAR_ERR_LIMIT_EXCEEDED)
            set_error("resource limit exceeded");
        else if (rc == RAR_ERR_MISSING_VOLUME)
            set_error("missing volume: " + openrar::io::u8_str(reader->missing_volume_path()));
        else if (rc == RAR_ERR_BAD_PASSWORD)
            set_error("wrong password for encrypted entry");
        else if (rc == RAR_ERR_ENCRYPTED)
            set_error("entry is encrypted (no password supplied at open)");
        else if (rc == RAR_ERR_CRC_MISMATCH)
            set_error("checksum mismatch");
        else if (rc == RAR_ERR_TRUNCATED)
            set_error("packed stream ended early");
        else if (rc == RAR_ERR_ABORTED)
            set_error("test aborted");
        return rc;
    }

    // Extended metadata (v1.5.0): straight from the cached walk's headers.
    int entry_ex(uint32_t entry_index, openrar_entry_ex_t* out, void** extra_out,
                 size_t* extra_size_out) override {
        if (!out || !extra_out || !extra_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out = openrar_entry_ex_t{};
        *extra_out = nullptr;
        *extra_size_out = 0;
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        const auto& re = reader->entries()[reader_index[entry_index]];
        const auto& h = re.header;
        out->attrs = static_cast<uint32_t>(h.attributes);
        out->host_os = h.host_os;
        out->mtime_ft = to_filetime(h.mtime_win, h.htime_mtime_unix, h.mtime_ns, h.has_mtime_ns);
        out->ctime_ft = to_filetime(h.ctime_win, h.htime_ctime_unix, h.ctime_ns, h.has_ctime_ns);
        out->atime_ft = to_filetime(h.atime_win, h.htime_atime_unix, h.atime_ns, h.has_atime_ns);
        uint32_t flags = 0;
        if (h.is_solid) flags |= OPENRAR_ENTRY_FLAG_SOLID;
        if (h.is_encrypted) flags |= OPENRAR_ENTRY_FLAG_ENCRYPTED;
        if (h.redir_type != 0) flags |= OPENRAR_ENTRY_FLAG_REDIR;
        if (re.split_before) flags |= OPENRAR_ENTRY_FLAG_SPLIT_BEFORE;
        if (re.split_after) flags |= OPENRAR_ENTRY_FLAG_SPLIT_AFTER;
        if (out->mtime_ft) flags |= OPENRAR_ENTRY_FLAG_HAS_MTIME;
        if (out->ctime_ft) flags |= OPENRAR_ENTRY_FLAG_HAS_CTIME;
        if (out->atime_ft) flags |= OPENRAR_ENTRY_FLAG_HAS_ATIME;
        if (h.file_flags & openrar::format::FHFL_DIRECTORY) flags |= OPENRAR_ENTRY_FLAG_DIRECTORY;
        if (h.has_file_version) flags |= OPENRAR_ENTRY_FLAG_HAS_VERSION;
        if (h.has_owner) flags |= OPENRAR_ENTRY_FLAG_HAS_OWNER;
        out->flags = flags;
        out->win_size =
            h.win_size > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(h.win_size);
        out->redir_type = h.redir_type;
        out->version_needed = h.unp_ver;
        if (h.redir_type != 0) {
            const std::string& target = h.redir_target;
            uint8_t* buf = static_cast<uint8_t*>(std::malloc(target.size() + 1));
            if (!buf) {
                set_error("oom");
                return RAR_ERR_NOMEM;
            }
            std::memcpy(buf, target.c_str(), target.size() + 1);
            *extra_out = buf;
            *extra_size_out = target.size();
        }
        return RAR_OK;
    }

    // Archive-level properties: main-header flags, volume provenance and
    // RR size come from the cached walk; the CMT payload is read lazily.
    int info(openrar_archive_info_t* out, void** comment_out, size_t* comment_size_out) override {
        if (!out || !comment_out || !comment_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out = openrar_archive_info_t{};
        *comment_out = nullptr;
        *comment_size_out = 0;
        const auto& mb = reader->main_block();
        out->flags = static_cast<uint32_t>(mb.arc_flags);
        out->volume_index = static_cast<uint32_t>(mb.vol_number);
        // The file-mode open is strict (a successfully opened set had every
        // volume of the chain present), so the count is always determinable.
        out->volume_count = static_cast<uint32_t>(reader->volume_paths().size());
        for (const auto& e : reader->entries()) {
            if (e.header.is_service && e.header.service_type == "RR" && e.data_size > 0) {
                out->recovery_size = e.data_size;
                break;
            }
        }
        // Archive comment: find the CMT service, read its payload on demand.
        // data_size comes from a (possibly crafted) archive header, so the
        // allocation is capped: a multi-GiB claimed comment would otherwise
        // turn into a huge speculative vector before the read fails.
        for (const auto& e : reader->entries()) {
            if (!(e.header.is_service && e.header.service_type == "CMT")) continue;
            constexpr openrar::core::uint64 MAX_COMMENT_PAYLOAD = 16ull * 1024 * 1024;
            std::vector<uint8_t> payload;
            if (!e.header.sub_data.empty()) {
                payload.assign(e.header.sub_data.begin(), e.header.sub_data.end());
            } else if (e.data_size > 0) {
                if (e.data_size > MAX_COMMENT_PAYLOAD) {
                    set_error("archive comment too large");
                    return RAR_ERR_UNSUPPORTED_FEATURE;
                }
                std::vector<uint8_t> packed(static_cast<size_t>(e.data_size));
                {
                    auto& s = reader->stream();
                    if (!s.seek(static_cast<openrar::core::int64>(e.data_offset),
                                openrar::io::SeekOrigin::Begin)) {
                        set_error("cannot read archive comment");
                        return RAR_ERR_IO;
                    }
                    if (s.read(packed.data(), packed.size()) != packed.size()) {
                        set_error("cannot read archive comment");
                        return RAR_ERR_IO;
                    }
                }
                if (e.header.method > 0) {
                    std::vector<openrar::core::byte> out_bytes;
                    openrar::compress::Decompressor50 dec(
                        e.header.win_size ? static_cast<size_t>(e.header.win_size)
                                          : (1024u * 1024));
                    if (!dec.decompress_to_vector(packed.data(), packed.size(), out_bytes)) {
                        return RAR_OK; // undecodable comment reported as absent
                    }
                    payload.assign(out_bytes.begin(), out_bytes.end());
                } else {
                    payload = std::move(packed); // stored comment
                }
            } else {
                continue; // zero-length comment slot
            }
            if (e.header.has_crc32) {
                openrar::crypto::Crc32 crc;
                crc.update(payload.data(), payload.size());
                if (crc.get() != e.header.data_crc32) return RAR_OK; // absent, not IO
            }
            uint8_t* buf = static_cast<uint8_t*>(std::malloc(payload.size() ? payload.size() : 1));
            if (!buf) {
                set_error("oom");
                return RAR_ERR_NOMEM;
            }
            if (!payload.empty()) std::memcpy(buf, payload.data(), payload.size());
            out->comment_len = static_cast<uint32_t>(payload.size());
            *comment_out = buf;
            *comment_size_out = payload.size();
            return RAR_OK;
        }
        return RAR_OK; // no CMT service
    }

    int entry_owner(uint32_t entry_index, openrar_entry_owner_t* owner_out, char** username_out,
                    char** groupname_out) override {
        if (!owner_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *owner_out = openrar_entry_owner_t{};
        if (username_out) *username_out = nullptr;
        if (groupname_out) *groupname_out = nullptr;
        if (entry_index >= entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        const auto& re = reader->entries()[reader_index[entry_index]];
        const auto& h = re.header;
        if (!h.has_owner) {
            return RAR_OK;
        }
        uint32_t flags = 0;
        if (h.has_owner_uid) {
            owner_out->uid = h.owner_uid;
            flags |= OPENRAR_OWNER_FLAG_HAS_UID;
        }
        if (h.has_owner_gid) {
            owner_out->gid = h.owner_gid;
            flags |= OPENRAR_OWNER_FLAG_HAS_GID;
        }
        // String contract (openrar_dll.h): when a HAS_* flag is set the
        // corresponding out param receives a malloc'd NUL-terminated UTF-8
        // string. An allocation failure must therefore clear the flag and
        // fail with RAR_ERR_NOMEM — never RAR_OK with a flag and a NULL
        // string (v1.21.1 fix).
        if (!h.owner_user.empty()) {
            if (username_out) {
                char* u = static_cast<char*>(std::malloc(h.owner_user.size() + 1));
                if (!u) {
                    if (*groupname_out) std::free(*groupname_out);
                    *groupname_out = nullptr;
                    owner_out->flags = 0;
                    set_error("out of memory");
                    return RAR_ERR_NOMEM;
                }
                std::memcpy(u, h.owner_user.c_str(), h.owner_user.size() + 1);
                *username_out = u;
                flags |= OPENRAR_OWNER_FLAG_HAS_USER;
            } else {
                flags |= OPENRAR_OWNER_FLAG_HAS_USER;
            }
        }
        if (!h.owner_group.empty()) {
            if (groupname_out) {
                char* g = static_cast<char*>(std::malloc(h.owner_group.size() + 1));
                if (!g) {
                    if (*username_out) std::free(*username_out);
                    *username_out = nullptr;
                    owner_out->flags = 0;
                    set_error("out of memory");
                    return RAR_ERR_NOMEM;
                }
                std::memcpy(g, h.owner_group.c_str(), h.owner_group.size() + 1);
                *groupname_out = g;
                flags |= OPENRAR_OWNER_FLAG_HAS_GROUP;
            } else {
                flags |= OPENRAR_OWNER_FLAG_HAS_GROUP;
            }
        }
        owner_out->flags = flags;
        return RAR_OK;
    }
};
} // namespace

static_assert(static_cast<int>(RAR_ERR_BUSY) == -14,
              "RAR_ERR_BUSY must be -14 to match openrar_dll.h and rar_errors.hpp");
static_assert(static_cast<int>(RAR_ERR_LIMIT_EXCEEDED) == -15,
              "RAR_ERR_LIMIT_EXCEEDED must be -15 to match openrar_dll.h and RarErrorCode.ts");

extern "C" {

int OPENRAR_DLL_CALL openrar_version(void) {
    return OPENRAR_DLL_API_VERSION;
}
int OPENRAR_DLL_CALL openrar_archive_version(void) {
    return 1;
}
const char* OPENRAR_DLL_CALL openrar_package_version_string(void) {
    return OPENRAR_VERSION_STRING;
}
uint64_t OPENRAR_DLL_CALL openrar_abi_features(void) {
    return OPENRAR_ABI_FEATURE_LIST_PROGRESS | OPENRAR_ABI_FEATURE_LIST_PASSWORD |
           OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS | OPENRAR_ABI_FEATURE_FILE_HANDLE |
           OPENRAR_ABI_FEATURE_MUTATION | OPENRAR_ABI_FEATURE_ENTRY_EX |
           OPENRAR_ABI_FEATURE_PACKAGE_VERSION | OPENRAR_ABI_FEATURE_SET_LIMITS |
           OPENRAR_ABI_FEATURE_REPAIR | OPENRAR_ABI_FEATURE_CREATE | OPENRAR_ABI_FEATURE_FILTERS |
           OPENRAR_ABI_FEATURE_OWNER | OPENRAR_ABI_FEATURE_DICT_EX |
           OPENRAR_ABI_FEATURE_VOL_ENCRYPT | OPENRAR_ABI_FEATURE_REC_VOL |
           OPENRAR_ABI_FEATURE_PARALLEL_COMPRESS;
}

void* OPENRAR_DLL_CALL openrar_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void OPENRAR_DLL_CALL openrar_free(void* ptr) {
    std::free(ptr);
}
void* OPENRAR_DLL_CALL openrar_archive_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void OPENRAR_DLL_CALL openrar_archive_free(void* ptr) {
    std::free(ptr);
}

int OPENRAR_DLL_CALL openrar_archive_get_error(char* buf, int buf_len) {
    return g_err.copy_message_to(buf, buf_len);
}
int OPENRAR_DLL_CALL openrar_last_error(char* buf, int buf_len) {
    return openrar_archive_get_error(buf, buf_len);
}

// ── Block codec ──────────────────────────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_compress(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                      size_t* out_len, int method) {
    return openrar_compress2(src, src_len, out_ptr, out_len, method, 2 * 1024 * 1024);
}
int OPENRAR_DLL_CALL openrar_compress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                       size_t* out_len, int method, size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        if (win_size > openrar::api::MAX_WIN_SIZE) return 0;
        *out_ptr = nullptr;
        *out_len = 0;
        if (src_len == 0) return 1;
        if (!src && src_len != 0) return 0;
        std::vector<openrar::core::byte> out;
        // clamp method
        if (method < 0) method = 3;
        if (method > 5) method = 5;
        if (method == 0) {
            out.assign(src, src + src_len);
        } else {
            if (!openrar::compress::Compressor50::compress_buffer(src, src_len, out, method,
                                                                  win_size))
                return 0;
        }
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return 1;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return 0;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 1;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_decompress(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                        size_t* out_len) {
    // Must mirror openrar_compress's 2 MiB default: raw block streams carry
    // no dictionary-size header, so the decompress default has to cover the
    // compress default (a larger window is always safe).
    return openrar_decompress2(src, src_len, out_ptr, out_len, 2 * 1024 * 1024);
}
int OPENRAR_DLL_CALL openrar_decompress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                         size_t* out_len, size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        if (win_size > openrar::api::MAX_WIN_SIZE) return 0;
        *out_ptr = nullptr;
        *out_len = 0;
        if (src_len == 0) return 1;
        if (!src) return 0;
        openrar::compress::Decompressor50 dec(win_size ? win_size : 2 * 1024 * 1024);
        std::vector<openrar::core::byte> out;
        size_t prior = out.size();
        bool ok = dec.decompress_to_vector(src, src_len, out);
        if (!ok) {
            out.resize(prior);
            return 0;
        }
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return 1;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return 0;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 1;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── Archive — buffer API ────────────────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_archive_list(const uint8_t* data, size_t size, uint32_t* count,
                                          void** entries_out, void** paths_out,
                                          size_t* paths_size_out) {
    try {
        if (!data || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed);
        if (rc != RAR_OK) {
            set_error("list failed");
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
void OPENRAR_DLL_CALL openrar_archive_list_free(void* entries, void* paths, size_t) {
    std::free(entries);
    std::free(paths);
}

int OPENRAR_DLL_CALL openrar_archive_extract(const uint8_t* data, size_t size, uint32_t entry_index,
                                             uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!data || !out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed);
        if (rc != RAR_OK) return rc;
        if (entry_index >= parsed.size()) {
            set_error("entry_index out of range");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<uint8_t> out;
        rc = ba.extract(data, size, entry_index, out);
        if (rc != RAR_OK) return rc;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() ? out.size() : 1));
        if (!buf && !out.empty()) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = out.empty() ? nullptr : buf;
        *out_len = out.size();
        if (out.empty()) std::free(buf);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_all(const uint8_t* data, size_t size,
                                                 uint8_t** buf_out_ptr, size_t* buf_size_out,
                                                 uint64_t** offsets_out_ptr,
                                                 uint32_t* offsets_count_out) {
    try {
        if (!data || !buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        openrar::archive::BufferArchive ba;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = ba.extract_all(data, size, files);
        if (rc != RAR_OK) return rc;

        if (files.empty()) {
            *buf_out_ptr = nullptr;
            *buf_size_out = 0;
            *offsets_out_ptr = nullptr;
            *offsets_count_out = 0;
            return RAR_OK;
        }

        // Checked 64-bit multiplication to prevent integer overflow
        if (files.size() > SIZE_MAX / (sizeof(uint64_t) * 2)) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }

        size_t total = 0;
        for (auto& f : files) {
            if (f.second.size() > SIZE_MAX - total) {
                set_error("oom");
                return RAR_ERR_NOMEM;
            }
            total += f.second.size();
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(total ? total : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        uint64_t* offs = static_cast<uint64_t*>(std::malloc(files.size() * sizeof(uint64_t) * 2));
        if (!offs) {
            std::free(buf);
            set_error("oom offs");
            return RAR_ERR_NOMEM;
        }
        size_t off = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            auto& p = files[i].second;
            if (!p.empty()) std::memcpy(buf + off, p.data(), p.size());
            offs[i * 2] = off;
            offs[i * 2 + 1] = p.size();
            off += p.size();
        }
        *buf_out_ptr = buf;
        *buf_size_out = total;
        *offsets_out_ptr = offs;
        *offsets_count_out = static_cast<uint32_t>(files.size());
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create(const uint8_t* const* paths_arr,
                                            const uint8_t* const* data_arr, const size_t* sizes_arr,
                                            uint32_t file_count, int method, uint32_t window_log2,
                                            uint8_t** out_ptr, size_t* out_len) {
    try {
        if ((file_count > 0 && (!paths_arr || !data_arr || !sizes_arr)) || !out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        if (method != 0 && method != 3 && method != 5) {
            set_error("method must be 0,3,5");
            return RAR_ERR_INVALID_ARG;
        }
        if (window_log2 < 1 || window_log2 > 4) {
            set_error("window_log2 must be 1..4");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        files.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            const uint8_t* p = paths_arr[i];
            const uint8_t* d = data_arr[i];
            size_t s = sizes_arr[i];
            if (!p || (!d && s != 0)) {
                set_error("null path/data");
                return RAR_ERR_INVALID_ARG;
            }
            if (!d) d = reinterpret_cast<const uint8_t*>("");
            size_t plen = 0;
            while (plen < 4096 && p[plen] != 0) ++plen;
            std::string path(reinterpret_cast<const char*>(p), plen);
            bool is_dir = !path.empty() && path.back() == '/';
            std::string err;
            if (!openrar::archive::validate_archive_path(path, is_dir, err)) {
                set_error("path " + path + ": " + err);
                return RAR_ERR_INVALID_ARG;
            }
            files.emplace_back(std::move(path), std::vector<uint8_t>(d, d + s));
        }
        std::vector<uint8_t> out;
        int rc = openrar::archive::create_archive(files, out, method, window_log2);
        if (rc != RAR_OK) return rc;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() ? out.size() : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── Handle API ───────────────────────────────────────────────────────────────
uint32_t OPENRAR_DLL_CALL openrar_archive_open(const uint8_t* data, size_t size) {
    try {
        if (!data || size == 0) {
            set_error("null argument");
            return 0;
        }
        auto h = std::make_shared<BufferArchiveHandle>();
        h->data.assign(data, data + size);
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = h->ba.list(h->data.data(), h->data.size(), parsed);
        if (rc != RAR_OK) {
            set_error("open failed");
            return 0;
        }
        h->entries = std::move(parsed);
        return g_handles.insert(std::move(h));
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
void OPENRAR_DLL_CALL openrar_archive_close(uint32_t handle) {
    g_handles.erase(handle);
}
uint32_t OPENRAR_DLL_CALL openrar_archive_open_ex(const uint8_t* data, size_t size,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!data || size == 0) {
            set_error("null argument");
            return 0;
        }
        auto h = std::make_shared<BufferArchiveHandle>();
        h->data.assign(data, data + size);
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = h->ba.list(h->data.data(), h->data.size(), parsed, list_progress_thunk, &ctx,
                            list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(rc == RAR_ERR_ABORTED ? "open aborted" : "open failed");
            return 0;
        }
        h->entries = std::move(parsed);
        return g_handles.insert(std::move(h));
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_list(uint32_t handle, uint32_t* count,
                                                 void** entries_out, void** paths_out,
                                                 size_t* paths_size_out) {
    try {
        if (!count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        return h->list(count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_extract(uint32_t handle, uint32_t entry_index,
                                                    uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->extract(entry_index, out_ptr, out_len);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr,
                                                        size_t* buf_size_out,
                                                        uint64_t** offsets_out_ptr,
                                                        uint32_t* offsets_count_out) {
    try {
        if (!buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->extract_all(buf_out_ptr, buf_size_out, offsets_out_ptr, offsets_count_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── File-mode handles (v1.3.0) ───────────────────────────────────────────────
uint32_t OPENRAR_DLL_CALL openrar_archive_open_file(const char* arc_path, const char* password_utf8,
                                                    openrar_progress_cb progress,
                                                    openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path) {
            set_error("null argument");
            return 0;
        }
        auto h = std::make_shared<FileArchiveHandle>();
        h->reader = std::make_unique<openrar::archive::ArchiveReader>();
        ListCallbackCtx ctx{progress, cancel, user};
        int status = RAR_OK;
        std::string detail;
        const bool ok =
            h->reader->open_ex(std::filesystem::u8path(arc_path),
                               password_utf8 ? std::string(password_utf8) : std::string(), status,
                               detail, make_reader_hooks(ctx), /*strict_volumes=*/true);
        if (!ok) {
            if (detail.empty()) {
                detail = "open failed";
                if (status == RAR_ERR_BAD_PASSWORD)
                    detail = "wrong password for encrypted headers";
                else if (status == RAR_ERR_ENCRYPTED)
                    detail = "archive headers are encrypted (password required to list)";
            }
            set_error(detail);
            return 0;
        }
        h->build_entry_map();
        return g_handles.insert(std::move(h));
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_extract_to_path(uint32_t handle, uint32_t entry_index,
                                                            const char* dest_path,
                                                            openrar_progress_cb progress,
                                                            openrar_cancel_cb cancel, void* user) {
    try {
        if (!dest_path) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->extract_to_path(entry_index, dest_path, progress, cancel, user);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_test(uint32_t handle, uint32_t entry_index,
                                                 openrar_progress_cb progress,
                                                 openrar_cancel_cb cancel, void* user) {
    try {
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->test(entry_index, progress, cancel, user);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_handle_set_limits(uint32_t handle, uint64_t max_member_bytes,
                                                       uint64_t max_total_bytes,
                                                       uint64_t max_header_count,
                                                       uint64_t max_header_bytes) {
    try {
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->set_limits(max_member_bytes, max_total_bytes, max_header_count, max_header_bytes);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

} // extern "C" helpers C++ linkage

// ── Archive mutation (v1.4.0; free functions over ArchiveMutator) ────────────
// (docs/dll-integration-spec.md §6.12)
namespace {

// Pre-check shared by both mutation exports: the archive must exist on disk
// (RAR_ERR_IO) and no open file-mode handle in this process may hold it
// (RAR_ERR_BUSY — open_file keeps the volume open with FILE_SHARE_READ, so
// the mutator's atomic replace over it fails opaquely on Windows; failing
// up front beats a sharing violation mid-rename).
int mutation_precheck(const char* arc_path, bool must_exist = true) {
    std::error_code ec;
    const std::filesystem::path arc = std::filesystem::u8path(arc_path);
    if (must_exist) {
        if (!std::filesystem::exists(arc, ec) || ec) {
            set_error("archive not found");
            return RAR_ERR_IO;
        }
    }
    if (std::filesystem::exists(arc, ec) && !ec) {
        for (const auto& h : g_handles.snapshot()) {
            const std::filesystem::path p = h->path();
            if (p.empty()) continue; // buffer handles hold no file
            if (paths_same_file(p, arc)) {
                set_error("archive is open in a handle; close it before mutating");
                return RAR_ERR_BUSY;
            }
        }
    }
    return RAR_OK;
}

// Open + classify for the mutation exports: locked, multi-volume and
// header-encrypted archives are refused with RAR_ERR_UNSUPPORTED_FEATURE
// (the mutation surface takes no password); anything else that fails the
// walk is RAR_ERR_IO. On RAR_OK `reader` is left OPEN for the caller's
// index translation — close it before invoking the mutator, whose final
// rename must not face our own open stream.
int mutation_open(const std::filesystem::path& arc,
                  std::unique_ptr<openrar::archive::ArchiveReader>& reader, std::string& detail) {
    auto r = std::make_unique<openrar::archive::ArchiveReader>();
    int status = RAR_OK;
    std::string open_detail;
    // A failed close()s the reader and resets its per-archive flags, so the
    // -hp verdict arrives as the status code, not via saw_crypt_header().
    // With the empty password used here, BAD_PASSWORD can only mean a
    // header-encrypted archive (the derived keys garbage out on HEAD_CRYPT),
    // so both that and ENCRYPTED map to the pinned refusal below.
    if (!r->open_ex(arc, /*password=*/"", status, open_detail)) {
        if (status == RAR_ERR_ENCRYPTED || status == RAR_ERR_BAD_PASSWORD) {
            detail = "mutating header-encrypted archive requires password";
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
        if (status == RAR_ERR_UNSUPPORTED_FEATURE) {
            detail = open_detail; // e.g. unsupported HEAD_CRYPT crypto version
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
        detail = "cannot open archive";
        return RAR_ERR_IO;
    }
    if (r->is_locked()) {
        detail = "archive is locked";
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    if (r->is_volume()) {
        detail = "cannot mutate a multi-volume archive";
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    reader = std::move(r);
    return RAR_OK;
}

} // namespace

extern "C" {

int OPENRAR_DLL_CALL openrar_archive_delete_entries_file(const char* arc_path,
                                                         const uint32_t* entry_indices,
                                                         uint32_t count) {
    try {
        if (!arc_path || !entry_indices || count == 0) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        int pre = mutation_precheck(arc_path);
        if (pre != RAR_OK) return pre;

        const std::filesystem::path arc = std::filesystem::u8path(arc_path);
        std::unique_ptr<openrar::archive::ArchiveReader> reader;
        std::string detail;
        int cls = mutation_open(arc, reader, detail);
        if (cls != RAR_OK) {
            set_error(detail);
            return cls;
        }

        // Translate handle-listing indices to header offsets with our own
        // walk (docs/dll-integration-spec.md §6.12): file entries only, in
        // listing order — the same mapping openrar_archive_handle_list
        // exposes on a file handle. Deletion matches by header offset,
        // never by name, so names containing '*'/'?' are safe.
        const std::vector<openrar::archive::ArchiveEntry>& entries = reader->entries();
        std::vector<size_t> file_indices;
        file_indices.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].header.is_service) file_indices.push_back(i);
        }
        std::vector<openrar::core::uint64> offsets;
        offsets.reserve(count);
        std::unordered_set<uint32_t> seen;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t idx = entry_indices[i];
            if (idx >= file_indices.size()) {
                set_error("entry index " + std::to_string(idx) + " out of range (archive has " +
                          std::to_string(file_indices.size()) + " entries)");
                return RAR_ERR_INVALID_ARG;
            }
            if (seen.insert(idx).second)
                offsets.push_back(entries[file_indices[idx]].header_offset);
        }
        reader.reset(); // close the walk before the mutator's rename

        int rc = openrar::archive::ArchiveMutator::delete_entries_by_index(arc, offsets, detail);
        if (rc != RAR_OK) set_error(detail.empty() ? "delete failed" : detail);
        return rc;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_add_files_file(const char* arc_path,
                                                    const char* const* src_paths,
                                                    const char* const* arc_names,
                                                    uint32_t file_count, int method,
                                                    uint32_t window_log2) {
    try {
        if (!arc_path || !src_paths || !arc_names || file_count == 0) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        if (method != 0 && method != 3 && method != 5) {
            set_error("method must be 0 (store), 3 (normal) or 5 (best)");
            return RAR_ERR_INVALID_ARG;
        }
        if (method != 0 && (window_log2 < 1 || window_log2 > 4)) {
            set_error("window_log2 must be 1..4 (128 KiB .. 1 MiB)");
            return RAR_ERR_INVALID_ARG;
        }
        int pre = mutation_precheck(arc_path, /*must_exist=*/true);
        if (pre != RAR_OK) return pre;

        const std::filesystem::path arc = std::filesystem::u8path(arc_path);
        if (std::filesystem::exists(arc)) {
            std::unique_ptr<openrar::archive::ArchiveReader> reader;
            std::string detail;
            int cls = mutation_open(arc, reader, detail);
            if (cls != RAR_OK) {
                set_error(detail);
                return cls;
            }
            // reader closes here — before the mutator's temp/replace dance.
        }

        // Build the batch (pure per-file computation, no disk effect on the
        // archive). Directory sources become directory records
        // (non-recursive); arc_names are normalized '\' → '/' per §6.12.
        std::vector<openrar::archive::ArchiveMutator::PreparedAdd> batch;
        batch.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            if (!src_paths[i] || !arc_names[i] || arc_names[i][0] == '\0') {
                set_error("null argument");
                return RAR_ERR_INVALID_ARG;
            }
            std::string name(arc_names[i]);
            std::replace(name.begin(), name.end(), '\\', '/');
            while (!name.empty() && (name[0] == '/' || name[0] == '\\')) {
                name.erase(0, 1);
            }
            if (name.empty()) {
                set_error("invalid entry name");
                return RAR_ERR_INVALID_ARG;
            }
            openrar::archive::ArchiveMutator::PreparedAdd p;
            p.entry_name = name;
            p.src_path = std::filesystem::u8path(src_paths[i]);
            std::error_code ec;
            if (std::filesystem::is_directory(p.src_path, ec)) {
                if (!openrar::archive::ArchiveMutator::prepare_add_dir(p.src_path, name, p)) {
                    set_error("cannot add directory " + std::string(src_paths[i]));
                    return RAR_ERR_IO;
                }
            } else if (!openrar::archive::ArchiveMutator::prepare_add_file(
                           p.src_path, name, method, /*password=*/"", p,
                           openrar::archive::time_flags::MTIME, window_log2, false, false, false,
                           /*direct_stream=*/true)) {
                set_error("cannot read " + std::string(src_paths[i]));
                return RAR_ERR_IO;
            }
            batch.push_back(std::move(p));
        }

        std::string detail;
        int rc =
            openrar::archive::ArchiveMutator::write_batch_add_ex(arc, batch, {}, /*password=*/"",
                                                                 /*encrypt_headers=*/false, {},
                                                                 /*solid=*/false, {}, detail);
        if (rc != RAR_OK) set_error(detail.empty() ? "add failed" : detail);
        return rc;
    } catch (const std::bad_alloc&) {
        set_error("out of memory");
        return RAR_ERR_NOMEM;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_create_file(const char* arc_path, const char* const* src_paths,
                                                 const char* const* arc_names, uint32_t file_count,
                                                 int method, uint64_t dict_size) {
    return openrar_archive_create_file_ex(arc_path, src_paths, arc_names, file_count, method,
                                          dict_size, nullptr, 0, 0, nullptr, nullptr, nullptr);
}

static openrar::compress::FilterConfig filter_cfg_from_flags(uint32_t flags) {
    openrar::compress::FilterConfig cfg;
    if (flags & OPENRAR_FILTER_DISABLE_ALL) {
        cfg.mode = openrar::compress::FilterMode::DisableAll;
        return cfg;
    }
    if (flags & OPENRAR_FILTER_FORCE_E8)
        cfg.e8_override = 1;
    else if (flags & OPENRAR_FILTER_DISABLE_E8)
        cfg.e8_override = -1;

    if (flags & OPENRAR_FILTER_FORCE_ARM)
        cfg.arm_override = 1;
    else if (flags & OPENRAR_FILTER_DISABLE_ARM)
        cfg.arm_override = -1;

    if (flags & OPENRAR_FILTER_FORCE_DELTA)
        cfg.delta_override = 1;
    else if (flags & OPENRAR_FILTER_DISABLE_DELTA)
        cfg.delta_override = -1;

    return cfg;
}

int OPENRAR_DLL_CALL openrar_archive_create_file_ex(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, openrar_progress_cb progress, openrar_cancel_cb cancel,
    void* user) {
    // Honor the documented contract: ANY non-zero value means solid. The
    // hidden encoding that carried filter flags in bits 8-15 silently
    // produced a NON-solid archive for values like 0x100 — that undocumented
    // wire format is removed; filter flags go through create_file_opts
    // (v1.21.1 fix).
    int is_solid = (solid != 0) ? 1 : 0;
    return openrar_archive_create_file_opts(arc_path, src_paths, arc_names, file_count, method,
                                            dict_size, password_utf8, encrypt_headers, is_solid,
                                            OPENRAR_FILTER_DEFAULT, progress, cancel, user);
}

int OPENRAR_DLL_CALL openrar_archive_create_file_opts(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, uint32_t filter_flags, openrar_progress_cb progress,
    openrar_cancel_cb cancel, void* user) {
    return openrar_archive_create_file_opts_mt(arc_path, src_paths, arc_names, file_count, method,
                                               dict_size, password_utf8, encrypt_headers, solid,
                                               filter_flags, /*threads=*/1, progress, cancel, user);
}

int OPENRAR_DLL_CALL openrar_archive_create_file_opts_mt(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, uint32_t filter_flags, uint32_t threads,
    openrar_progress_cb progress, openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path || !src_paths || !arc_names || file_count == 0) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        if (method < 0 || method > 5) {
            set_error("method must be 0..5");
            return RAR_ERR_INVALID_ARG;
        }
        if (encrypt_headers && (!password_utf8 || password_utf8[0] == '\0')) {
            set_error("header encryption requires a password");
            return RAR_ERR_INVALID_ARG;
        }
        if (cancel && cancel(user)) {
            set_error("aborted");
            return RAR_ERR_ABORTED;
        }
        int pre = mutation_precheck(arc_path, /*must_exist=*/false);
        if (pre != RAR_OK) return pre;

        const std::filesystem::path arc = std::filesystem::u8path(arc_path);

        std::string password = password_utf8 ? password_utf8 : "";
        openrar::compress::FilterConfig filter_cfg = filter_cfg_from_flags(filter_flags);

        // Exclusive Concurrency Policy:
        // Single files parallelize across chunks with chunk_threads = threads;
        // Multi-file batches parallelize across files (or serial) with chunk_threads = 1.
        unsigned file_threads = (file_count == 1) ? threads : 1;

        std::vector<openrar::archive::ArchiveMutator::PreparedAdd> batch;
        batch.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            if (cancel && cancel(user)) {
                set_error("aborted");
                return RAR_ERR_ABORTED;
            }
            if (!src_paths[i] || !arc_names[i] || arc_names[i][0] == '\0') {
                set_error("null argument");
                return RAR_ERR_INVALID_ARG;
            }
            std::string name(arc_names[i]);
            std::replace(name.begin(), name.end(), '\\', '/');
            while (!name.empty() && (name[0] == '/' || name[0] == '\\')) {
                name.erase(0, 1);
            }
            if (name.empty()) {
                set_error("invalid entry name");
                return RAR_ERR_INVALID_ARG;
            }

            openrar::archive::ArchiveMutator::PreparedAdd p;
            p.entry_name = name;
            p.src_path = std::filesystem::u8path(src_paths[i]);
            std::error_code ec;
            if (std::filesystem::is_directory(p.src_path, ec)) {
                if (!openrar::archive::ArchiveMutator::prepare_add_dir(p.src_path, name, p)) {
                    set_error("cannot add directory " + std::string(src_paths[i]));
                    return RAR_ERR_IO;
                }
            } else if (!openrar::archive::ArchiveMutator::prepare_add_file(
                           p.src_path, name, method, password, p,
                           openrar::archive::time_flags::MTIME, dict_size,
                           /*want_streams=*/false, /*want_acl=*/false, solid != 0,
                           /*direct_stream=*/true, filter_cfg,
                           /*default_group=*/"", /*default_user=*/"",
                           /*threads=*/file_threads)) {
                set_error("cannot read " + std::string(src_paths[i]));
                return RAR_ERR_IO;
            }
            batch.push_back(std::move(p));
        }

        std::function<void(size_t, const std::string&)> on_write;
        if (progress) {
            on_write = [&](size_t idx, const std::string&) {
                progress(user, static_cast<uint64_t>(idx + 1), static_cast<uint64_t>(file_count));
            };
        }

        std::string detail;
        int rc = openrar::archive::ArchiveMutator::write_batch_add_ex(
            arc, batch, {}, password, encrypt_headers != 0, on_write, solid != 0, {}, detail);
        if (rc != RAR_OK) set_error(detail.empty() ? "create failed" : detail);
        return rc;
    } catch (const std::bad_alloc&) {
        set_error("out of memory");
        return RAR_ERR_NOMEM;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_repair(const char* arc_path, openrar_progress_cb progress,
                                            openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        if (cancel && cancel(user)) {
            set_error("aborted");
            return RAR_ERR_ABORTED;
        }
        // If .rev files are present, repair reconstructs missing volumes,
        // so arc_path itself does not strictly need to exist on disk.
        bool has_rev =
            openrar::recovery::RecoveryWriter::has_rev_files(std::filesystem::u8path(arc_path));
        int pre = mutation_precheck(arc_path, /*must_exist=*/!has_rev);
        if (pre != RAR_OK) return pre;

        if (progress) progress(user, 0, 100);

        bool ok = openrar::recovery::RecoveryWriter::repair(std::filesystem::u8path(
            arc_path)); // No post-operation cancel poll: RecoveryWriter takes no cancel
        // callback, so the operation has already committed by the time the
        // flag could be sampled here (v1.21.1 fix).
        if (!ok) {
            set_error("repair failed");
            return RAR_ERR_IO;
        }
        if (progress) progress(user, 100, 100);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_create_rev_volumes(const char* arc_path,
                                                        uint32_t count_or_percent, int is_percent,
                                                        unsigned int threads,
                                                        openrar_progress_cb progress,
                                                        openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        if (cancel && cancel(user)) {
            set_error("aborted");
            return RAR_ERR_ABORTED;
        }
        int pre = mutation_precheck(arc_path, /*must_exist=*/false);
        if (pre != RAR_OK) return pre;

        if (progress) progress(user, 0, 100);

        bool ok = openrar::recovery::RecoveryWriter::write_rev_volumes(
            std::filesystem::u8path(arc_path), count_or_percent, is_percent != 0,
            threads > 0 ? threads
                        : 1); // No post-operation cancel poll: RecoveryWriter takes no cancel
        // callback, so the operation has already committed by the time the
        // flag could be sampled here (v1.21.1 fix).
        if (!ok) {
            set_error("create recovery volumes failed: not a volume archive or invalid params");
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
        if (progress) progress(user, 100, 100);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

int OPENRAR_DLL_CALL openrar_archive_add_recovery_record(const char* arc_path, uint32_t percent,
                                                         unsigned int threads,
                                                         openrar_progress_cb progress,
                                                         openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        if (percent == 0 || percent > 1000) {
            set_error("invalid recovery record percentage (must be 1..1000)");
            return RAR_ERR_INVALID_ARG;
        }
        if (cancel && cancel(user)) {
            set_error("aborted");
            return RAR_ERR_ABORTED;
        }
        int pre = mutation_precheck(arc_path, /*must_exist=*/true);
        if (pre != RAR_OK) return pre;

        if (progress) progress(user, 0, 100);

        bool ok = openrar::recovery::RecoveryWriter::add_recovery_record(
            std::filesystem::u8path(arc_path), percent,
            threads > 0 ? threads
                        : 1); // No post-operation cancel poll: RecoveryWriter takes no cancel
        // callback, so the operation has already committed by the time the
        // flag could be sampled here (v1.21.1 fix).
        if (!ok) {
            set_error("add recovery record failed");
            return RAR_ERR_IO;
        }
        if (progress) progress(user, 100, 100);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

} // extern "C"

extern "C" {

// ── Extended metadata (v1.5.0) ───────────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_archive_handle_entry_ex(uint32_t handle, uint32_t entry_index,
                                                     openrar_entry_ex_t* out, void** extra_out,
                                                     size_t* extra_size_out) {
    try {
        if (!out || !extra_out || !extra_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out = openrar_entry_ex_t{};
        *extra_out = nullptr;
        *extra_size_out = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->entry_ex(entry_index, out, extra_out, extra_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

void OPENRAR_DLL_CALL openrar_archive_entry_ex_free(void* extra) {
    std::free(extra);
}

int OPENRAR_DLL_CALL openrar_archive_handle_entry_owner(uint32_t handle, uint32_t entry_index,
                                                        openrar_entry_owner_t* owner_out,
                                                        char** username_out, char** groupname_out) {
    try {
        if (!owner_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *owner_out = openrar_entry_owner_t{};
        if (username_out) *username_out = nullptr;
        if (groupname_out) *groupname_out = nullptr;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->entry_owner(entry_index, owner_out, username_out, groupname_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

void OPENRAR_DLL_CALL openrar_archive_entry_owner_free(char* str) {
    std::free(str);
}

int OPENRAR_DLL_CALL openrar_archive_handle_info(uint32_t handle, openrar_archive_info_t* out,
                                                 void** comment_out, size_t* comment_size_out) {
    try {
        if (!out || !comment_out || !comment_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out = openrar_archive_info_t{};
        *comment_out = nullptr;
        *comment_size_out = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        return h->info(out, comment_out, comment_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

} // extern "C"

static std::vector<uint8_t> read_file_bytes(const char* path, bool& ok) {
    ok = false;
    std::vector<uint8_t> out;
    if (!path) return out;
    std::filesystem::path p = std::filesystem::u8path(path);
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
    if (!f && !f.eof()) {
        out.clear();
        return out;
    }
    ok = true;
    return out;
}

extern "C" {

int OPENRAR_DLL_CALL openrar_archive_list_file(const char* arc_path, uint32_t* count,
                                               void** entries_out, void** paths_out,
                                               size_t* paths_size_out) {
    try {
        bool ok = false;
        auto data = read_file_bytes(arc_path, ok);
        if (!ok) return RAR_ERR_IO;
        return openrar_archive_list(data.data(), data.size(), count, entries_out, paths_out,
                                    paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_file(const char* arc_path, uint32_t entry_index,
                                                  uint8_t** out_ptr, size_t* out_len) {
    try {
        bool ok = false;
        auto data = read_file_bytes(arc_path, ok);
        if (!ok) return RAR_ERR_IO;
        return openrar_archive_extract(data.data(), data.size(), entry_index, out_ptr, out_len);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_file_to_path(const char* arc_path,
                                                          uint32_t entry_index,
                                                          const char* dest_path) {
    try {
        if (!arc_path || !dest_path) return RAR_ERR_INVALID_ARG;
        uint8_t* buf = nullptr;
        size_t len = 0;
        int rc = openrar_archive_extract_file(arc_path, entry_index, &buf, &len);
        if (rc != RAR_OK) return rc;
        std::filesystem::path out = std::filesystem::u8path(dest_path);
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        if (!f) {
            openrar_free(buf);
            return RAR_ERR_IO;
        }
        if (len > 0) f.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
        openrar_free(buf);
        return f ? RAR_OK : RAR_ERR_IO;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create_from_paths(const char* const* src_paths,
                                                       const char* const* arc_names,
                                                       uint32_t file_count, int method,
                                                       uint32_t window_log2, uint8_t** out_ptr,
                                                       size_t* out_len) {
    try {
        if (!src_paths || !arc_names || !out_ptr || !out_len) return RAR_ERR_INVALID_ARG;
        if (file_count == 0) return RAR_ERR_INVALID_ARG;
        std::vector<std::vector<uint8_t>> file_datas;
        std::vector<const uint8_t*> data_ptrs;
        std::vector<const uint8_t*> path_ptrs;
        std::vector<size_t> sizes;
        std::vector<std::string> arc_str;
        file_datas.reserve(file_count);
        data_ptrs.reserve(file_count);
        path_ptrs.reserve(file_count);
        sizes.reserve(file_count);
        arc_str.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            if (!src_paths[i] || !arc_names[i]) return RAR_ERR_INVALID_ARG;
            bool ok = false;
            auto b = read_file_bytes(src_paths[i], ok);
            if (!ok) return RAR_ERR_IO;
            arc_str.emplace_back(arc_names[i]);
            file_datas.emplace_back(std::move(b));
        }
        for (uint32_t i = 0; i < file_count; ++i) {
            path_ptrs.push_back(reinterpret_cast<const uint8_t*>(arc_str[i].c_str()));
            data_ptrs.push_back(file_datas[i].data());
            sizes.push_back(file_datas[i].size());
        }
        return openrar_archive_create(path_ptrs.data(), data_ptrs.data(), sizes.data(), file_count,
                                      method, window_log2, out_ptr, out_len);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create_to_file(const char* const* src_paths,
                                                    const char* const* arc_names,
                                                    uint32_t file_count, int method,
                                                    uint32_t window_log2, const char* out_path) {
    try {
        if (!out_path) return RAR_ERR_INVALID_ARG;
        uint8_t* buf = nullptr;
        size_t len = 0;
        int rc = openrar_archive_create_from_paths(src_paths, arc_names, file_count, method,
                                                   window_log2, &buf, &len);
        if (rc != RAR_OK) return rc;
        std::filesystem::path out = std::filesystem::u8path(out_path);
        rc = durable_write_to(out, [&](openrar::io::FileStream& f) {
            if (len > 0 && f.write(buf, len) != len) return RAR_ERR_IO;
            return RAR_OK;
        });
        openrar_free(buf);
        return rc;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

// ── Streaming ────────────────────────────────────────────────────────────────
namespace {
struct StreamHandle {
    std::unique_ptr<openrar::compress::StreamEncoder> enc;
    openrar_progress_cb prog_cb{nullptr};
    void* prog_user{nullptr};
    openrar_cancel_cb cancel_cb{nullptr};
    void* cancel_user{nullptr};
};
// Shared contract table: pin() returns a shared_ptr so feed/finish can work
// with the table lock released (L12: host callbacks must never run under the
// map mutex, and a callback may close the handle concurrently).
openrar::api::HandleTable<StreamHandle> g_stream_handles;
} // namespace
uint32_t OPENRAR_DLL_CALL openrar_stream_create(int method, uint32_t window_log2) {
    try {
        size_t win = 0;
        switch (window_log2) {
        case 1:
            win = 128 * 1024;
            break;
        case 2:
            win = 256 * 1024;
            break;
        case 3:
            win = 512 * 1024;
            break;
        case 4:
            win = 1024 * 1024;
            break;
        case 5:
            win = 4 * 1024 * 1024;
            break;
        default:
            return 0;
        }
        auto h = std::make_shared<StreamHandle>();
        h->enc = std::make_unique<openrar::compress::StreamEncoder>(method, win);
        return g_stream_handles.insert(h);
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
int OPENRAR_DLL_CALL openrar_stream_feed(uint32_t handle, const uint8_t* src, size_t n) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        // L12 (via HandleTable::pin): no host callback runs under the table
        // mutex — the shared_ptr pins the entry's lifetime, so the cancel poll
        // and enc->feed() below run with no lock held.
        if (h->cancel_cb && h->cancel_cb(h->cancel_user)) return RAR_ERR_ABORTED;
        bool ok = h->enc->feed(src, n);
        return ok ? RAR_OK : RAR_ERR_INVALID_ARG;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}
int OPENRAR_DLL_CALL openrar_stream_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return RAR_ERR_INVALID_ARG;
        *out_ptr = nullptr;
        *out_len = 0;
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        // L12 (via HandleTable::pin): cancel poll and enc->finish() run with
        // no table lock held (see feed).
        if (h->cancel_cb && h->cancel_cb(h->cancel_user)) return RAR_ERR_ABORTED;
        std::vector<uint8_t> out;
        bool ok = h->enc->finish(out);
        if (!ok) return RAR_ERR_NOMEM;
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return RAR_OK;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return RAR_ERR_NOMEM;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
void OPENRAR_DLL_CALL openrar_stream_free(uint32_t handle) {
    g_stream_handles.erase(handle);
}
// openrar_set_progress/openrar_set_cancel removed (L12/I19): they were silent
// no-ops and there is no per-block hook in the one-shot archive handle API to
// drive them honestly. Use the stream-level openrar_stream_set_progress /
// openrar_stream_set_cancel instead.
int OPENRAR_DLL_CALL openrar_stream_set_progress(uint32_t handle, openrar_progress_cb cb,
                                                 void* user) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        h->prog_cb = cb;
        h->prog_user = user;
        if (cb)
            h->enc->set_progress(cb, user);
        else
            h->enc->set_progress(nullptr, nullptr);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}
int OPENRAR_DLL_CALL openrar_stream_set_cancel(uint32_t handle, openrar_cancel_cb cb, void* user) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        h->cancel_cb = cb;
        h->cancel_user = user;
        h->enc->set_cancel(cb, user);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}

// ── Listing with progress / cancel ───────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_archive_list_file_ex(const char* arc_path, uint32_t* count,
                                                  void** entries_out, void** paths_out,
                                                  size_t* paths_size_out,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = openrar::archive::list_file_stream(
            std::filesystem::u8path(arc_path), parsed,
            /*password=*/nullptr,
            /*emit_encrypted_entries=*/false, list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_list_file_pw(const char* arc_path, const char* password,
                                                  uint32_t* count, void** entries_out,
                                                  void** paths_out, size_t* paths_size_out,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        // The streaming path owns header decryption; encrypted file entries
        // are reported with is_encrypted = 1 (this is what makes -hp listing
        // useful — see the header contract).
        int rc = openrar::archive::list_file_stream(
            std::filesystem::u8path(arc_path), parsed, password, /*emit_encrypted_entries=*/true,
            list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_list_ex(const uint8_t* data, size_t size, uint32_t* count,
                                             void** entries_out, void** paths_out,
                                             size_t* paths_size_out, openrar_progress_cb progress,
                                             openrar_cancel_cb cancel, void* user) {
    try {
        if (!data || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed, list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

} // extern "C"
