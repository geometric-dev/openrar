#include "archive_mutator.hpp"
#include "volume.hpp"
#include "rar_errors.hpp" // RAR_* status codes for the mutation variants
#include "../compress/compressor50.hpp"
#include "../format/header_writer.hpp"
#include "../io/path_util.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/aes256.hpp"
#include "../crypto/pbkdf2.hpp"
#include "../crypto/rng.hpp"
#include "../io/win32_meta.hpp"
#include <chrono>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openrar::archive {

namespace {

// Returns false on any short read or write so callers can abort.
bool copy_stream_region(io::FileStream& src, io::FileStream& dest, core::uint64 src_offset,
                        core::uint64 size) {
    src.seek(static_cast<core::int64>(src_offset), io::SeekOrigin::Begin);
    core::byte buf[16384];
    core::uint64 remaining = size;

    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(sizeof(buf))));
        if (src.read(buf, take) != take) return false;
        if (dest.write(buf, take) != take) return false;
        remaining -= take;
    }
    return true;
}

// Wall-clock times of a filesystem entry as unix seconds (FHEXTRA_HTIME).
// Windows reads all three from one call; POSIX maps ctime to the inode change
// time, the closest portable analogue.
struct FileTimes {
    core::uint64 mtime{0};
    core::uint64 ctime{0};
    core::uint64 atime{0};
};

bool get_file_times(const std::filesystem::path& path, FileTimes& out) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    auto to_unix = [](const FILETIME& ft) -> core::uint64 {
        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        if (ul.QuadPart == 0) return 0; // unset on this filesystem
        return static_cast<core::uint64>(ul.QuadPart / 10000000ULL) - 11644473600ULL;
    };
    out.mtime = to_unix(fa.ftLastWriteTime);
    out.ctime = to_unix(fa.ftCreationTime);
    out.atime = to_unix(fa.ftLastAccessTime);
    return true;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    out.mtime = static_cast<core::uint64>(st.st_mtime);
    out.ctime = static_cast<core::uint64>(st.st_ctime);
    out.atime = static_cast<core::uint64>(st.st_atime);
    return true;
#endif
}

// Populate utime_unix (always, from mtime) and the FHEXTRA_HTIME fields
// selected by times_mask. Zero (1970-01-01) times are treated as unset by the
// header writer's presence logic, matching the existing field semantics.
void apply_file_times(format::FileBlock& fb, const FileTimes& times, core::uint32 times_mask) {
    fb.utime_unix = static_cast<core::uint32>(times.mtime);
    if (times_mask & time_flags::MTIME)
        fb.htime_mtime_unix = static_cast<core::uint32>(times.mtime);
    if (times_mask & time_flags::CTIME)
        fb.htime_ctime_unix = static_cast<core::uint32>(times.ctime);
    if (times_mask & time_flags::ATIME)
        fb.htime_atime_unix = static_cast<core::uint32>(times.atime);
    if (fb.htime_mtime_unix != 0 || fb.htime_ctime_unix != 0 || fb.htime_atime_unix != 0)
        fb.htime_is_unix = true;
}

// RAR5 per-file AES-256-CBC encryption helper. Generates a fresh salt + IV,
// pads the payload to a 16-byte multiple with zero bytes, encrypts in place,
// and populates the encryption fields on fb. Returns false on RNG failure.
//
// The stored pack_size (fb.pack_size) is set to the padded ciphertext length.
// Decompression side reads unp_size bytes and stops, so trailing pad bytes
// after the final RAR5 block are harmless.
bool encrypt_file_payload(std::vector<core::byte>& payload, format::FileBlock& fb,
                          const std::string& password) {
    // Default of 2^15 = 32768 PBKDF2 rounds.
    constexpr core::uint8 LG2_COUNT = 15;
    constexpr size_t SALT_LEN = 16;
    constexpr size_t IV_LEN = 16;

    core::byte salt[SALT_LEN];
    core::byte iv[IV_LEN];
    if (!crypto::secure_random_bytes(salt, SALT_LEN)) return false;
    if (!crypto::secure_random_bytes(iv, IV_LEN)) return false;

    crypto::Rar5Keys keys;
    crypto::Pbkdf2Rar5::derive_keys(password, salt, SALT_LEN, 1U << LG2_COUNT, keys);

    // Zero-pad payload to 16-byte boundary.
    size_t orig_size = payload.size();
    size_t padded = (orig_size + 15u) & ~size_t(15);
    payload.resize(padded, 0);

    // AES-CBC destroys iv (writes back the last cipher block). Preserve the
    // original IV in fb so extraction can reproduce it.
    core::byte iv_copy[IV_LEN];
    std::memcpy(iv_copy, iv, IV_LEN);
    crypto::Aes256 aes(keys.aes_key);
    if (!aes.encrypt_cbc(payload.data(), padded, iv_copy)) return false;

    fb.is_encrypted = true;
    fb.crypt_version = 0;
    fb.crypt_flags = 0x01; // has psw_check
    fb.lg2_count = LG2_COUNT;
    std::memcpy(fb.salt.data(), salt, SALT_LEN);
    std::memcpy(fb.init_v.data(), iv, IV_LEN);
    fb.has_psw_check = true;
    std::memcpy(fb.psw_check.data(), keys.psw_check, sizeof(keys.psw_check));
    std::memcpy(fb.psw_check_csum.data(), keys.psw_check_csum, sizeof(keys.psw_check_csum));
    fb.pack_size = static_cast<core::int64>(padded);
    return true;
}

// (L8) Temp path for archive mutation: "<arc>.<tag>.<pid>-<millis>-<counter>",
// e.g. "a.rar.mut_tmp.4210-1694290000000-0". The legacy fixed names
// (".mut_tmp"/".lck_tmp"/".app_tmp") were predictable, so a local same-directory
// attacker could pre-plant a symlink/hardlink there and have the CreateAlways
// open follow it and truncate an arbitrary user-writable file. The per-call
// unique suffix combined with FileMode::CreateNew (fail-if-exists) makes any
// pre-planted name fail the open cleanly instead of clobbering a victim file.
std::filesystem::path mutation_temp_path(const std::filesystem::path& arc_path, const char* tag) {
    static std::atomic<core::uint32> counter{0};
#ifdef _WIN32
    const core::uint64 pid = static_cast<core::uint64>(GetCurrentProcessId());
#else
    const core::uint64 pid = static_cast<core::uint64>(getpid());
#endif
    const core::uint64 millis =
        static_cast<core::uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
    std::filesystem::path p = arc_path;
    p += std::string(".") + tag + "." + std::to_string(pid) + "-" + std::to_string(millis) + "-" +
         std::to_string(counter.fetch_add(1));
    return p;
}

// Data-loss-safe replacement (same contract as recovery_writer::atomic_replace):
// a failure must never destroy the original archive.
bool atomic_replace(const std::filesystem::path& tmp_path, const std::filesystem::path& arc_path) {
#ifdef _WIN32
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (MoveFileExW(tmp_path.c_str(), arc_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        Sleep(50);
    }
    // Fallback: park the original, rename tmp into place, restore on failure.
    std::filesystem::path orig_parked = arc_path;
    orig_parked += ".replace_bak";
    std::error_code ec;
    std::filesystem::remove(orig_parked, ec);
    std::filesystem::rename(arc_path, orig_parked, ec);
    if (ec) {
        return false;
    }
    std::filesystem::rename(tmp_path, arc_path, ec);
    if (!ec) {
        std::filesystem::remove(orig_parked, ec);
        return true;
    }
    std::filesystem::rename(orig_parked, arc_path, ec);
    return false;
#else
    // std::filesystem::rename(a, b, ec) returns void — the result is ec.
    // (The old `if (!rename(a, b, ec))` shape does not compile under GCC 13.)
    std::error_code ec;
    std::filesystem::rename(tmp_path, arc_path, ec);
    return ec == std::error_code{};
#endif
}

namespace volume_detail {

// A logical payload we're going to slice across output volumes. Backed either
// by an in-memory buffer (the new file we're adding, or an encrypted one that
// had to be fully materialised) or by a list of extents pointing into
// existing on-disk volumes. When extents-backed, slice writes stream bytes
// through a small scratch buffer instead of loading the whole payload.
struct PayloadEntry {
    format::FileBlock fb;
    // Exactly one of these is populated:
    std::vector<core::byte> packed_mem;
    std::vector<archive::VolumeExtent> extents;
    core::uint64 total_size{0};
    core::uint32 unpacked_crc{0};
};

// Copy `length` bytes of packed payload starting at offset into `out`.
// Streams from disk when the entry is extents-backed so we never buffer
// more than one slice at a time.
static bool read_packed_slice(const PayloadEntry& pe, core::uint64 offset, core::uint64 length,
                              std::vector<core::byte>& out) {
    out.resize(static_cast<size_t>(length));
    if (length == 0) return true;
    if (!pe.packed_mem.empty()) {
        if (offset + length > pe.packed_mem.size()) return false;
        std::memcpy(out.data(), pe.packed_mem.data() + offset, static_cast<size_t>(length));
        return true;
    }
    core::uint64 remain = length;
    core::uint64 pos = offset;
    size_t out_off = 0;
    for (const auto& e : pe.extents) {
        if (pos >= e.size) {
            pos -= e.size;
            continue;
        }
        core::uint64 take = std::min<core::uint64>(remain, e.size - pos);
        io::FileStream vs;
        if (!vs.open(e.volume_path, io::FileMode::ReadOnly)) return false;
        if (!vs.seek(static_cast<core::int64>(e.offset + pos), io::SeekOrigin::Begin)) return false;
        if (vs.read(out.data() + out_off, static_cast<size_t>(take)) != take) return false;
        out_off += static_cast<size_t>(take);
        remain -= take;
        pos = 0;
        if (remain == 0) break;
    }
    return remain == 0;
}
} // namespace volume_detail

// ── Solid-run analysis for the mutation guards (docs/invariants.md §1) ──────
// A run is a compressed, non-service, non-solid entry (the head) followed by
// compressed, non-service entries carrying the solid flag. Stored, directory
// and service entries inside a run carry no LZ state, but the mutation
// contract is range-literal: nothing in [H(S), S) of a retained solid entry
// may be removed (frozen plan §3.2).

// run_head[i] = index of the head of the run covering entry i; -1 = the
// chain starts at the archive beginning (a solid first compressed entry).
void analyze_solid_runs(const std::vector<ArchiveEntry>& entries,
                        std::vector<long long>& run_head) {
    run_head.assign(entries.size(), -1);
    long long cur = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        const format::FileBlock& h = entries[i].header;
        if (!h.is_service && h.method > 0 && !h.is_solid) cur = static_cast<long long>(i);
        run_head[i] = cur;
    }
}

// True when the removal set marked in `removed` leaves no retained solid
// entry orphaned; otherwise detail_out carries the contract message. A
// removed entry r violates iff its run still has a retained solid member
// after it (r ∈ [H, S) of that member).
bool solid_delete_permitted(const std::vector<ArchiveEntry>& entries,
                            const std::vector<char>& removed,
                            const std::vector<long long>& run_head, std::string& detail_out) {
    // Largest retained solid member per run, keyed by run head (-1 included).
    std::unordered_map<long long, size_t> max_retained_solid;
    for (size_t i = 0; i < entries.size(); ++i) {
        const format::FileBlock& h = entries[i].header;
        if (removed[i] || h.is_service || h.method == 0 || !h.is_solid) continue;
        auto it = max_retained_solid.find(run_head[i]);
        if (it == max_retained_solid.end() || it->second < i) max_retained_solid[run_head[i]] = i;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!removed[i]) continue;
        auto it = max_retained_solid.find(run_head[i]);
        if (it == max_retained_solid.end() || it->second <= i) continue;
        if (run_head[i] == static_cast<long long>(i))
            detail_out = "cannot delete head of solid block without recompressing chain";
        else
            detail_out = "cannot delete entries from solid archive without recompressing chain";
        return false;
    }
    return true;
}

// 'u' replacement guard: replacing a solid entry, or the head of a run that
// still has retained solid members, is refused (docs/dll-integration-spec.md
// §6.12 — the tail-replace asymmetry is intentional). Replaced entries then
// still run through solid_delete_permitted, which covers stored entries
// inside a retained run's chain range.
bool solid_replace_permitted(const std::vector<ArchiveEntry>& entries,
                             const std::vector<char>& replaced,
                             const std::vector<long long>& run_head, std::string& detail_out) {
    std::unordered_map<long long, bool> run_has_retained_solid;
    for (size_t i = 0; i < entries.size(); ++i) {
        const format::FileBlock& h = entries[i].header;
        if (replaced[i] || h.is_service || h.method == 0 || !h.is_solid) continue;
        run_has_retained_solid[run_head[i]] = true;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!replaced[i]) continue;
        const format::FileBlock& h = entries[i].header;
        const bool active_member = !h.is_service && h.method > 0 &&
                                   (h.is_solid || run_has_retained_solid.count(run_head[i]));
        if (active_member) {
            detail_out = "cannot replace entry in solid archive without recompressing chain";
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

// Shared rewrite core for both delete flavors (mask + by-index): `removed`
// marks reader entry indices to drop (non-service entries only; the QO
// service header is always stripped in addition). Runs the solid guard
// before any disk write, then rewrites SFX + main header (locator stripped)
// + surviving entries verbatim into a temp file and atomically replaces the
// archive. The reader is closed before the replace so the rename cannot hit
// the walk's own open handle.
int delete_entries_impl(const std::filesystem::path& arc_path, ArchiveReader& reader,
                        const std::vector<char>& removed, std::string& detail_out) {
    {
        std::vector<long long> run_head;
        analyze_solid_runs(reader.entries(), run_head);
        if (!solid_delete_permitted(reader.entries(), removed, run_head, detail_out)) {
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
    }

    std::filesystem::path tmp_path = mutation_temp_path(arc_path, "mut_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) {
        detail_out = "cannot create temp file for rewrite";
        return RAR_ERR_IO;
    }

    // Exception-safe temp (same pattern as write_batch_add_ex): an escaping
    // exception (bad_alloc, a throwing filesystem call in a future edit)
    // must not leave the mut_tmp file behind.
    try {
        // Preserve SFX module if present
        if (reader.sfx_offset() > 0) {
            if (!copy_stream_region(reader.stream(), out, 0, reader.sfx_offset())) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
        }

        // Signature
        format::HeaderWriter::write_signature(out);

        // Main block: Strip QuickOpen locator on mutation per spec
        format::MainBlock mb = reader.main_block();
        mb.has_locator = false;
        mb.locator_qo_offset = -1;
        mb.locator_rr_offset = -1;
        format::HeaderWriter::write_main_block(out, mb);

        // Copy surviving entries
        const std::vector<ArchiveEntry>& entries = reader.entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const ArchiveEntry& entry = entries[i];

            // Strip QuickOpen service block on mutation
            if (entry.header.is_service && entry.header.service_type == "QO") {
                continue;
            }
            if (i < removed.size() && removed[i]) {
                continue; // Deleted
            }

            // Retain entry and its verbatim payload
            if (!copy_stream_region(reader.stream(), out, entry.header_offset,
                                    entry.header_size + entry.data_size)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
        }

        // End of archive block
        format::EndArcBlock eb;
        eb.end_flags = 0;
        format::HeaderWriter::write_end_block(out, eb);

        reader.close();
        out.close();
    } catch (...) {
        try {
            out.close();
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
        } catch (...) {
        }
        throw;
    }

    if (!atomic_replace(tmp_path, arc_path)) {
        detail_out = "atomic replace failed";
        return RAR_ERR_IO;
    }
    return RAR_OK;
}

} // namespace

bool ArchiveMutator::delete_entries(const std::filesystem::path& arc_path,
                                    const std::vector<std::string>& masks) {
    ArchiveReader reader;
    if (!reader.open(arc_path)) {
        return false;
    }

    if (reader.is_locked() || reader.is_volume()) {
        return false; // Mutations strictly prohibited
    }

    std::vector<char> removed(reader.entries().size(), 0);
    for (size_t i = 0; i < reader.entries().size(); ++i) {
        const format::FileBlock& h = reader.entries()[i].header;
        if (h.is_service) continue;
        for (const auto& mask : masks) {
            if (io::wildcard_match(mask, h.file_name)) {
                removed[i] = 1;
                break;
            }
        }
    }

    std::string detail;
    return delete_entries_impl(arc_path, reader, removed, detail) == RAR_OK;
}

int ArchiveMutator::delete_entries_by_index(const std::filesystem::path& arc_path,
                                            const std::vector<core::uint64>& header_offsets,
                                            std::string& detail_out) {
    ArchiveReader reader;
    int status = RAR_OK;
    std::string open_detail;
    // A failed open close()s the reader and resets its per-archive flags, so
    // the -hp verdict arrives as the status code, not via saw_crypt_header().
    // With the empty password used here, BAD_PASSWORD can only mean a
    // header-encrypted archive (the derived keys garbage out on HEAD_CRYPT),
    // so both that and ENCRYPTED map to the pinned refusal below.
    if (!reader.open_ex(arc_path, /*password=*/"", status, open_detail)) {
        if (status == RAR_ERR_ENCRYPTED || status == RAR_ERR_BAD_PASSWORD) {
            detail_out = "mutating header-encrypted archive requires password";
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
        if (status == RAR_ERR_UNSUPPORTED_FEATURE) {
            detail_out = open_detail;
            return RAR_ERR_UNSUPPORTED_FEATURE;
        }
        detail_out = "cannot open archive";
        return RAR_ERR_IO;
    }
    if (reader.is_locked()) {
        detail_out = "archive is locked";
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    if (reader.is_volume()) {
        detail_out = "cannot mutate a multi-volume archive";
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }

    // Identity match: mark the (non-service) entries whose header offset was
    // requested. Offsets are file positions, stable across independent walks
    // of the same archive, so the caller's translation walk and this one
    // always agree.
    const std::vector<ArchiveEntry>& entries = reader.entries();
    std::vector<char> removed(entries.size(), 0);
    for (const core::uint64 off : header_offsets) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].header.is_service && entries[i].header_offset == off) removed[i] = 1;
        }
    }

    return delete_entries_impl(arc_path, reader, removed, detail_out);
}

bool ArchiveMutator::lock_archive(const std::filesystem::path& arc_path) {
    ArchiveReader reader;
    if (!reader.open(arc_path)) {
        return false;
    }

    if (reader.is_locked()) {
        return true; // Already locked
    }
    if (reader.is_volume()) {
        return false;
    }

    std::filesystem::path tmp_path = mutation_temp_path(arc_path, "lck_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) {
        return false;
    }

    // Exception-safe temp (same pattern as write_batch_add_ex): an escaping
    // exception must not leave the lck_tmp file behind.
    try {
        // SFX module
        if (reader.sfx_offset() > 0) {
            if (!copy_stream_region(reader.stream(), out, 0, reader.sfx_offset())) {
                out.close();
                std::filesystem::remove(tmp_path);
                return false;
            }
        }

        // Signature
        format::HeaderWriter::write_signature(out);

        // Main block with MHFL_LOCK set
        format::MainBlock mb = reader.main_block();
        mb.arc_flags |= format::MHFL_LOCK;
        format::HeaderWriter::write_main_block(out, mb);

        // Copy all entries verbatim
        for (const auto& entry : reader.entries()) {
            if (!copy_stream_region(reader.stream(), out, entry.header_offset,
                                    entry.header_size + entry.data_size)) {
                out.close();
                std::filesystem::remove(tmp_path);
                return false;
            }
        }

        // End block
        format::EndArcBlock eb;
        eb.end_flags = 0;
        format::HeaderWriter::write_end_block(out, eb);

        reader.close();
        out.close();
    } catch (...) {
        try {
            out.close();
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
        } catch (...) {
        }
        throw;
    }

    return atomic_replace(tmp_path, arc_path);
}

static std::filesystem::path get_exe_dir(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0) {
        return std::filesystem::path(buf).parent_path();
    }
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
#endif
    if (argv0 && argv0[0] != '\0') {
        std::error_code ec;
        auto p = std::filesystem::absolute(std::filesystem::path(argv0), ec);
        if (!ec) return p.parent_path();
        return std::filesystem::path(argv0).parent_path();
    }
    return {};
}

static bool is_path_qualified(const std::string& name) {
    for (char c : name)
        if (c == '/' || c == '\\') return true;
    if (name.size() >= 2 && name[1] == ':') return true; // drive letter
    return false;
}

static bool copy_sfx_stub(const std::filesystem::path& stub_path, io::FileStream& out) {
    io::FileStream stub;
    if (!stub.open(stub_path, io::FileMode::ReadOnly)) return false;
    core::uint64 sz = stub.size();
    if (sz > ArchiveMutator::MAX_SFX_SIZE) return false;
    if (sz == 0) return true;
    core::byte buf[65536];
    core::uint64 remaining = sz;
    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(sizeof(buf))));
        if (stub.read(buf, take) != take) return false;
        if (out.write(buf, take) != take) return false;
        remaining -= take;
    }
    return true;
}

bool ArchiveMutator::prepare_add_file(const std::filesystem::path& src_file,
                                      const std::string& arc_entry_name, int method,
                                      const std::string& password, PreparedAdd& out,
                                      core::uint32 times_mask, core::uint32 window_log2,
                                      bool want_streams, bool want_acl) {
    if (!std::filesystem::exists(src_file)) {
        return false;
    }

    // Dictionary window: window_log2 1..4 → 128 KiB..1 MiB (create parity);
    // 0 keeps the historical 2 MiB CLI default. Must match the win_size
    // passed to Compressor50 and written into the header (mismatch makes
    // decoders report checksum errors even for a valid LZ stream).
    core::uint32 win_size = 0x200000u;
    if (window_log2 >= 1 && window_log2 <= 4) {
        win_size = 0x20000u << (window_log2 - 1);
    } else if (window_log2 == 0) {
        switch (method) {
        case 0:
            win_size = 0x20000u;
            break; // 128 KB
        case 1:
            win_size = 0x80000u;
            break; // 512 KB
        case 2:
            win_size = 0x100000u;
            break; // 1 MB
        case 3:
            win_size = 0x200000u;
            break; // 2 MB
        case 4:
            win_size = 0x400000u;
            break; // 4 MB
        case 5:
            win_size = 0x1000000u;
            break; // 16 MB
        default:
            win_size = 0x200000u;
            break;
        }
    }

    core::uint64 file_sz = 0;
    core::uint32 crc = 0;
    std::vector<core::byte> compressed_payload;
    std::vector<core::byte> uncompressed;

    {
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) {
            return false;
        }
        file_sz = src.size();
        if (file_sz > 0) {
            uncompressed.resize(static_cast<size_t>(file_sz));
            if (src.read(uncompressed.data(), uncompressed.size()) != uncompressed.size()) {
                return false;
            }
            crypto::Crc32 crc_calc;
            crc_calc.update(uncompressed.data(), uncompressed.size());
            crc = crc_calc.get();

            if (method > 0) {
                if (compress::Compressor50::compress_buffer(uncompressed.data(),
                                                            uncompressed.size(), compressed_payload,
                                                            method, win_size)) {
                    // Only use compressed payload if smaller than uncompressed
                    if (compressed_payload.size() >= uncompressed.size()) {
                        compressed_payload.clear();
                        method = 0;
                    }
                } else {
                    method = 0;
                }
            }
        }
        src.close();
    }

    // Materialize the payload we're actually going to write.
    // For encrypted files we do this before building the header so pack_size
    // reflects the padded ciphertext length.
    std::vector<core::byte> payload_to_write;
    if (file_sz > 0) {
        if (!compressed_payload.empty())
            payload_to_write = std::move(compressed_payload);
        else
            payload_to_write = std::move(uncompressed);
    }

    format::FileBlock fb;
    fb.file_name = arc_entry_name;
    fb.unp_size = file_sz;
    fb.pack_size = static_cast<core::int64>(payload_to_write.size());
    fb.attributes = 0x20;
    fb.data_crc32 = crc;
    fb.has_crc32 = true;
    fb.method = static_cast<core::uint32>(method);
    // Must match the win_size passed to Compressor50 above (previously
    // hard-coded 4 MiB while the compressor defaulted to 2 MiB → interop
    // checksum failures).
    fb.win_size = (method > 0) ? win_size : 0;
    fb.unp_ver = 0;
    FileTimes times;
    if (get_file_times(src_file, times)) {
        apply_file_times(fb, times, times_mask);
    } else {
        fb.utime_unix = static_cast<core::uint32>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    if (!password.empty()) {
        if (!encrypt_file_payload(payload_to_write, fb, password)) {
            // RNG failure — refuse to write a weakly encrypted archive.
            return false;
        }
    }

    out.fb = std::move(fb);
    out.payload = std::move(payload_to_write);

    if (want_streams) {
        std::vector<io::StreamEntry> streams;
        if (io::read_alternate_streams(src_file, streams)) {
            for (const auto& s : streams) {
                if (s.data.size() > 0x40000000ULL) continue; // cap at 1 GiB per spec
                PreparedAdd child;
                child.fb.is_service = true;
                child.fb.service_type = "STM";
                child.fb.file_name = "STM";
                child.fb.sub_data.assign(s.name.begin(), s.name.end());
                child.fb.unp_size = s.data.size();
                child.fb.pack_size = static_cast<core::int64>(s.data.size());
                child.fb.attributes = 0x20;
                child.fb.method = 0;
                child.fb.win_size = 0;
                child.fb.unp_ver = 0;
                child.fb.has_crc32 = true;
                crypto::Crc32 scrc;
                scrc.update(s.data.data(), s.data.size());
                child.fb.data_crc32 = scrc.get();
                child.payload = s.data;
                out.child_services.push_back(std::move(child));
            }
        }
    }
    if (want_acl) {
        std::vector<core::byte> sd;
        if (io::read_security_descriptor(src_file, sd) && !sd.empty()) {
            if (sd.size() <= 0x100000ULL) { // 1 MiB cap per spec
                PreparedAdd child;
                child.fb.is_service = true;
                child.fb.service_type = "ACL";
                child.fb.file_name = "ACL";
                child.fb.unp_size = sd.size();
                child.fb.pack_size = static_cast<core::int64>(sd.size());
                child.fb.attributes = 0x20;
                child.fb.method = 0;
                child.fb.win_size = 0;
                child.fb.unp_ver = 0;
                child.fb.has_crc32 = true;
                crypto::Crc32 acrc;
                acrc.update(sd.data(), sd.size());
                child.fb.data_crc32 = acrc.get();
                child.payload = std::move(sd);
                out.child_services.push_back(std::move(child));
            }
        }
    }

    // entry_name/src_path are caller-owned identity fields, filled before the
    // call: the batch writer reads them concurrently while this prepare may
    // still be running, so writing them here would be a data race (M4).
    return true;
}

bool ArchiveMutator::prepare_add_dir(const std::filesystem::path& src_dir,
                                     const std::string& arc_entry_name, PreparedAdd& out,
                                     core::uint32 times_mask) {
    std::error_code ec;
    if (!std::filesystem::is_directory(src_dir, ec)) return false;

    format::FileBlock fb;
    fb.file_name = arc_entry_name;
    fb.file_flags = format::FHFL_DIRECTORY;
    fb.unp_size = 0;
    fb.pack_size = -1; // directories carry no data area
    fb.attributes = 0x10;
    fb.method = 0;
    fb.win_size = 0;
    fb.unp_ver = 0;
    FileTimes times;
    if (get_file_times(src_dir, times)) {
        apply_file_times(fb, times, times_mask);
    }

    // entry_name/src_path stay caller-owned (see prepare_add_file, M4).
    out.fb = std::move(fb);
    return true;
}

bool ArchiveMutator::prepare_add_symlink(const std::filesystem::path& src_symlink,
                                         const std::string& arc_entry_name,
                                         const std::string& target, bool is_dir_target,
                                         PreparedAdd& out, core::uint32 times_mask) {
    format::FileBlock fb;
    fb.file_name = arc_entry_name;
    fb.unp_size = 0;
    fb.pack_size = -1;
    fb.attributes = is_dir_target ? 0x10 : 0x20;
    fb.method = 0;
    fb.win_size = 0;
    fb.unp_ver = 0;
#ifdef _WIN32
    fb.redir_type = 2;
#else
    fb.redir_type = 1;
#endif
    fb.redir_dir_target = is_dir_target;
    fb.redir_target = target;
    FileTimes times;
    if (get_file_times(src_symlink, times)) {
        apply_file_times(fb, times, times_mask);
    }
    out.fb = std::move(fb);
    return true;
}

bool ArchiveMutator::get_file_mtime(const std::filesystem::path& path, core::uint64& mtime_out) {
    FileTimes ft;
    if (!get_file_times(path, ft)) return false;
    mtime_out = ft.mtime;
    return true;
}

int ArchiveMutator::write_batch_add_ex(
    const std::filesystem::path& arc_path, std::vector<PreparedAdd>& files,
    const std::filesystem::path& sfx_stub_path, const std::string& password, bool encrypt_headers,
    const std::function<void(size_t, const std::string&)>& on_write, bool solid,
    const std::vector<core::byte>& comment, std::string& detail_out) {
    if (files.empty()) {
        detail_out = "no input files";
        return RAR_ERR_INVALID_ARG;
    }

    // Validate SFX stub size before any write (MAX_SFX_SIZE guard per 10-sfx.md:70)
    if (!sfx_stub_path.empty()) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(sfx_stub_path, ec);
        if (ec) {
            detail_out = "cannot stat sfx stub";
            return RAR_ERR_IO;
        }
        if (sz > ArchiveMutator::MAX_SFX_SIZE) {
            detail_out = "sfx stub too large";
            return RAR_ERR_INVALID_ARG;
        }
    }

    std::filesystem::path tmp_path = mutation_temp_path(arc_path, "app_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) {
        detail_out = "cannot create temp file";
        return RAR_ERR_IO;
    }

    // Body runs in a lambda so an exception (the CLI's pipelined writer
    // aborts via a throwing on_write; std::filesystem/bad_alloc can also
    // throw) still closes and removes the tmp file before propagating.
    auto body = [&]() -> int {
        // If SFX stub requested, copy it 64 KiB chunks before signature (no alignment gap)
        bool have_sfx = !sfx_stub_path.empty();
        if (have_sfx) {
            if (!copy_sfx_stub(sfx_stub_path, out)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "cannot copy sfx stub";
                return RAR_ERR_IO;
            }
        }

        format::HeaderCryptWriter hcw;
        format::CryptBlock new_crypt;
        bool header_encrypt_mode = false;
        // True when the first data-bearing entry continues an existing solid
        // stream (append to a solid archive); false on fresh creates.
        bool continue_solid_stream = false;

        if (std::filesystem::exists(arc_path)) {
            ArchiveReader reader;
            // -hp archives carry encrypted headers: reading them back for the
            // append path requires the password.
            if (!reader.open(arc_path, password)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "cannot open existing archive";
                return RAR_ERR_IO;
            }
            if (reader.is_locked() || reader.is_volume()) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = reader.is_locked() ? "archive is locked"
                                                : "cannot mutate a multi-volume archive";
                return RAR_ERR_UNSUPPORTED_FEATURE;
            }

            // 'u' replacement removal set + solid guards, before any of the
            // rewrite is emitted (docs/invariants.md §1). Replaced entries are
            // the ones whose removal the guards must vet.
            const std::vector<ArchiveEntry>& entries = reader.entries();
            std::vector<char> replaced(entries.size(), 0);
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].header.is_service) continue;
                for (const auto& pf : files) {
                    if (entries[i].header.file_name == pf.entry_name) {
                        replaced[i] = 1;
                        for (size_t j = i + 1; j < entries.size() && entries[j].header.is_service; ++j) {
                            if (entries[j].header.service_type != "QO" && entries[j].header.service_type != "CMT") {
                                replaced[j] = 1;
                            }
                        }
                        break;
                    }
                }
            }
            {
                std::vector<long long> run_head;
                analyze_solid_runs(entries, run_head);
                if (!solid_replace_permitted(entries, replaced, run_head, detail_out) ||
                    !solid_delete_permitted(entries, replaced, run_head, detail_out)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    return RAR_ERR_UNSUPPORTED_FEATURE;
                }
            }

            // Header encryption context. An existing -hp archive keeps its
            // HEAD_CRYPT parameters (same salt/key must decrypt the copied old
            // headers); a plaintext archive can never be converted to -hp by
            // appending, because the copied old headers would stay in clear.
            if (reader.is_header_encrypted()) {
                if (!hcw.init_existing(password, reader.header_crypt())) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "cannot initialize header encryption";
                    return RAR_ERR_IO;
                }
                new_crypt = reader.header_crypt();
                header_encrypt_mode = true;
            } else if (encrypt_headers) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "cannot convert a plaintext archive to header encryption";
                return RAR_ERR_INVALID_ARG;
            }

            // If creating SFX anew, do not preserve old SFX prefix; we already wrote new stub
            if (!have_sfx && reader.sfx_offset() > 0) {
                if (!copy_stream_region(reader.stream(), out, 0, reader.sfx_offset())) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "rewrite failed";
                    return RAR_ERR_IO;
                }
            }
            format::HeaderWriter::write_signature(out);

            if (header_encrypt_mode) {
                // Re-emit HEAD_CRYPT with the archive's original parameters so
                // the appended encrypted headers share one key with the copied
                // old ones.
                if (!format::HeaderWriter::write_crypt_block(out, new_crypt)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "rewrite failed";
                    return RAR_ERR_IO;
                }
            }

            format::MainBlock mb = reader.main_block();
            mb.has_locator = false;
            if (solid) mb.arc_flags |= format::MHFL_SOLID;
            format::HeaderWriter::write_main_block(out, mb, header_encrypt_mode ? &hcw : nullptr);

            // A new CMT replaces the archive's previous comment; QO is always stripped per spec.
            continue_solid_stream = reader.is_solid();
            // Replace-if-exists across the whole batch: an old entry is dropped
            // when any prepared file carries its name (per-file appends produced
            // the same net effect one rewrite at a time). The replacement set
            // was already vetted by the solid guards above.
            for (size_t i = 0; i < entries.size(); ++i) {
                const ArchiveEntry& entry = entries[i];
                // Preserve RR for RecoveryWriter path — do not strip RR here; only QO is stripped per spec.
                // RecoveryWriter will splice out old RR and recompute parity/locator itself.
                if (entry.header.is_service && entry.header.service_type == "QO") continue;
                if (entry.header.is_service && entry.header.service_type == "CMT" &&
                    !comment.empty())
                    continue;
                if (i < replaced.size() && replaced[i]) continue;
                if (!copy_stream_region(reader.stream(), out, entry.header_offset,
                                        entry.header_size + entry.data_size)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "rewrite failed";
                    return RAR_ERR_IO;
                }
            }
            reader.close();
        } else {
            if (encrypt_headers && password.empty()) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "encrypt_headers requires a password";
                return RAR_ERR_INVALID_ARG;
            }
            if (encrypt_headers) {
                if (!hcw.init_new(password, new_crypt)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "cannot initialize header encryption";
                    return RAR_ERR_IO;
                }
                header_encrypt_mode = true;
            }

            format::HeaderWriter::write_signature(out);
            if (header_encrypt_mode) {
                if (!format::HeaderWriter::write_crypt_block(out, new_crypt)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "rewrite failed";
                    return RAR_ERR_IO;
                }
            }

            format::MainBlock mb;
            mb.arc_flags = solid ? format::MHFL_SOLID : 0;
            format::HeaderWriter::write_main_block(out, mb, header_encrypt_mode ? &hcw : nullptr);
        }

        // A requested comment (-z) becomes a CMT service header right after the
        // main header, ahead of every file entry.
        if (!comment.empty()) {
            format::FileBlock cmt;
            cmt.is_service = true;
            cmt.service_type = "CMT";
            cmt.file_name = "CMT";
            cmt.unp_size = comment.size();
            cmt.pack_size = static_cast<core::int64>(comment.size());
            cmt.attributes = 0x20;
            cmt.has_crc32 = true;
            crypto::Crc32 cmt_crc;
            cmt_crc.update(comment.data(), comment.size());
            cmt.data_crc32 = cmt_crc.get();
            cmt.method = 0;
            cmt.unp_ver = 0;
            cmt.win_size = 0;
            if (!format::HeaderWriter::write_file_block(out, cmt, 0,
                                                        header_encrypt_mode ? &hcw : nullptr)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
            if (out.write(comment.data(), comment.size()) != comment.size()) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
        }

        // Write every prepared entry header + payload in order. The archive is
        // not replaced until all writes succeed, so a mid-batch failure below
        // leaves the original untouched (all-or-nothing vs the old per-file
        // append which could leave a partially updated archive behind).
        // Solid chain bookkeeping: the solid bit on a compressed entry means its
        // LZ stream continues the previous compressed entry's stream. Directory
        // and stored entries carry no LZ state, so they neither start nor continue
        // the chain (and stay parallel-extractable).
        bool seen_compressed_entry = continue_solid_stream;
        for (size_t i = 0; i < files.size(); ++i) {
            PreparedAdd& pf = files[i];
            // entry_name is caller-owned and filled before the prepare jobs run —
            // reading it here is race-free (M4); fb/payload below are only read
            // after on_write has synchronized with the prepare job.
            if (on_write) on_write(i, pf.entry_name);

            if (pf.fb.method > 0 && !(pf.fb.file_flags & format::FHFL_DIRECTORY)) {
                // Appending to a solid archive continues its stream regardless
                // of whether -s was passed again.
                pf.fb.is_solid = (solid || continue_solid_stream) && seen_compressed_entry;
                seen_compressed_entry = true;
            } else {
                pf.fb.is_solid = false;
            }

            format::HeaderWriter::write_file_block(out, pf.fb, 0,
                                                   header_encrypt_mode ? &hcw : nullptr);
            if (!pf.payload.empty()) {
                out.write(pf.payload.data(), pf.payload.size());
            }
            // Free the payload as soon as it is on disk so peak memory tracks
            // the in-flight preparation set, not the whole batch.
            std::vector<core::byte>().swap(pf.payload);

            for (auto& child : pf.child_services) {
                format::HeaderWriter::write_file_block(
                    out, child.fb, format::HFL_CHILD | format::HFL_INHERITED,
                    header_encrypt_mode ? &hcw : nullptr);
                if (!child.payload.empty()) {
                    out.write(child.payload.data(), child.payload.size());
                }
                std::vector<core::byte>().swap(child.payload);
            }
        }

        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb, header_encrypt_mode ? &hcw : nullptr);
        out.close();

        // Atomically replace the archive (no pre-delete needed)
        if (!atomic_replace(tmp_path, arc_path)) {
            detail_out = "atomic replace failed";
            return RAR_ERR_IO;
        }

        for (const auto& pf : files) {
            if (!pf.delete_source) continue;
            std::error_code ec;
            std::filesystem::remove(pf.src_path, ec);
            if (ec) {
                detail_out = "cannot delete moved source";
                return RAR_ERR_IO;
            }
        }
        return RAR_OK;
    };

    try {
        return body();
    } catch (...) {
        out.close();
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        throw;
    }
}

bool ArchiveMutator::write_batch_add(
    const std::filesystem::path& arc_path, std::vector<PreparedAdd>& files,
    const std::filesystem::path& sfx_stub_path, const std::string& password, bool encrypt_headers,
    const std::function<void(size_t, const std::string&)>& on_write, bool solid,
    const std::vector<core::byte>& comment) {
    std::string detail;
    return write_batch_add_ex(arc_path, files, sfx_stub_path, password, encrypt_headers, on_write,
                              solid, comment, detail) == RAR_OK;
}

static bool add_or_move_file(const std::filesystem::path& arc_path,
                             const std::filesystem::path& src_file,
                             const std::string& arc_entry_name, bool delete_source, int method = 3,
                             const std::filesystem::path& sfx_stub_path = {},
                             const std::string& password = "", bool encrypt_headers = false) {
    ArchiveMutator::PreparedAdd prepared;
    prepared.entry_name = arc_entry_name;
    prepared.src_path = src_file;
    if (!ArchiveMutator::prepare_add_file(src_file, arc_entry_name, method, password, prepared)) {
        return false;
    }
    prepared.delete_source = delete_source;
    std::vector<ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prepared));
    return ArchiveMutator::write_batch_add(arc_path, batch, sfx_stub_path, password,
                                           encrypt_headers);
}

bool ArchiveMutator::add_file_to_archive_vol(const std::filesystem::path& arc_path,
                                             const std::filesystem::path& src_file,
                                             const std::string& arc_entry_name, int method,
                                             core::uint64 vol_size, const std::string& password,
                                             bool solid) {
    if (vol_size == 0 || vol_size == volume::VOLSIZE_AUTO) {
        return add_or_move_file(arc_path, src_file, arc_entry_name, false, method, {}, password);
    }
    if (vol_size < 1024) return false; // too small
    if (!std::filesystem::exists(src_file)) return false;

    // Prepare new file payload
    core::uint64 file_sz = 0;
    core::uint32 unpacked_crc = 0;
    std::vector<core::byte> compressed_payload;
    std::vector<core::byte> uncompressed;
    {
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) return false;
        file_sz = src.size();
        if (file_sz > 0) {
            uncompressed.resize(static_cast<size_t>(file_sz));
            if (src.read(uncompressed.data(), uncompressed.size()) != uncompressed.size())
                return false;
            crypto::Crc32 c;
            c.update(uncompressed.data(), uncompressed.size());
            unpacked_crc = c.get();
            constexpr size_t DICT_SIZE = 0x200000;
            if (method > 0) {
                if (compress::Compressor50::compress_buffer(uncompressed.data(),
                                                            uncompressed.size(), compressed_payload,
                                                            method, DICT_SIZE)) {
                    if (compressed_payload.size() >= uncompressed.size()) {
                        compressed_payload.clear();
                        method = 0;
                    }
                } else
                    method = 0;
            }
        }
    }
    // Materialise the new file's packed payload in a single owning buffer.
    // No stray copy: whichever of `compressed_payload` / `uncompressed` won,
    // its storage is moved into new_packed and the loser was already cleared
    // above. If encryption is requested, encrypt_file_payload() below mutates
    // new_packed in place (zero-pads to 16 bytes and AES-CBC-encrypts).
    std::vector<core::byte> new_packed;
    if (file_sz > 0) {
        if (!compressed_payload.empty())
            new_packed = std::move(compressed_payload);
        else
            new_packed = std::move(uncompressed);
    }
    // Build base FileBlock for new file
    format::FileBlock base_fb;
    base_fb.file_name = arc_entry_name;
    base_fb.unp_size = file_sz;
    base_fb.pack_size = static_cast<core::int64>(new_packed.size());
    base_fb.attributes = 0x20;
    base_fb.data_crc32 = unpacked_crc;
    base_fb.has_crc32 = true;
    base_fb.method = static_cast<core::uint32>(method);
    base_fb.win_size = (method > 0) ? 0x200000u : 0;
    base_fb.unp_ver = 0;
    FileTimes vol_times;
    if (get_file_times(src_file, vol_times))
        base_fb.utime_unix = static_cast<core::uint32>(vol_times.mtime);
    else
        base_fb.utime_unix = static_cast<core::uint32>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

    if (!password.empty() && file_sz > 0) {
        // Encrypt the whole packed payload in one shot; slices below just
        // carve up the ciphertext. Each slice's header carries the same
        // FHEXTRA_CRYPT record (copied via pe.fb = base_fb).
        if (!encrypt_file_payload(new_packed, base_fb, password)) return false;
    }

    // Collect existing entries if any (try first volume then arc_path)
    std::vector<volume_detail::PayloadEntry> all_payloads;
    std::filesystem::path firstVolume = volume::first_volume_name(arc_path, false);
    std::filesystem::path existing_path;
    // Solid chaining across the rewritten chain: requested via -s, or
    // continued when the existing chain is solid. The new entry carries the
    // solid bit only when a prior compressed entry starts/continues the
    // stream (matching write_batch_add's bookkeeping).
    bool vol_solid = solid;
    bool has_prior_compressed = false;
    if (std::filesystem::exists(firstVolume))
        existing_path = firstVolume;
    else if (std::filesystem::exists(arc_path) && arc_path != firstVolume)
        existing_path = arc_path;
    else if (std::filesystem::exists(arc_path))
        existing_path = arc_path;

    // SFX handling: capture sfx bytes if present (first-volume-only)
    std::vector<core::byte> sfx_bytes;
    core::uint64 sfx_off = 0;
    if (!existing_path.empty()) {
        ArchiveReader r;
        // Hard guard: never proceed when the existing archive cannot be
        // opened (header-encrypted without password, corrupt file, IO
        // error). Continuing would rename every volume aside and write a
        // fresh chain containing only the new file — silent loss of the
        // original contents — so refuse instead.
        if (!r.open(existing_path)) return false;
        if (r.is_locked()) return false;
        // Allow volume rewrite; previously would reject but for multivolume we permit recreating
        vol_solid = vol_solid || r.is_solid();
        sfx_off = r.sfx_offset();
        if (sfx_off > 0) {
            io::FileStream s;
            if (s.open(existing_path, io::FileMode::ReadOnly)) {
                sfx_bytes.resize(static_cast<size_t>(sfx_off));
                s.read(sfx_bytes.data(), sfx_bytes.size());
            }
        }
        for (auto& e : r.entries()) {
            if (e.header.is_service && e.header.service_type == "QO") continue;
            if (!e.header.is_service && e.header.file_name == arc_entry_name) continue; // replace
            volume_detail::PayloadEntry pe;
            pe.fb = e.header;
            pe.unpacked_crc = e.header.data_crc32;
            // Carry over the on-disk extents without slurping the bytes.
            // For split-across-volume entries this is a small list of
            // (path, offset, size) tuples; read_packed_slice() streams
            // through them on demand when we emit each output slice.
            pe.extents = e.extents;
            pe.total_size = 0;
            for (const auto& x : pe.extents) pe.total_size += x.size;
            pe.fb.pack_size = static_cast<core::int64>(pe.total_size);
            if (!pe.fb.is_service && pe.fb.method > 0) has_prior_compressed = true;
            all_payloads.push_back(std::move(pe));
        }
        r.close();
    }
    base_fb.is_solid = vol_solid && has_prior_compressed && base_fb.method > 0;

    // Rename every existing volume we're about to overwrite to a `.mv_bak`
    // sidecar. The extents in each pe.extents get their volume_path rewritten
    // to point at the sidecar so read_packed_slice() can stream through the
    // original bytes even after start_vol() clobbers the canonical paths.
    // On success we remove the sidecars at the end; on failure a SidecarGuard
    // restores them by renaming back to the originals.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> sidecars;
    struct SidecarGuard {
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* sidecars;
        bool committed{false};
        ~SidecarGuard() {
            if (!sidecars) return;
            for (auto& [orig, bak] : *sidecars) {
                std::error_code ec;
                if (committed) {
                    std::filesystem::remove(bak, ec);
                } else {
                    std::filesystem::remove(orig, ec);
                    std::filesystem::rename(bak, orig, ec);
                }
            }
        }
    } sidecar_guard{&sidecars};

    if (!existing_path.empty()) {
        auto do_rename = [&](const std::filesystem::path& orig) -> bool {
            std::filesystem::path bak = orig.string() + ".mv_bak";
            std::error_code ec;
            std::filesystem::remove(bak, ec);
            std::filesystem::rename(orig, bak, ec);
            if (ec) return false;
            sidecars.emplace_back(orig, bak);
            return true;
        };
        // Walk the volume chain from firstVolume until missing; also cover
        // arc_path itself when it's outside the chain (single-file source).
        // Cap must match the reader's MAX_VOLUME_CHAIN (65535): leaving
        // volumes beyond the cap un-renamed would let start_vol() truncate
        // them in place while stale later volumes survive — corrupting the
        // chain instead of rewriting it.
        {
            std::filesystem::path p = firstVolume;
            for (size_t i = 0; i < 65535 && std::filesystem::exists(p); ++i) {
                if (!do_rename(p)) return false;
                auto nxt = volume::next_volume_name(p, false);
                if (nxt == p) break;
                p = nxt;
            }
        }
        if (existing_path != firstVolume && std::filesystem::exists(existing_path)) {
            if (!do_rename(existing_path)) return false;
        }
        // Rewrite pe.extents to reference the sidecars.
        for (auto& pe : all_payloads) {
            for (auto& ex : pe.extents) {
                for (const auto& [orig, bak] : sidecars) {
                    if (ex.volume_path == orig) {
                        ex.volume_path = bak;
                        break;
                    }
                }
            }
        }
    }

    // Append new file at end. Move the packed buffer in so we don't hold two
    // copies of the file's bytes simultaneously (matters for large inputs).
    {
        volume_detail::PayloadEntry pe;
        pe.fb = base_fb;
        pe.unpacked_crc = unpacked_crc;
        pe.total_size = new_packed.size();
        pe.packed_mem = std::move(new_packed);
        all_payloads.push_back(std::move(pe));
    }

    // (The old chain-cleanup loop was a no-op — it enumerated volume names
    // but never actually removed anything, relying on start_vol()'s
    // CreateAlways to truncate. The sidecar rename above has already moved
    // existing volumes aside, so the canonical paths are guaranteed clear
    // by the time start_vol runs.)

    io::FileStream cur;
    std::filesystem::path curPath = firstVolume;
    int vol_idx = 0;
    std::vector<std::filesystem::path> created;

    struct VolumeCleanupGuard {
        io::FileStream* cur;
        std::vector<std::filesystem::path>* created;
        const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>* sidecars;
        bool committed{false};
        ~VolumeCleanupGuard() {
            if (committed) return;
            if (cur) cur->close();
            if (!created) return;
            for (const auto& p : *created) {
                bool in_sidecars = false;
                if (sidecars) {
                    for (const auto& [orig, bak] : *sidecars) {
                        if (p == orig) {
                            in_sidecars = true;
                            break;
                        }
                    }
                }
                if (!in_sidecars) {
                    std::error_code ec;
                    std::filesystem::remove(p, ec);
                }
            }
        }
    } vol_cleanup_guard{&cur, &created, &sidecars};

    // Helper to start volume
    auto start_vol = [&](std::filesystem::path path, int idx) -> bool {
        // close previous already handled
        if (!cur.open(path, io::FileMode::CreateAlways)) return false;
        if (!sfx_bytes.empty() && idx == 0) {
            if (cur.write(sfx_bytes.data(), sfx_bytes.size()) != sfx_bytes.size()) return false;
        }
        if (!format::HeaderWriter::write_signature(cur)) return false;
        format::MainBlock mb;
        mb.arc_flags = format::MHFL_VOLUME;
        if (vol_solid) mb.arc_flags |= format::MHFL_SOLID;
        if (idx != 0) {
            mb.arc_flags |= format::MHFL_VOLNUMBER;
            mb.vol_number = static_cast<core::uint64>(idx);
        }
        mb.has_locator = false;
        if (!format::HeaderWriter::write_main_block(cur, mb)) return false;
        created.push_back(path);
        return true;
    };
    if (!start_vol(curPath, vol_idx)) return false;

    std::vector<core::byte> slice_buf; // scratch used per output slice
    for (auto& pe : all_payloads) {
        core::uint64 total = pe.total_size;
        // Empty file (0 payload) needs single header without split
        if (total == 0) {
            // Ensure space for header+end
            core::uint64 space = (vol_size > cur.tell()) ? vol_size - cur.tell() : 0;
            if (space < volume::MAX_HEADER_SIZE_MARGIN + volume::ENDARC_SIZE + 10) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
            }
            format::FileBlock slice_fb = pe.fb;
            slice_fb.pack_size = 0;
            // no split flags for empty
            if (!format::HeaderWriter::write_file_block(cur, slice_fb, 0)) return false;
            continue;
        }
        core::uint64 written = 0;
        while (written < total) {
            core::uint64 cur_pos = cur.tell();
            if (vol_size <= cur_pos) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
                continue;
            }
            core::uint64 space = vol_size - cur_pos;
            core::int64 maxSlice = (core::int64)space -
                                   (core::int64)volume::MAX_HEADER_SIZE_MARGIN -
                                   (core::int64)volume::ENDARC_SIZE;
            if (maxSlice <= 0) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
                continue;
            }
            core::uint64 remaining = total - written;
            core::uint64 slice = std::min(remaining, static_cast<core::uint64>(maxSlice));
            if (slice == 0) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
                continue;
            }
            bool final = (written + slice >= total);
            bool split_before = written > 0;
            bool split_after = !final;
            format::FileBlock slice_fb = pe.fb;
            slice_fb.pack_size = static_cast<core::int64>(slice);

            // Pull this slice's bytes into the scratch buffer. For existing
            // extents-backed entries this streams from disk; for the new
            // in-memory entry it's a plain memcpy.
            if (!volume_detail::read_packed_slice(pe, written, slice, slice_buf)) {
                cur.close();
                return false;
            }
            if (!final) {
                crypto::Crc32 slice_crc;
                slice_crc.update(slice_buf.data(), slice_buf.size());
                slice_fb.data_crc32 = slice_crc.get();
                slice_fb.has_crc32 = true;
            } else {
                slice_fb.data_crc32 = pe.unpacked_crc;
                slice_fb.has_crc32 = true;
            }
            core::uint64 extra_flags = (split_before ? format::HFL_SPLITBEFORE : 0) |
                                       (split_after ? format::HFL_SPLITAFTER : 0);
            if (!format::HeaderWriter::write_file_block(cur, slice_fb, extra_flags)) {
                cur.close();
                return false;
            }
            if (cur.write(slice_buf.data(), static_cast<size_t>(slice)) != slice) {
                cur.close();
                return false;
            }
            written += slice;
            if (!final) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
            }
        }
    }
    // finalize last volume with end flag 0
    {
        format::EndArcBlock eb;
        eb.end_flags = 0;
        format::HeaderWriter::write_end_block(cur, eb);
        cur.close();
    }
    // If firstVolume != arc_path and original single file exists, remove it to avoid confusion
    if (firstVolume != arc_path && std::filesystem::exists(arc_path)) {
        std::error_code ec;
        // Only remove if arc_path is not one of created volumes
        bool is_created = false;
        for (auto& p : created)
            if (p == arc_path) is_created = true;
        if (!is_created) std::filesystem::remove(arc_path, ec);
    }
    // Delete trailing stale volumes beyond created set (up to next 100).
    // Sidecars have `.mv_bak` suffixes so this only catches leftover volumes
    // from a previous longer chain.
    {
        std::filesystem::path p = volume::next_volume_name(created.back(), false);
        for (int i = 0; i < 100; ++i) {
            if (!std::filesystem::exists(p)) break;
            std::error_code ec;
            std::filesystem::remove(p, ec);
            auto nxt = volume::next_volume_name(p, false);
            if (nxt == p) break;
            p = nxt;
        }
    }
    sidecar_guard.committed = true; // success: guard will drop .mv_bak files
    vol_cleanup_guard.committed = true;
    return true;
}

bool ArchiveMutator::add_file_to_archive(const std::filesystem::path& arc_path,
                                         const std::filesystem::path& src_file,
                                         const std::string& arc_entry_name, int method,
                                         const std::filesystem::path& sfx_stub_path,
                                         core::uint64 vol_size, const std::string& password,
                                         bool encrypt_headers, bool solid) {
    if (vol_size != 0 && vol_size != volume::VOLSIZE_AUTO) {
        return add_file_to_archive_vol(arc_path, src_file, arc_entry_name, method, vol_size,
                                       password, solid);
    }
    return add_or_move_file(arc_path, src_file, arc_entry_name, false, method, sfx_stub_path,
                            password, encrypt_headers);
}

bool ArchiveMutator::move_file_to_archive_vol(const std::filesystem::path& arc_path,
                                              const std::filesystem::path& src_file,
                                              const std::string& arc_entry_name, int method,
                                              core::uint64 vol_size, const std::string& password,
                                              bool solid) {
    bool ok = add_file_to_archive_vol(arc_path, src_file, arc_entry_name, method, vol_size,
                                      password, solid);
    if (ok) {
        std::error_code ec;
        std::filesystem::remove(src_file, ec);
        return !ec || !std::filesystem::exists(src_file);
    }
    return false;
}

bool ArchiveMutator::move_file_to_archive(const std::filesystem::path& arc_path,
                                          const std::filesystem::path& src_file,
                                          const std::string& arc_entry_name, int method,
                                          const std::filesystem::path& sfx_stub_path,
                                          const std::string& password, bool encrypt_headers) {
    return add_or_move_file(arc_path, src_file, arc_entry_name, true, method, sfx_stub_path,
                            password, encrypt_headers);
}

std::filesystem::path ArchiveMutator::resolve_sfx_stub(const std::string& sfx_name_raw,
                                                       const char* argv0) {
    std::string name = sfx_name_raw;
    // Strip leading '=' if user passed -sfx=Default.sfx style
    if (!name.empty() && name[0] == '=') name = name.substr(1);
    if (name.empty()) name = "default.sfx";
    std::filesystem::path p(name);
    if (is_path_qualified(name)) {
        return p;
    }
    // Bare name: cwd -> exe dir
    std::filesystem::path cwd_try = std::filesystem::current_path() / p;
    if (std::filesystem::exists(cwd_try)) return cwd_try;
    auto exe_dir = get_exe_dir(argv0);
    if (!exe_dir.empty()) {
        std::filesystem::path exe_try = exe_dir / p;
        if (std::filesystem::exists(exe_try)) return exe_try;
    }
    // Return cwd attempt for error message (will fail open)
    return cwd_try;
}

std::filesystem::path ArchiveMutator::apply_sfx_extension(const std::filesystem::path& arc_path) {
    std::string ext = arc_path.extension().string();
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == ".exe" || lower == ".sfx") return arc_path;
    // Bare name handling: if no extension, add .exe via replace_extension
    std::filesystem::path out = arc_path;
    out.replace_extension(".exe");
    return out;
}

} // namespace openrar::archive
