#include "archive_mutator.hpp"
#include "archive_reader.hpp"
#include "volume.hpp"
#include "rar_errors.hpp" // RAR_* status codes for the mutation variants
#include "../compress/compressor50.hpp"
#include "../compress/stream_encoder.hpp"
#include "../core/vint.hpp"
#include "../format/header_writer.hpp"
#include "../format/header_reader.hpp"
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
#include <iostream>
#include <optional>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#endif

namespace openrar::archive {

namespace {

struct TempFileCleanupGuard {
    io::FileStream* stream{nullptr};
    std::filesystem::path path;
    bool committed{false};

    void commit() noexcept { committed = true; }

    ~TempFileCleanupGuard() {
        if (committed) return;
        if (stream && stream->is_open()) stream->close();
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

// Returns false on any short read or write so callers can abort.
bool copy_stream_region(io::FileStream& src, io::FileStream& dest, core::uint64 src_offset,
                        core::uint64 size) {
    src.seek(static_cast<core::int64>(src_offset), io::SeekOrigin::Begin);
    std::vector<core::byte> buf(1048576); // 1 MiB transfer buffer for mechanical sympathy
    core::uint64 remaining = size;

    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(buf.size())));
        if (src.read(buf.data(), take) != take) return false;
        if (dest.write(buf.data(), take) != take) return false;
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
#ifdef _WIN32
    core::uint64 mtime_win{0};
    core::uint64 ctime_win{0};
    core::uint64 atime_win{0};
    bool has_win_times{false};
#endif
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
        const core::uint64 EPOCH_DIFFERENCE = 11644473600ULL;
        core::uint64 sec = ul.QuadPart / 10000000ULL;
        if (sec < EPOCH_DIFFERENCE) return 0; // Pre-1970 clamps utime to 0 to prevent uint64 underflow
        return sec - EPOCH_DIFFERENCE;
    };
    auto to_win = [](const FILETIME& ft) -> core::uint64 {
        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        return ul.QuadPart;
    };
    out.mtime = to_unix(fa.ftLastWriteTime);
    out.ctime = to_unix(fa.ftCreationTime);
    out.atime = to_unix(fa.ftLastAccessTime);
    out.mtime_win = to_win(fa.ftLastWriteTime);
    out.ctime_win = to_win(fa.ftCreationTime);
    out.atime_win = to_win(fa.ftLastAccessTime);
    out.has_win_times = true;
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
#ifdef _WIN32
    if (times.has_win_times) {
        fb.htime_is_unix = false;
        if (times_mask & time_flags::MTIME) fb.mtime_win = times.mtime_win;
        if (times_mask & time_flags::CTIME) fb.ctime_win = times.ctime_win;
        if (times_mask & time_flags::ATIME) fb.atime_win = times.atime_win;
        return;
    }
#endif
    if (times_mask & time_flags::MTIME)
        fb.htime_mtime_unix = static_cast<core::uint32>(times.mtime);
    if (times_mask & time_flags::CTIME)
        fb.htime_ctime_unix = static_cast<core::uint32>(times.ctime);
    if (times_mask & time_flags::ATIME)
        fb.htime_atime_unix = static_cast<core::uint32>(times.atime);
    if (fb.htime_mtime_unix != 0 || fb.htime_ctime_unix != 0 || fb.htime_atime_unix != 0)
        fb.htime_is_unix = true;
}

#ifndef _WIN32
void apply_unix_owner(format::FileBlock& fb, const std::filesystem::path& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return;
    fb.has_owner = true;
    fb.has_owner_uid = true;
    fb.owner_uid = static_cast<core::uint64>(st.st_uid);
    fb.has_owner_gid = true;
    fb.owner_gid = static_cast<core::uint64>(st.st_gid);

    long bufsize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize <= 0) bufsize = 1024;
    std::vector<char> buf(static_cast<size_t>(bufsize));
    struct passwd pwd;
    std::memset(&pwd, 0, sizeof(pwd));
    struct passwd* result = nullptr;
    while (true) {
        int rc = ::getpwuid_r(st.st_uid, &pwd, buf.data(), buf.size(), &result);
        if (rc == 0) {
            if (result && result->pw_name) {
                fb.owner_user = result->pw_name;
            }
            break;
        } else if (rc == ERANGE && buf.size() < 65536) {
            buf.resize(buf.size() * 2);
        } else {
            break;
        }
    }

    long gbufsize = ::sysconf(_SC_GETGR_R_SIZE_MAX);
    if (gbufsize <= 0) gbufsize = 1024;
    std::vector<char> gbuf(static_cast<size_t>(gbufsize));
    struct group grp;
    std::memset(&grp, 0, sizeof(grp));
    struct group* gresult = nullptr;
    while (true) {
        int rc = ::getgrgid_r(st.st_gid, &grp, gbuf.data(), gbuf.size(), &gresult);
        if (rc == 0) {
            if (gresult && gresult->gr_name) {
                fb.owner_group = gresult->gr_name;
            }
            break;
        } else if (rc == ERANGE && gbuf.size() < 65536) {
            gbuf.resize(gbuf.size() * 2);
        } else {
            break;
        }
    }
}
#endif

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

bool ArchiveMutator::lock_archive(const std::filesystem::path& arc_path,
                                   const std::string& password) {
    std::filesystem::path actual_path = arc_path;
    if (!std::filesystem::exists(actual_path)) {
        std::filesystem::path first = volume::first_volume_name(arc_path, false);
        if (std::filesystem::exists(first)) {
            actual_path = first;
        }
    }
    ArchiveReader reader;
    if (!password.empty()) {
        reader.set_password(password);
    }
    if (!reader.open(actual_path, password)) {
        return false;
    }

    if (reader.is_locked()) {
        return true; // Already locked
    }
    if (reader.is_volume()) {
        std::filesystem::path cur_vol = volume::first_volume_name(actual_path, false);
        if (!std::filesystem::exists(cur_vol)) {
            cur_vol = actual_path;
        }
        std::vector<std::filesystem::path> vols;
        while (std::filesystem::exists(cur_vol) && vols.size() < 65535) {
            vols.push_back(cur_vol);
            cur_vol = volume::next_volume_name(cur_vol, false);
        }
        if (vols.empty()) return false;
        format::CryptBlock cb = reader.header_crypt();
        bool is_enc = reader.is_header_encrypted();
        core::uint64 first_sfx = reader.sfx_offset();
        reader.close();

        for (size_t vi = 0; vi < vols.size(); ++vi) {
            const auto& v = vols[vi];
            core::uint64 sfx_off = (vi == 0) ? first_sfx : 0;
            io::FileStream in_s;
            if (!in_s.open(v, io::FileMode::ReadOnly)) return false;

            std::filesystem::path tmp_path = mutation_temp_path(v, "lck_tmp");
            io::FileStream out;
            if (!out.open(tmp_path, io::FileMode::CreateNew)) return false;
            try {
                if (sfx_off > 0) {
                    if (!copy_stream_region(in_s, out, 0, sfx_off)) {
                        out.close();
                        in_s.close();
                        std::filesystem::remove(tmp_path);
                        return false;
                    }
                }
                in_s.seek(static_cast<core::int64>(sfx_off), io::SeekOrigin::Begin);
                if (!format::HeaderReader::read_signature(in_s)) {
                    out.close();
                    in_s.close();
                    std::filesystem::remove(tmp_path);
                    return false;
                }
                format::HeaderWriter::write_signature(out);

                format::HeaderCryptReader hcr;
                format::HeaderCryptWriter hcw;
                if (is_enc) {
                    if (password.empty() || !hcr.init(password, cb) || !hcw.init_existing(password, cb)) {
                        out.close();
                        in_s.close();
                        std::filesystem::remove(tmp_path);
                        return false;
                    }
                    core::uint64 ctype = 0, cflags = 0, cdata_size = 0;
                    std::vector<core::byte> cbody;
                    auto cres = format::HeaderReader::read_block_raw(in_s, ctype, cflags, cbody, cdata_size, nullptr);
                    if (cres != format::HeaderResult::Ok || ctype != format::HEAD_CRYPT) {
                        out.close();
                        in_s.close();
                        std::filesystem::remove(tmp_path);
                        return false;
                    }
                    if (!format::HeaderWriter::write_crypt_block(out, cb)) {
                        out.close();
                        in_s.close();
                        std::filesystem::remove(tmp_path);
                        return false;
                    }
                }

                core::uint64 mtype = 0, mflags = 0, mdata_size = 0;
                std::vector<core::byte> mbody;
                auto mres = format::HeaderReader::read_block_raw(in_s, mtype, mflags, mbody, mdata_size, is_enc ? &hcr : nullptr);
                if (mres != format::HeaderResult::Ok || mtype != format::HEAD_MAIN) {
                    out.close();
                    in_s.close();
                    std::filesystem::remove(tmp_path);
                    return false;
                }
                format::MainBlock mb;
                if (!format::HeaderReader::parse_main_header(mbody.data(), mbody.size(), mb)) {
                    out.close();
                    in_s.close();
                    std::filesystem::remove(tmp_path);
                    return false;
                }
                mb.arc_flags |= format::MHFL_LOCK;
                if (!format::HeaderWriter::write_main_block(out, mb, is_enc ? &hcw : nullptr)) {
                    out.close();
                    in_s.close();
                    std::filesystem::remove(tmp_path);
                    return false;
                }

                core::uint64 cur_in_pos = in_s.tell();
                core::uint64 total_in_sz = in_s.size();
                if (total_in_sz > cur_in_pos) {
                    if (!copy_stream_region(in_s, out, cur_in_pos, total_in_sz - cur_in_pos)) {
                        out.close();
                        in_s.close();
                        std::filesystem::remove(tmp_path);
                        return false;
                    }
                }

                in_s.close();
                out.close();
                std::error_code ec;
                std::filesystem::remove(v, ec);
                std::filesystem::rename(tmp_path, v, ec);
                if (ec) {
                    std::filesystem::remove(tmp_path, ec);
                    return false;
                }
            } catch (...) {
                out.close();
                in_s.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                return false;
            }
        }
        return true;
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

        format::HeaderCryptWriter hcw;
        bool is_enc = reader.is_header_encrypted();
        if (is_enc) {
            if (password.empty() || !hcw.init_existing(password, reader.header_crypt())) {
                out.close();
                std::filesystem::remove(tmp_path);
                return false;
            }
            if (!format::HeaderWriter::write_crypt_block(out, reader.header_crypt())) {
                out.close();
                std::filesystem::remove(tmp_path);
                return false;
            }
        }

        // Main block with MHFL_LOCK set
        format::MainBlock mb = reader.main_block();
        mb.arc_flags |= format::MHFL_LOCK;
        format::HeaderWriter::write_main_block(out, mb, is_enc ? &hcw : nullptr);

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
        format::HeaderWriter::write_end_block(out, eb, is_enc ? &hcw : nullptr);

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

    std::error_code ren_ec;
    std::filesystem::remove(arc_path, ren_ec);
    std::filesystem::rename(tmp_path, arc_path, ren_ec);
    if (ren_ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        return false;
    }
    return true;
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
    std::vector<core::byte> buf(262144);
    core::uint64 remaining = sz;
    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(buf.size())));
        if (stub.read(buf.data(), take) != take) return false;
        if (out.write(buf.data(), take) != take) return false;
        remaining -= take;
    }
    return true;
}

static void apply_owner_overrides(format::FileBlock& fb, const std::string& default_group,
                                    const std::string& default_user) {
    if (!default_group.empty()) {
        fb.has_owner = true;
        bool all_digits = std::all_of(default_group.begin(), default_group.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
        });
        if (all_digits) {
            try {
                fb.owner_gid = std::stoull(default_group);
                fb.has_owner_gid = true;
            } catch (...) {
                fb.owner_group = default_group;
            }
        } else {
            fb.owner_group = default_group;
        }
    }
    if (!default_user.empty()) {
        fb.has_owner = true;
        bool all_digits = std::all_of(default_user.begin(), default_user.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
        });
        if (all_digits) {
            try {
                fb.owner_uid = std::stoull(default_user);
                fb.has_owner_uid = true;
            } catch (...) {
                fb.owner_user = default_user;
            }
        } else {
            fb.owner_user = default_user;
        }
    }
}

bool ArchiveMutator::prepare_add_file(const std::filesystem::path& src_file,
                                      const std::string& arc_entry_name, int method,
                                      const std::string& password, PreparedAdd& out,
                                      core::uint32 times_mask, core::uint64 dict_size,
                                      bool want_streams, bool want_acl, bool is_solid,
                                      bool direct_stream,
                                      const compress::FilterConfig& filter_cfg,
                                      const std::string& default_group,
                                      const std::string& default_user) {
    if (!std::filesystem::exists(src_file)) {
        return false;
    }

    // Dictionary window: dict_size 1..15 -> 128 KiB..2 GiB (backward compatibility);
    // 0 uses tuned defaults per method (8 MB for -m3, 64 MB for -m5);
    // > 15 is treated directly as exact byte sizes (supporting non-power-of-two and > 2 GiB).
    // Must match the win_size passed to Compressor50 and written into the header.
    core::uint64 win_size = 0x800000ULL;
    if (dict_size >= 1 && dict_size <= 15) {
        win_size = 0x20000ULL << (dict_size - 1);
    } else if (dict_size == 0) {
        switch (method) {
        case 0:
            win_size = 0x20000ULL;
            break; // 128 KB
        case 1:
            win_size = 0x80000ULL;
            break; // 512 KB
        case 2:
            win_size = 0x100000ULL;
            break; // 1 MB
        case 3:
            win_size = 0x800000ULL;
            break; // 8 MB
        case 4:
            win_size = 0x1000000ULL;
            break; // 16 MB
        case 5:
            win_size = 0x4000000ULL;
            break; // 64 MB
        default:
            win_size = 0x800000ULL;
            break;
        }
    } else {
        win_size = dict_size;
    }

    core::uint64 file_sz = 0;
    core::uint32 crc = 0;
    bool is_spooled = false;
    std::filesystem::path spool_path_used;
    SpoolFileGuard spool_guard;
    std::vector<core::byte> payload_to_write;

    format::FileBlock fb;
    fb.file_name = arc_entry_name;
    fb.attributes = 0x20;
    bool is_non_pow2 = (win_size & (win_size - 1)) != 0;
    fb.unp_ver = (method > 0 && (win_size > (1ULL * 1024 * 1024 * 1024) || is_non_pow2)) ? 1 : 0;
    FileTimes times;
    if (get_file_times(src_file, times)) {
        apply_file_times(fb, times, times_mask);
    } else {
        fb.utime_unix = static_cast<core::uint32>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    {
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) {
            return false;
        }
        file_sz = src.size();
        fb.unp_size = file_sz;

        if (dict_size == 0 && !is_solid && method > 0 && file_sz > 0) {
            core::uint64 file_pow2 = 0x20000ULL; // 128 KiB floor
            while (file_pow2 < file_sz && file_pow2 < win_size) {
                file_pow2 <<= 1;
            }
            win_size = std::min(win_size, file_pow2);
        }

        if (file_sz == 0) {
            fb.pack_size = 0;
            fb.data_crc32 = 0;
            fb.has_crc32 = true;
            fb.method = 0;
            fb.win_size = 0;
            src.close();
        } else if (file_sz <= SPOOL_MEMORY_THRESHOLD) {
            // Fast in-memory path for files <= 16 MiB
            std::vector<core::byte> uncompressed(static_cast<size_t>(file_sz));
            if (src.read(uncompressed.data(), uncompressed.size()) != uncompressed.size()) {
                return false;
            }
            src.close();
            crypto::Crc32 crc_calc;
            crc_calc.update(uncompressed.data(), uncompressed.size());
            crc = crc_calc.get();

            std::vector<core::byte> compressed_payload;
            if (method > 0) {
                if (compress::Compressor50::compress_buffer(uncompressed.data(),
                                                            uncompressed.size(), compressed_payload,
                                                            method, win_size, filter_cfg)) {
                    if (compressed_payload.size() >= uncompressed.size()) {
                        compressed_payload.clear();
                        method = 0;
                    }
                } else {
                    method = 0;
                }
            }

            if (!compressed_payload.empty())
                payload_to_write = std::move(compressed_payload);
            else
                payload_to_write = std::move(uncompressed);

            fb.pack_size = static_cast<core::int64>(payload_to_write.size());
            fb.data_crc32 = crc;
            fb.has_crc32 = true;
            fb.method = static_cast<core::uint32>(method);
            fb.win_size = (method > 0) ? win_size : 0;
            if (method == 0) fb.unp_ver = 0;

            if (!password.empty()) {
                if (!encrypt_file_payload(payload_to_write, fb, password)) {
                    return false;
                }
            }
        } else {
            // Streaming path for files > 16 MiB
            bool do_encrypt = !password.empty();
            constexpr core::uint8 LG2_COUNT = 15;
            constexpr size_t SALT_LEN = 16;
            constexpr size_t IV_LEN = 16;
            core::byte salt[SALT_LEN];
            core::byte iv[IV_LEN];
            core::byte iv_stream[IV_LEN];
            crypto::Rar5Keys keys;
            std::unique_ptr<crypto::Aes256> aes;
            std::vector<core::byte> enc_residual;
            core::uint64 spooled_pack_bytes = 0;

            if (do_encrypt) {
                if (!crypto::secure_random_bytes(salt, SALT_LEN) ||
                    !crypto::secure_random_bytes(iv, IV_LEN)) {
                    return false;
                }
                crypto::Pbkdf2Rar5::derive_keys(password, salt, SALT_LEN, 1U << LG2_COUNT, keys);
                std::memcpy(iv_stream, iv, IV_LEN);
                aes = std::make_unique<crypto::Aes256>(keys.aes_key);
            }

            if (method == 0 && !do_encrypt) {
                // Uncompressed, unencrypted large file: no temp spool needed!
                // Defer CRC32 calculation to write_batch_add_ex streaming pass.
                // write_batch_add_ex will stream the payload directly into the archive
                // in 1 MiB chunks while computing CRC32 on-the-fly, and back-patch the header.
                // Total I/O drops from 3x to 2x (8.0 GiB vs 12.0 GiB for a 4.0 GiB file).
                src.close();
                fb.pack_size = static_cast<core::int64>(file_sz);
                fb.data_crc32 = 0; // Deterministic placeholder for header back-patching
                fb.has_crc32 = true;
                fb.method = 0;
                fb.win_size = 0;
                out.needs_deferred_crc = true;
            } else if (method > 0 && !do_encrypt && direct_stream) {
                // Direct-to-archive streaming compression via fixed-width vint back-patching:
                // No temp spool needed! write_batch_add_ex will stream-compress directly into
                // the archive in 1 MiB chunks, compute CRC32 on-the-fly, and back-patch the
                // 10-byte fixed-width vint header. Eliminates intermediate disk spooling.
                src.close();
                fb.pack_size = 0;  // 10-byte fixed vint placeholder
                fb.data_crc32 = 0; // Deterministic placeholder for header back-patching
                fb.has_crc32 = true;
                fb.method = static_cast<core::uint32>(method);
                fb.win_size = win_size;
                fb.unp_ver = (win_size > (1ULL * 1024 * 1024 * 1024) || is_non_pow2) ? 1 : 0;
                out.needs_direct_stream = true;
            } else {
                // Compressed or encrypted streaming to temporary spool file
                std::filesystem::path spool_p = mutation_temp_path(src_file, "spool_tmp");
                io::FileStream spool_out;
                if (!spool_out.open(spool_p, io::FileMode::CreateNew)) {
                    std::error_code ec;
                    spool_p = mutation_temp_path(std::filesystem::temp_directory_path() / "openrar", "spool_tmp");
                    std::filesystem::create_directories(spool_p.parent_path(), ec);
                    if (!spool_out.open(spool_p, io::FileMode::CreateNew)) {
                        return false;
                    }
                }
                spool_guard.reset(spool_p);

                auto write_spool_chunk = [&](const core::byte* data, size_t size) -> bool {
                    if (size == 0) return true;
                    if (!do_encrypt) {
                        if (spool_out.write(data, size) != size) return false;
                        spooled_pack_bytes += size;
                        return true;
                    }
                    enc_residual.insert(enc_residual.end(), data, data + size);
                    size_t complete_blocks = (enc_residual.size() / 16) * 16;
                    if (complete_blocks > 0) {
                        if (!aes->encrypt_cbc(enc_residual.data(), complete_blocks, iv_stream)) {
                            return false;
                        }
                        if (spool_out.write(enc_residual.data(), complete_blocks) != complete_blocks) {
                            return false;
                        }
                        spooled_pack_bytes += complete_blocks;
                        enc_residual.erase(enc_residual.begin(), enc_residual.begin() + complete_blocks);
                    }
                    return true;
                };

                crypto::Crc32 crc_calc;
                std::vector<core::byte> read_buf(1048576);
                core::uint64 remaining = file_sz;

                if (method > 0) {
                    compress::StreamEncoder encoder(method, win_size);
                    struct FlushCtx {
                        decltype(write_spool_chunk)* writer;
                        bool ok;
                    } fctx{&write_spool_chunk, true};
                    encoder.set_flush([](void* user, const core::byte* data, size_t size) -> int {
                        auto* ctx = static_cast<FlushCtx*>(user);
                        if (!(*ctx->writer)(data, size)) {
                            ctx->ok = false;
                            return -1;
                        }
                        return 0;
                    }, &fctx);

                    while (remaining > 0) {
                        size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                        if (src.read(read_buf.data(), to_read) != to_read) return false;
                        crc_calc.update(read_buf.data(), to_read);
                        if (!encoder.feed(read_buf.data(), to_read) || !fctx.ok) return false;
                        remaining -= to_read;
                    }
                    std::vector<core::byte> final_chunk;
                    if (!encoder.finish(final_chunk) || !fctx.ok) return false;
                    if (!final_chunk.empty()) {
                        if (!write_spool_chunk(final_chunk.data(), final_chunk.size())) return false;
                    }
                    if (do_encrypt && !enc_residual.empty()) {
                        size_t pad_need = 16 - enc_residual.size();
                        enc_residual.insert(enc_residual.end(), pad_need, core::byte(0));
                        if (!aes->encrypt_cbc(enc_residual.data(), 16, iv_stream)) return false;
                        if (spool_out.write(enc_residual.data(), 16) != 16) return false;
                        spooled_pack_bytes += 16;
                        enc_residual.clear();
                    }

                    // Store fallback check
                    if (spooled_pack_bytes >= file_sz) {
                        method = 0;
                        spool_out.close();
                        spool_guard.cleanup(); // removes the compressed spool
                        if (!do_encrypt) {
                            // Unencrypted store fallback: no spool needed!
                            spooled_pack_bytes = file_sz;
                        } else {
                            // Encrypted store fallback: stream read src_file, encrypt, and write to fresh spool
                            if (!spool_out.open(spool_p, io::FileMode::CreateNew)) return false;
                            spool_guard.reset(spool_p);
                            src.seek(0, io::SeekOrigin::Begin);
                            std::memcpy(iv_stream, iv, IV_LEN);
                            aes = std::make_unique<crypto::Aes256>(keys.aes_key);
                            spooled_pack_bytes = 0;
                            remaining = file_sz;
                            while (remaining > 0) {
                                size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                                if (src.read(read_buf.data(), to_read) != to_read) return false;
                                if (!write_spool_chunk(read_buf.data(), to_read)) return false;
                                remaining -= to_read;
                            }
                            if (!enc_residual.empty()) {
                                size_t pad_need = 16 - enc_residual.size();
                                enc_residual.insert(enc_residual.end(), pad_need, core::byte(0));
                                if (!aes->encrypt_cbc(enc_residual.data(), 16, iv_stream)) return false;
                                if (spool_out.write(enc_residual.data(), 16) != 16) return false;
                                spooled_pack_bytes += 16;
                                enc_residual.clear();
                            }
                            spool_out.close();
                            is_spooled = true;
                            spool_path_used = spool_p;
                            spool_guard.disarm();
                        }
                    } else {
                        spool_out.close();
                        is_spooled = true;
                        spool_path_used = spool_p;
                        spool_guard.disarm();
                    }
                } else {
                    // method == 0 && do_encrypt: stream encrypt directly to spool
                    while (remaining > 0) {
                        size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                        if (src.read(read_buf.data(), to_read) != to_read) return false;
                        crc_calc.update(read_buf.data(), to_read);
                        if (!write_spool_chunk(read_buf.data(), to_read)) return false;
                        remaining -= to_read;
                    }
                    if (!enc_residual.empty()) {
                        size_t pad_need = 16 - enc_residual.size();
                        enc_residual.insert(enc_residual.end(), pad_need, core::byte(0));
                        if (!aes->encrypt_cbc(enc_residual.data(), 16, iv_stream)) return false;
                        if (spool_out.write(enc_residual.data(), 16) != 16) return false;
                        spooled_pack_bytes += 16;
                        enc_residual.clear();
                    }
                    spool_out.close();
                    is_spooled = true;
                    spool_path_used = spool_p;
                    spool_guard.disarm();
                }

                src.close();
                crc = crc_calc.get();
                fb.pack_size = static_cast<core::int64>(spooled_pack_bytes);
                fb.data_crc32 = crc;
                fb.has_crc32 = true;
                fb.method = static_cast<core::uint32>(method);
                fb.win_size = (method > 0) ? win_size : 0;
                if (method == 0) fb.unp_ver = 0;
                if (do_encrypt) {
                    fb.is_encrypted = true;
                    fb.crypt_version = 0;
                    fb.crypt_flags = 0x01; // has psw_check
                    fb.lg2_count = LG2_COUNT;
                    std::memcpy(fb.salt.data(), salt, SALT_LEN);
                    std::memcpy(fb.init_v.data(), iv, IV_LEN);
                    fb.has_psw_check = true;
                    std::memcpy(fb.psw_check.data(), keys.psw_check, sizeof(keys.psw_check));
                    std::memcpy(fb.psw_check_csum.data(), keys.psw_check_csum, sizeof(keys.psw_check_csum));
                }
            }
        }
    }

    apply_owner_overrides(fb, default_group, default_user);
    out.fb = std::move(fb);
    out.src_path = src_file;
    if (is_spooled) {
        out.spool_path = std::move(spool_path_used);
        out.payload.clear();
    } else {
        out.payload = std::move(payload_to_write);
        out.spool_path.clear();
    }

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
#ifndef _WIN32
        apply_unix_owner(fb, src_file);
#else
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
#endif
    }

    // entry_name/src_path are caller-owned identity fields, filled before the
    // call: the batch writer reads them concurrently while this prepare may
    // still be running, so writing them here would be a data race (M4).
    return true;
}

bool ArchiveMutator::prepare_add_dir(const std::filesystem::path& src_dir,
                                     const std::string& arc_entry_name, PreparedAdd& out,
                                     core::uint32 times_mask, [[maybe_unused]] bool want_acl,
                                     const std::string& default_group,
                                     const std::string& default_user) {
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
#ifndef _WIN32
    if (want_acl) {
        apply_unix_owner(fb, src_dir);
    }
#endif
    apply_owner_overrides(fb, default_group, default_user);

    // entry_name/src_path stay caller-owned (see prepare_add_file, M4).
    out.fb = std::move(fb);
    return true;
}

bool ArchiveMutator::prepare_add_symlink(const std::filesystem::path& src_symlink,
                                         const std::string& arc_entry_name,
                                         const std::string& target, bool is_dir_target,
                                         PreparedAdd& out, core::uint32 times_mask,
                                         [[maybe_unused]] bool want_acl,
                                         const std::string& default_group,
                                         const std::string& default_user) {
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
#ifndef _WIN32
    if (want_acl) {
        apply_unix_owner(fb, src_symlink);
    }
#endif
    apply_owner_overrides(fb, default_group, default_user);
    out.fb = std::move(fb);
    return true;
}

bool ArchiveMutator::prepare_add_hardlink(const std::filesystem::path& src_file,
                                          const std::string& arc_entry_name,
                                          const std::string& target, PreparedAdd& out,
                                          core::uint32 times_mask, [[maybe_unused]] bool want_acl,
                                          const std::string& default_group,
                                          const std::string& default_user) {
    format::FileBlock fb;
    fb.file_name = arc_entry_name;
    fb.unp_size = 0;
    fb.pack_size = -1;
    fb.attributes = 0x20;
    fb.method = 0;
    fb.win_size = 0;
    fb.unp_ver = 0;
    fb.redir_type = 4; // HARDLINK
    fb.redir_dir_target = false;
    fb.redir_target = target;
    FileTimes times;
    if (get_file_times(src_file, times)) {
        apply_file_times(fb, times, times_mask);
    }
#ifndef _WIN32
    if (want_acl) {
        apply_unix_owner(fb, src_file);
    }
#endif
    apply_owner_overrides(fb, default_group, default_user);
    out.fb = std::move(fb);
    out.payload.clear();
    return true;
}

bool ArchiveMutator::get_file_mtime(const std::filesystem::path& path, core::uint64& mtime_out) {
    FileTimes ft;
    if (!get_file_times(path, ft)) return false;
    mtime_out = ft.mtime;
    return true;
}

compress::CompressPlan ArchiveMutator::plan_batch(const std::vector<PreparedAdd>& files, bool solid,
                                                  bool continue_solid_stream) {
    std::vector<compress::EntryPlan> requests;
    requests.reserve(files.size());
    for (const auto& pf : files) {
        compress::EntryPlan ep;
        ep.is_dir = (pf.fb.file_flags & format::FHFL_DIRECTORY) != 0;
        ep.method = static_cast<uint32_t>(pf.fb.method);
        ep.dict_size = pf.fb.win_size;
        ep.raw_size = pf.fb.unp_size;
        requests.push_back(ep);
    }
    return compress::CompressPlan::plan_entries(requests, solid, /*default_method=*/3,
                                                /*default_dict_size=*/0, continue_solid_stream);
}

int ArchiveMutator::write_batch_add_ex(
    const std::filesystem::path& arc_path, std::vector<PreparedAdd>& files,
    const std::filesystem::path& sfx_stub_path, const std::string& password, bool encrypt_headers,
    const std::function<void(size_t, const std::string&)>& on_write, bool solid,
    const std::vector<core::byte>& comment, std::string& detail_out, bool want_qo, bool want_ams,
    const compress::FilterConfig& filter_cfg, int max_versions) {
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
    TempFileCleanupGuard tmp_guard{&out, tmp_path};

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

        format::MainBlock written_main_block;
        core::uint64 main_header_pos = 0;
        struct QoIndex {
            core::uint64 orig_pos;
            size_t arena_offset;
            size_t size;
        };
        std::vector<core::byte> qo_arena;
        std::vector<QoIndex> qo_indices;

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
            std::vector<std::optional<format::FileBlock>> updated_headers(entries.size(), std::nullopt);

            if (max_versions < 0) {
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (entries[i].header.is_service) continue;
                    for (const auto& pf : files) {
                        if (entries[i].header.file_name == pf.entry_name) {
                            replaced[i] = 1;
                            for (size_t j = i + 1; j < entries.size() && entries[j].header.is_service;
                                 ++j) {
                                if (entries[j].header.service_type != "QO" &&
                                    entries[j].header.service_type != "CMT") {
                                    replaced[j] = 1;
                                }
                            }
                            break;
                        }
                    }
                }
            } else {
                // Versioning mode: max_versions >= 0
                for (const auto& pf : files) {
                    std::vector<size_t> hist_indices;
                    std::vector<size_t> curr_indices;
                    for (size_t i = 0; i < entries.size(); ++i) {
                        if (entries[i].header.is_service) continue;
                        if (entries[i].header.file_name == pf.entry_name) {
                            if (entries[i].header.has_file_version) {
                                hist_indices.push_back(i);
                            } else {
                                curr_indices.push_back(i);
                            }
                        }
                    }

                    // Sort historical versions by file_version
                    std::sort(hist_indices.begin(), hist_indices.end(), [&](size_t a, size_t b) {
                        return entries[a].header.file_version < entries[b].header.file_version;
                    });

                    // Any existing current (unversioned) entry becomes a historical version
                    for (size_t c_idx : curr_indices) {
                        hist_indices.push_back(c_idx);
                    }

                    // If max_versions > 0, prune oldest versions if total exceeds max_versions
                    if (max_versions > 0 && hist_indices.size() > static_cast<size_t>(max_versions)) {
                        size_t to_prune = hist_indices.size() - static_cast<size_t>(max_versions);
                        for (size_t p = 0; p < to_prune; ++p) {
                            size_t p_idx = hist_indices[p];
                            replaced[p_idx] = 1;
                            for (size_t j = p_idx + 1; j < entries.size() && entries[j].header.is_service; ++j) {
                                if (entries[j].header.service_type != "QO" &&
                                    entries[j].header.service_type != "CMT") {
                                    replaced[j] = 1;
                                }
                            }
                        }
                        // The remaining historical versions are renumbered 1..max_versions
                        for (size_t r = 0; r < static_cast<size_t>(max_versions); ++r) {
                            size_t r_idx = hist_indices[to_prune + r];
                            core::uint64 ver_num = r + 1;
                            format::FileBlock fb = entries[r_idx].header;
                            fb.has_file_version = true;
                            fb.file_version = ver_num;
                            updated_headers[r_idx] = fb;
                        }
                    } else {
                        // max_versions == 0 (unlimited) or doesn't exceed limit
                        core::uint64 next_ver = 1;
                        for (size_t h_idx : hist_indices) {
                            const format::FileBlock& fb = entries[h_idx].header;
                            if (fb.has_file_version) {
                                next_ver = std::max(next_ver, fb.file_version + 1);
                            }
                        }
                        for (size_t c_idx : curr_indices) {
                            format::FileBlock fb = entries[c_idx].header;
                            fb.has_file_version = true;
                            fb.file_version = next_ver++;
                            updated_headers[c_idx] = fb;
                        }
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

            written_main_block = reader.main_block();
            written_main_block.has_locator = false;
            if (want_ams) {
                written_main_block.has_metadata = true;
                written_main_block.metadata_name = arc_path.filename().string();
#ifdef _WIN32
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                written_main_block.metadata_ctime =
                    (static_cast<core::uint64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
                core::uint64 now_unix = static_cast<core::uint64>(std::time(nullptr));
                written_main_block.metadata_ctime = (now_unix + 11644473600ULL) * 10000000ULL;
#endif
                written_main_block.metadata_is_unix_time = false;
                written_main_block.metadata_is_nanoseconds = false;
            }
            if (want_qo) {
                written_main_block.has_locator = true;
                written_main_block.locator_qo_offset = 0;
            }
            if (solid) written_main_block.arc_flags |= format::MHFL_SOLID;
            main_header_pos = out.tell();
            format::HeaderWriter::write_main_block(out, written_main_block,
                                                   header_encrypt_mode ? &hcw : nullptr);

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
                if (i < updated_headers.size() && updated_headers[i].has_value()) {
                    const format::FileBlock& updated_fb = *updated_headers[i];
                    core::uint64 orig_pos = out.tell();
                    auto block_bytes = format::HeaderWriter::serialize_file_block(updated_fb, 0);
                    if (want_qo) {
                        size_t offset = qo_arena.size();
                        qo_arena.insert(qo_arena.end(), block_bytes.begin(), block_bytes.end());
                        qo_indices.push_back({orig_pos, offset, block_bytes.size()});
                    }
                    if (!format::HeaderWriter::emit_block(out, block_bytes,
                                                          header_encrypt_mode ? &hcw : nullptr)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "rewrite failed";
                        return RAR_ERR_IO;
                    }
                    if (entry.data_size > 0) {
                        if (!copy_stream_region(reader.stream(), out, entry.data_offset, entry.data_size)) {
                            out.close();
                            std::filesystem::remove(tmp_path);
                            detail_out = "rewrite failed";
                            return RAR_ERR_IO;
                        }
                    }
                } else {
                    if (want_qo) {
                        size_t offset = qo_arena.size();
                        std::vector<core::byte> old_hdr(static_cast<size_t>(entry.header_size));
                        if (reader.stream().seek(static_cast<core::int64>(entry.header_offset),
                                                 io::SeekOrigin::Begin) &&
                            reader.stream().read(old_hdr.data(),
                                                 static_cast<size_t>(entry.header_size)) ==
                                entry.header_size) {
                            qo_arena.insert(qo_arena.end(), old_hdr.begin(), old_hdr.end());
                            qo_indices.push_back(
                                {out.tell(), offset, static_cast<size_t>(entry.header_size)});
                        }
                    }
                    if (!copy_stream_region(reader.stream(), out, entry.header_offset,
                                            entry.header_size + entry.data_size)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "rewrite failed";
                        return RAR_ERR_IO;
                    }
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

            written_main_block = format::MainBlock{};
            written_main_block.arc_flags = solid ? format::MHFL_SOLID : 0;
            if (want_ams) {
                written_main_block.has_metadata = true;
                written_main_block.metadata_name = arc_path.filename().string();
#ifdef _WIN32
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                written_main_block.metadata_ctime =
                    (static_cast<core::uint64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
                core::uint64 now_unix = static_cast<core::uint64>(std::time(nullptr));
                written_main_block.metadata_ctime = (now_unix + 11644473600ULL) * 10000000ULL;
#endif
                written_main_block.metadata_is_unix_time = false;
                written_main_block.metadata_is_nanoseconds = false;
            }
            if (want_qo && !header_encrypt_mode) {
                written_main_block.has_locator = true;
                written_main_block.locator_qo_offset = 0;
            }
            main_header_pos = out.tell();
            format::HeaderWriter::write_main_block(out, written_main_block,
                                                   header_encrypt_mode ? &hcw : nullptr);
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
            core::uint64 cmt_orig_pos = out.tell();
            auto cmt_bytes = format::HeaderWriter::serialize_file_block(cmt, 0);
            if (want_qo && !header_encrypt_mode) {
                size_t offset = qo_arena.size();
                qo_arena.insert(qo_arena.end(), cmt_bytes.begin(), cmt_bytes.end());
                qo_indices.push_back({cmt_orig_pos, offset, cmt_bytes.size()});
            }
            if (!format::HeaderWriter::emit_block(out, cmt_bytes,
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
        // Plan stage: compute entry decisions (solid chaining, method, etc.)
        // across the batch (Directive: Plan/Schedule/Execute separation).
        compress::CompressPlan plan;
        plan.solid = solid;
        plan.continue_solid_stream = continue_solid_stream;
        plan.seen_compressed_entry = continue_solid_stream;

        for (size_t i = 0; i < files.size(); ++i) {
            PreparedAdd& pf = files[i];
            // entry_name is caller-owned and filled before the prepare jobs run —
            // reading it here is race-free (M4); fb/payload below are only read
            // after on_write has synchronized with the prepare job.
            if (on_write) on_write(i, pf.entry_name);

            compress::EntryPlan req;
            req.is_dir = (pf.fb.file_flags & format::FHFL_DIRECTORY) != 0;
            req.method = static_cast<uint32_t>(pf.fb.method);
            req.dict_size = pf.fb.win_size;
            req.raw_size = pf.fb.unp_size;
            compress::EntryPlan ep = plan.plan_next_entry(req);

            pf.fb.is_solid = ep.is_solid_chain;

            core::uint64 orig_pos = out.tell();
            auto block_bytes = format::HeaderWriter::serialize_file_block(
                pf.fb, 0, pf.needs_direct_stream);
            if (want_qo) {
                size_t offset = qo_arena.size();
                qo_arena.insert(qo_arena.end(), block_bytes.begin(), block_bytes.end());
                qo_indices.push_back({orig_pos, offset, block_bytes.size()});
            }
            if (!format::HeaderWriter::emit_block(out, block_bytes,
                                                  header_encrypt_mode ? &hcw : nullptr)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
            if (!pf.payload.empty()) {
                out.write(pf.payload.data(), pf.payload.size());
            } else if (!pf.spool_path.empty()) {
                io::FileStream spool_in;
                if (!spool_in.open(pf.spool_path, io::FileMode::ReadOnly)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "cannot read spool file";
                    return RAR_ERR_IO;
                }
                core::uint64 spool_sz = spool_in.size();
                if (!copy_stream_region(spool_in, out, 0, spool_sz)) {
                    spool_in.close();
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "spool stream copy failed";
                    return RAR_ERR_IO;
                }
                spool_in.close();
                std::error_code ec;
                std::filesystem::remove(pf.spool_path, ec);
                pf.spool_path.clear();
            } else if (pf.needs_direct_stream && pf.fb.unp_size > 0 && !pf.src_path.empty()) {
                io::FileStream src_in;
                if (!src_in.open(pf.src_path, io::FileMode::ReadOnly)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "cannot read source file for direct streaming compression";
                    return RAR_ERR_IO;
                }

                core::uint64 actual_pack_size = 0;
                crypto::Crc32 crc_calc;
                bool stream_ok = true;

                struct DirectFlushCtx {
                    io::FileStream* out_stream;
                    core::uint64* pack_bytes;
                    bool ok;
                } fctx{&out, &actual_pack_size, true};

                compress::StreamEncoder encoder(pf.fb.method, pf.fb.win_size, filter_cfg);
                encoder.set_flush([](void* user, const core::byte* data, size_t size) -> int {
                    auto* ctx = static_cast<DirectFlushCtx*>(user);
                    if (size > 0) {
                        if (ctx->out_stream->write(data, size) != size) {
                            ctx->ok = false;
                            return -1;
                        }
                        *(ctx->pack_bytes) += size;
                    }
                    return 0;
                }, &fctx);

                std::vector<core::byte> read_buf(1048576); // 1 MiB streaming buffer
                core::uint64 remaining = pf.fb.unp_size;
                while (remaining > 0) {
                    size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                    if (src_in.read(read_buf.data(), to_read) != to_read) {
                        stream_ok = false;
                        break;
                    }
                    crc_calc.update(read_buf.data(), to_read);
                    if (!encoder.feed(read_buf.data(), to_read) || !fctx.ok) {
                        stream_ok = false;
                        break;
                    }
                    remaining -= to_read;
                }

                if (stream_ok) {
                    std::vector<core::byte> final_chunk;
                    if (!encoder.finish(final_chunk) || !fctx.ok) {
                        stream_ok = false;
                    } else if (!final_chunk.empty()) {
                        if (out.write(final_chunk.data(), final_chunk.size()) != final_chunk.size()) {
                            stream_ok = false;
                        } else {
                            actual_pack_size += final_chunk.size();
                        }
                    }
                }

                src_in.close();

                if (!stream_ok) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "direct streaming compression failed";
                    return RAR_ERR_IO;
                }

                if (actual_pack_size >= pf.fb.unp_size) {
                    // Store fallback: compression expanded. Truncate to orig_pos and write store-mode entry.
                    if (!out.seek(static_cast<core::int64>(orig_pos), io::SeekOrigin::Begin) ||
                        !out.truncate(orig_pos)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to truncate archive for store fallback";
                        return RAR_ERR_IO;
                    }

                    pf.fb.method = 0;
                    pf.fb.win_size = 0;
                    pf.fb.unp_ver = 0;
                    pf.fb.pack_size = static_cast<core::int64>(pf.fb.unp_size);
                    pf.fb.data_crc32 = crc_calc.get();

                    auto store_block_bytes = format::HeaderWriter::serialize_file_block(pf.fb, 0, false);
                    if (!format::HeaderWriter::emit_block(out, store_block_bytes,
                                                          header_encrypt_mode ? &hcw : nullptr)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to write store header on compression expansion";
                        return RAR_ERR_IO;
                    }

                    if (want_qo && !qo_indices.empty()) {
                        qo_arena.resize(qo_indices.back().arena_offset);
                        size_t offset = qo_arena.size();
                        qo_arena.insert(qo_arena.end(), store_block_bytes.begin(), store_block_bytes.end());
                        qo_indices.back() = {orig_pos, offset, store_block_bytes.size()};
                    }

                    if (!src_in.open(pf.src_path, io::FileMode::ReadOnly)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "cannot reopen source file for store fallback";
                        return RAR_ERR_IO;
                    }

                    if (!copy_stream_region(src_in, out, 0, pf.fb.unp_size)) {
                        src_in.close();
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "source copy failed during store fallback";
                        return RAR_ERR_IO;
                    }
                    src_in.close();
                } else {
                    // Normal compression succeeded: back-patch FileBlock header with exact pack_size and CRC32
                    core::uint64 payload_end = out.tell();
                    pf.fb.pack_size = static_cast<core::int64>(actual_pack_size);
                    pf.fb.data_crc32 = crc_calc.get();

                    auto updated_block_bytes = format::HeaderWriter::serialize_file_block(pf.fb, 0, true);
                    if (updated_block_bytes.size() != block_bytes.size()) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "header size invariant broken on streaming pack_size back-patch";
                        return RAR_ERR_IO;
                    }

                    if (want_qo && !qo_indices.empty()) {
                        size_t arena_off = qo_indices.back().arena_offset;
                        std::memcpy(&qo_arena[arena_off], updated_block_bytes.data(), updated_block_bytes.size());
                    }

                    if (!out.seek(static_cast<core::int64>(orig_pos), io::SeekOrigin::Begin)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to seek to header for pack_size back-patch";
                        return RAR_ERR_IO;
                    }

                    if (!format::HeaderWriter::emit_block(out, updated_block_bytes,
                                                          header_encrypt_mode ? &hcw : nullptr)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to rewrite header for pack_size back-patch";
                        return RAR_ERR_IO;
                    }

                    if (!out.seek(static_cast<core::int64>(payload_end), io::SeekOrigin::Begin)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to seek to payload end after pack_size back-patch";
                        return RAR_ERR_IO;
                    }
                }
            } else if (pf.fb.method == 0 && pf.fb.unp_size > 0 && !pf.src_path.empty()) {
                io::FileStream src_in;
                if (!src_in.open(pf.src_path, io::FileMode::ReadOnly)) {
                    out.close();
                    std::filesystem::remove(tmp_path);
                    detail_out = "cannot read source file for store copy";
                    return RAR_ERR_IO;
                }
                if (pf.needs_deferred_crc) {
                    crypto::Crc32 crc_calc;
                    std::vector<core::byte> buf(1048576); // 1 MiB streaming buffer
                    core::uint64 remaining = pf.fb.unp_size;
                    while (remaining > 0) {
                        size_t take = static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(buf.size())));
                        if (src_in.read(buf.data(), take) != take) {
                            src_in.close();
                            out.close();
                            std::filesystem::remove(tmp_path);
                            detail_out = "source stream read failed";
                            return RAR_ERR_IO;
                        }
                        if (out.write(buf.data(), take) != take) {
                            src_in.close();
                            out.close();
                            std::filesystem::remove(tmp_path);
                            detail_out = "archive write failed";
                            return RAR_ERR_IO;
                        }
                        crc_calc.update(buf.data(), take);
                        remaining -= take;
                    }
                    src_in.close();

                    core::uint64 payload_end = out.tell();
                    pf.fb.data_crc32 = crc_calc.get();
                    auto updated_block_bytes = format::HeaderWriter::serialize_file_block(pf.fb, 0);
                    if (updated_block_bytes.size() != block_bytes.size()) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "header size invariant broken on CRC back-patch";
                        return RAR_ERR_IO;
                    }
                    if (want_qo && !qo_indices.empty()) {
                        size_t arena_off = qo_indices.back().arena_offset;
                        std::memcpy(&qo_arena[arena_off], updated_block_bytes.data(), updated_block_bytes.size());
                    }
                    if (!out.seek(static_cast<core::int64>(orig_pos), io::SeekOrigin::Begin)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to seek to header for CRC back-patch";
                        return RAR_ERR_IO;
                    }
                    if (!format::HeaderWriter::emit_block(out, updated_block_bytes,
                                                          header_encrypt_mode ? &hcw : nullptr)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to rewrite header for CRC back-patch";
                        return RAR_ERR_IO;
                    }
                    if (!out.seek(static_cast<core::int64>(payload_end), io::SeekOrigin::Begin)) {
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "failed to seek to payload end after CRC back-patch";
                        return RAR_ERR_IO;
                    }
                } else {
                    if (!copy_stream_region(src_in, out, 0, pf.fb.unp_size)) {
                        src_in.close();
                        out.close();
                        std::filesystem::remove(tmp_path);
                        detail_out = "source stream copy failed";
                        return RAR_ERR_IO;
                    }
                    src_in.close();
                }
            }
            // Free the payload as soon as it is on disk so peak memory tracks
            // the in-flight preparation set, not the whole batch.
            std::vector<core::byte>().swap(pf.payload);

            for (auto& child : pf.child_services) {
                core::uint64 child_orig_pos = out.tell();
                auto child_bytes = format::HeaderWriter::serialize_file_block(
                    child.fb, format::HFL_CHILD | format::HFL_INHERITED);
                if (want_qo) {
                    size_t offset = qo_arena.size();
                    qo_arena.insert(qo_arena.end(), child_bytes.begin(), child_bytes.end());
                    qo_indices.push_back({child_orig_pos, offset, child_bytes.size()});
                }
                format::HeaderWriter::emit_block(out, child_bytes,
                                                 header_encrypt_mode ? &hcw : nullptr);
                if (!child.payload.empty()) {
                    out.write(child.payload.data(), child.payload.size());
                }
                std::vector<core::byte>().swap(child.payload);
            }
        }

        if (want_qo && !header_encrypt_mode && !qo_indices.empty()) {
            core::uint64 qo_header_pos = out.tell();
            std::vector<core::byte> qo_payload;
            // Pre-size: each record costs 4 bytes (CRC32) + up to 3 bytes
            // (size vint for reasonable header sizes) + up to 22 bytes (3
            // vint fields: flags=0, dist, size) + idx.size header bytes from
            // qo_arena. Reserving arena_size + 30*N avoids all reallocations
            // for archives up to several thousand entries.
            qo_payload.reserve(qo_arena.size() + 30u * qo_indices.size());

            for (const auto& idx : qo_indices) {
                core::uint64 dist = qo_header_pos - idx.orig_pos;
                std::vector<core::byte> struct_body;
                // Reserve: 3 vints (≤10 bytes each) + idx.size payload.
                struct_body.reserve(30u + idx.size);
                core::push_vint(struct_body, 0);        // Flags = 0
                core::push_vint(struct_body, dist);     // Distance from start of QO header
                core::push_vint(struct_body, idx.size); // Data size
                struct_body.insert(struct_body.end(), qo_arena.begin() + idx.arena_offset,
                                   qo_arena.begin() + idx.arena_offset + idx.size);

                std::vector<core::byte> struct_size_vint;
                core::push_vint(struct_size_vint, struct_body.size());

                crypto::Crc32 crc;
                crc.update(struct_size_vint.data(), struct_size_vint.size());
                crc.update(struct_body.data(), struct_body.size());
                core::uint32 struct_crc = crc.get();

                qo_payload.push_back(static_cast<core::byte>(struct_crc & 0xFF));
                qo_payload.push_back(static_cast<core::byte>((struct_crc >> 8) & 0xFF));
                qo_payload.push_back(static_cast<core::byte>((struct_crc >> 16) & 0xFF));
                qo_payload.push_back(static_cast<core::byte>((struct_crc >> 24) & 0xFF));

                qo_payload.insert(qo_payload.end(), struct_size_vint.begin(),
                                  struct_size_vint.end());
                qo_payload.insert(qo_payload.end(), struct_body.begin(), struct_body.end());
            }


            format::FileBlock qo_block;
            qo_block.is_service = true;
            qo_block.service_type = "QO";
            qo_block.file_name = "QO";
            qo_block.unp_size = qo_payload.size();
            qo_block.pack_size = static_cast<core::int64>(qo_payload.size());
            qo_block.attributes = 0x20;
            qo_block.has_crc32 = true;
            crypto::Crc32 qo_crc;
            qo_crc.update(qo_payload.data(), qo_payload.size());
            qo_block.data_crc32 = qo_crc.get();
            qo_block.method = 0;
            qo_block.unp_ver = 0;
            qo_block.win_size = 0;

            if (!format::HeaderWriter::write_file_block(out, qo_block, 0,
                                                        header_encrypt_mode ? &hcw : nullptr)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
            if (out.write(qo_payload.data(), qo_payload.size()) != qo_payload.size()) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }

            core::uint64 end_after_qo = out.tell();

            written_main_block.has_locator = true;
            written_main_block.locator_qo_offset =
                static_cast<core::int64>(qo_header_pos - main_header_pos);
            if (!out.seek(static_cast<core::int64>(main_header_pos), io::SeekOrigin::Begin)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "seek failed";
                return RAR_ERR_IO;
            }
            if (!format::HeaderWriter::write_main_block(out, written_main_block,
                                                        header_encrypt_mode ? &hcw : nullptr)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "rewrite failed";
                return RAR_ERR_IO;
            }
            if (!out.seek(static_cast<core::int64>(end_after_qo), io::SeekOrigin::Begin)) {
                out.close();
                std::filesystem::remove(tmp_path);
                detail_out = "seek failed";
                return RAR_ERR_IO;
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
        tmp_guard.commit();

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
    const std::vector<core::byte>& comment, bool want_qo, bool want_ams,
    const compress::FilterConfig& filter_cfg, int max_versions) {
    std::string detail;
    return write_batch_add_ex(arc_path, files, sfx_stub_path, password, encrypt_headers, on_write,
                              solid, comment, detail, want_qo, want_ams, filter_cfg, max_versions) == RAR_OK;
}

static bool add_or_move_file(const std::filesystem::path& arc_path,
                             const std::filesystem::path& src_file,
                             const std::string& arc_entry_name, bool delete_source, int method = 3,
                             const std::filesystem::path& sfx_stub_path = {},
                             const std::string& password = "", bool encrypt_headers = false,
                             core::uint64 dict_size = 0,
                             const compress::FilterConfig& filter_cfg = {}) {
    ArchiveMutator::PreparedAdd prepared;
    prepared.entry_name = arc_entry_name;
    prepared.src_path = src_file;
    if (!ArchiveMutator::prepare_add_file(src_file, arc_entry_name, method, password, prepared,
                                          time_flags::MTIME, dict_size, false, false, false,
                                          /*direct_stream=*/true, filter_cfg)) {
        return false;
    }
    prepared.delete_source = delete_source;
    std::vector<ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prepared));
    return ArchiveMutator::write_batch_add(arc_path, batch, sfx_stub_path, password,
                                           encrypt_headers, {}, false, {}, false, false,
                                           filter_cfg);
}

bool ArchiveMutator::add_file_to_archive_vol(const std::filesystem::path& arc_path,
                                             const std::filesystem::path& src_file,
                                             const std::string& arc_entry_name, int method,
                                             core::uint64 vol_size, const std::string& password,
                                             bool solid, core::uint64 dict_size,
                                             const compress::FilterConfig& filter_cfg,
                                             bool encrypt_headers,
                                             const std::vector<core::byte>* comment,
                                             bool lock) {
    if (vol_size == 0 || vol_size == volume::VOLSIZE_AUTO) {
        return add_or_move_file(arc_path, src_file, arc_entry_name, false, method, {}, password, encrypt_headers, dict_size, filter_cfg);
    }
    if (vol_size < 1024) return false; // too small
    if (!std::filesystem::exists(src_file)) return false;
    if (encrypt_headers && password.empty()) return false;

    // Prepare new file payload using streaming compression (O(window) RAM invariant)
    core::uint64 file_sz = 0;
    {
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) return false;
        file_sz = src.size();
    }

    core::uint64 win_size = (dict_size > 0) ? dict_size : 0x200000ULL; // 2 MiB default
    if (dict_size == 0 && !solid && method > 0 && file_sz > 0) {
        core::uint64 file_pow2 = 0x20000ULL; // 128 KiB floor
        while (file_pow2 < file_sz && file_pow2 < win_size) {
            file_pow2 <<= 1;
        }
        win_size = std::min(win_size, file_pow2);
    }

    volume_detail::PayloadEntry new_pe;
    format::FileBlock& base_fb = new_pe.fb;
    base_fb.file_name = arc_entry_name;
    base_fb.unp_size = file_sz;
    base_fb.attributes = 0x20;
    base_fb.unp_ver = 0;
    FileTimes vol_times;
    if (get_file_times(src_file, vol_times))
        base_fb.utime_unix = static_cast<core::uint32>(vol_times.mtime);
    else
        base_fb.utime_unix = static_cast<core::uint32>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

    SpoolFileGuard spool_guard;

    if (file_sz == 0) {
        base_fb.pack_size = 0;
        base_fb.data_crc32 = 0;
        base_fb.has_crc32 = true;
        base_fb.method = 0;
        base_fb.win_size = 0;
        new_pe.total_size = 0;
        new_pe.unpacked_crc = 0;
    } else if (method == 0 && password.empty()) {
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) return false;
        crypto::Crc32 crc_calc;
        std::vector<core::byte> buf(1024 * 1024);
        core::uint64 rem = file_sz;
        while (rem > 0) {
            size_t take = static_cast<size_t>(std::min<core::uint64>(rem, buf.size()));
            if (src.read(buf.data(), take) != take) return false;
            crc_calc.update(buf.data(), take);
            rem -= take;
        }
        src.close();
        base_fb.pack_size = static_cast<core::int64>(file_sz);
        base_fb.data_crc32 = crc_calc.get();
        base_fb.has_crc32 = true;
        base_fb.method = 0;
        base_fb.win_size = 0;
        new_pe.extents = { archive::VolumeExtent{src_file, 0, file_sz} };
        new_pe.total_size = file_sz;
        new_pe.unpacked_crc = base_fb.data_crc32;
    } else if (file_sz <= SPOOL_MEMORY_THRESHOLD && method > 0 && password.empty()) {
        std::vector<core::byte> uncompressed(static_cast<size_t>(file_sz));
        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) return false;
        if (src.read(uncompressed.data(), uncompressed.size()) != uncompressed.size()) return false;
        src.close();

        crypto::Crc32 c;
        c.update(uncompressed.data(), uncompressed.size());
        core::uint32 unpacked_crc = c.get();

        std::vector<core::byte> compressed_payload;
        if (compress::Compressor50::compress_buffer(uncompressed.data(), uncompressed.size(),
                                                    compressed_payload, method, win_size, filter_cfg)) {
            if (compressed_payload.size() >= uncompressed.size()) {
                compressed_payload.clear();
                method = 0;
            }
        } else {
            method = 0;
        }

        if (method > 0) {
            new_pe.packed_mem = std::move(compressed_payload);
            base_fb.pack_size = static_cast<core::int64>(new_pe.packed_mem.size());
            base_fb.method = static_cast<core::uint32>(method);
            base_fb.win_size = win_size;
        } else {
            new_pe.packed_mem = std::move(uncompressed);
            base_fb.pack_size = static_cast<core::int64>(new_pe.packed_mem.size());
            base_fb.method = 0;
            base_fb.win_size = 0;
        }
        base_fb.data_crc32 = unpacked_crc;
        base_fb.has_crc32 = true;
        new_pe.total_size = new_pe.packed_mem.size();
        new_pe.unpacked_crc = unpacked_crc;
    } else {
        std::filesystem::path spool_p = mutation_temp_path(src_file, "spool_vol");
        io::FileStream spool_out;
        if (!spool_out.open(spool_p, io::FileMode::CreateNew)) {
            std::error_code ec;
            spool_p = mutation_temp_path(std::filesystem::temp_directory_path() / "openrar", "spool_vol");
            std::filesystem::create_directories(spool_p.parent_path(), ec);
            if (!spool_out.open(spool_p, io::FileMode::CreateNew)) {
                return false;
            }
        }
        spool_guard.reset(spool_p);

        constexpr size_t SALT_LEN = 16;
        constexpr size_t IV_LEN = 16;
        constexpr core::uint8 LG2_COUNT = 15;
        core::byte salt[SALT_LEN];
        core::byte iv[IV_LEN];
        core::byte iv_stream[IV_LEN];
        crypto::Rar5Keys keys;
        std::unique_ptr<crypto::Aes256> aes;
        std::vector<core::byte> enc_residual;
        core::uint64 spooled_pack_bytes = 0;
        const bool do_encrypt = !password.empty();

        if (do_encrypt) {
            if (!crypto::secure_random_bytes(salt, SALT_LEN) ||
                !crypto::secure_random_bytes(iv, IV_LEN)) {
                return false;
            }
            crypto::Pbkdf2Rar5::derive_keys(password, salt, SALT_LEN, 1U << LG2_COUNT, keys);
            std::memcpy(iv_stream, iv, IV_LEN);
            aes = std::make_unique<crypto::Aes256>(keys.aes_key);
        }

        auto write_spool_chunk = [&](const core::byte* data, size_t size) -> bool {
            if (size == 0) return true;
            if (!do_encrypt) {
                if (spool_out.write(data, size) != size) return false;
                spooled_pack_bytes += size;
                return true;
            }
            enc_residual.insert(enc_residual.end(), data, data + size);
            size_t complete_blocks = (enc_residual.size() / 16) * 16;
            if (complete_blocks > 0) {
                if (!aes->encrypt_cbc(enc_residual.data(), complete_blocks, iv_stream)) {
                    return false;
                }
                if (spool_out.write(enc_residual.data(), complete_blocks) != complete_blocks) {
                    return false;
                }
                spooled_pack_bytes += complete_blocks;
                enc_residual.erase(enc_residual.begin(), enc_residual.begin() + complete_blocks);
            }
            return true;
        };

        crypto::Crc32 crc_calc;
        std::vector<core::byte> read_buf(1024 * 1024);
        core::uint64 remaining = file_sz;

        io::FileStream src;
        if (!src.open(src_file, io::FileMode::ReadOnly)) return false;

        if (method > 0) {
            compress::StreamEncoder encoder(method, win_size);
            struct FlushCtx {
                decltype(write_spool_chunk)* writer;
                bool ok;
            } fctx{&write_spool_chunk, true};
            encoder.set_flush([](void* user, const core::byte* data, size_t size) -> int {
                auto* ctx = static_cast<FlushCtx*>(user);
                if (!(*ctx->writer)(data, size)) {
                    ctx->ok = false;
                    return -1;
                }
                return 0;
            }, &fctx);

            while (remaining > 0) {
                size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                if (src.read(read_buf.data(), to_read) != to_read) return false;
                crc_calc.update(read_buf.data(), to_read);
                if (!encoder.feed(read_buf.data(), to_read) || !fctx.ok) return false;
                remaining -= to_read;
            }
            std::vector<core::byte> final_chunk;
            if (!encoder.finish(final_chunk) || !fctx.ok) return false;
            if (!final_chunk.empty()) {
                if (!write_spool_chunk(final_chunk.data(), final_chunk.size())) return false;
            }
        } else {
            while (remaining > 0) {
                size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                if (src.read(read_buf.data(), to_read) != to_read) return false;
                crc_calc.update(read_buf.data(), to_read);
                if (!write_spool_chunk(read_buf.data(), to_read)) return false;
                remaining -= to_read;
            }
        }
        src.close();

        if (do_encrypt && !enc_residual.empty()) {
            size_t pad_len = 16 - (enc_residual.size() % 16);
            if (pad_len < 16) enc_residual.resize(enc_residual.size() + pad_len, 0);
            if (!aes->encrypt_cbc(enc_residual.data(), enc_residual.size(), iv_stream)) return false;
            if (spool_out.write(enc_residual.data(), enc_residual.size()) != enc_residual.size()) return false;
            spooled_pack_bytes += enc_residual.size();
            enc_residual.clear();
        }
        spool_out.close();

        // Check if compression expanded
        if (method > 0 && spooled_pack_bytes >= file_sz) {
            method = 0;
            if (!do_encrypt) {
                spool_guard.cleanup();
                base_fb.pack_size = static_cast<core::int64>(file_sz);
                base_fb.method = 0;
                base_fb.win_size = 0;
                new_pe.extents = { archive::VolumeExtent{src_file, 0, file_sz} };
                new_pe.total_size = file_sz;
            } else {
                spool_out.open(spool_p, io::FileMode::CreateAlways);
                std::memcpy(iv_stream, iv, IV_LEN);
                spooled_pack_bytes = 0;
                if (!src.open(src_file, io::FileMode::ReadOnly)) return false;
                remaining = file_sz;
                while (remaining > 0) {
                    size_t to_read = static_cast<size_t>(std::min<core::uint64>(remaining, read_buf.size()));
                    if (src.read(read_buf.data(), to_read) != to_read) return false;
                    if (!write_spool_chunk(read_buf.data(), to_read)) return false;
                    remaining -= to_read;
                }
                src.close();
                if (!enc_residual.empty()) {
                    size_t pad_len = 16 - (enc_residual.size() % 16);
                    if (pad_len < 16) enc_residual.resize(enc_residual.size() + pad_len, 0);
                    if (!aes->encrypt_cbc(enc_residual.data(), enc_residual.size(), iv_stream)) return false;
                    if (spool_out.write(enc_residual.data(), enc_residual.size()) != enc_residual.size()) return false;
                    spooled_pack_bytes += enc_residual.size();
                    enc_residual.clear();
                }
                spool_out.close();
                base_fb.pack_size = static_cast<core::int64>(spooled_pack_bytes);
                base_fb.method = 0;
                base_fb.win_size = 0;
                new_pe.extents = { archive::VolumeExtent{spool_p, 0, spooled_pack_bytes} };
                new_pe.total_size = spooled_pack_bytes;
            }
        } else {
            base_fb.pack_size = static_cast<core::int64>(spooled_pack_bytes);
            base_fb.method = static_cast<core::uint32>(method);
            base_fb.win_size = (method > 0) ? win_size : 0;
            new_pe.extents = { archive::VolumeExtent{spool_p, 0, spooled_pack_bytes} };
            new_pe.total_size = spooled_pack_bytes;
        }

        if (do_encrypt) {
            base_fb.is_encrypted = true;
            base_fb.crypt_version = 0;
            base_fb.crypt_flags = 0x01;
            base_fb.lg2_count = LG2_COUNT;
            std::memcpy(base_fb.salt.data(), salt, SALT_LEN);
            std::memcpy(base_fb.init_v.data(), iv, IV_LEN);
            base_fb.has_psw_check = true;
            std::memcpy(base_fb.psw_check.data(), keys.psw_check, sizeof(keys.psw_check));
            std::memcpy(base_fb.psw_check_csum.data(), keys.psw_check_csum, sizeof(keys.psw_check_csum));
        }
        base_fb.data_crc32 = crc_calc.get();
        base_fb.has_crc32 = true;
        new_pe.unpacked_crc = base_fb.data_crc32;
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
    format::HeaderCryptWriter hcw;
    format::CryptBlock new_crypt;
    bool header_encrypt_mode = false;

    if (!existing_path.empty()) {
        ArchiveReader r;
        if (!password.empty()) {
            r.set_password(password);
        }
        // Hard guard: never proceed when the existing archive cannot be
        // opened (header-encrypted without password, corrupt file, IO
        // error). Continuing would rename every volume aside and write a
        // fresh chain containing only the new file — silent loss of the
        // original contents — so refuse instead.
        if (!r.open(existing_path, password)) return false;
        if (r.is_locked()) return false;
        if (r.is_header_encrypted()) {
            if (password.empty()) return false;
            new_crypt = r.header_crypt();
            if (!hcw.init_existing(password, new_crypt)) return false;
            header_encrypt_mode = true;
        } else if (encrypt_headers) {
            return false;
        }
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
            if (e.header.is_service && e.header.service_type == "CMT" && comment && !comment->empty()) continue;
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
    } else {
        if (encrypt_headers) {
            if (password.empty()) return false;
            if (!hcw.init_new(password, new_crypt)) return false;
            header_encrypt_mode = true;
        }
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

    // Append new file at end.
    all_payloads.push_back(std::move(new_pe));

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
        if (header_encrypt_mode) {
            if (!format::HeaderWriter::write_crypt_block(cur, new_crypt)) return false;
        }
        format::MainBlock mb;
        mb.arc_flags = format::MHFL_VOLUME;
        if (vol_solid) mb.arc_flags |= format::MHFL_SOLID;
        if (lock) mb.arc_flags |= format::MHFL_LOCK;
        if (idx != 0) {
            mb.arc_flags |= format::MHFL_VOLNUMBER;
            mb.vol_number = static_cast<core::uint64>(idx);
        }
        mb.has_locator = false;
        if (!format::HeaderWriter::write_main_block(cur, mb, header_encrypt_mode ? &hcw : nullptr)) return false;

        if (idx == 0 && comment && !comment->empty()) {
            format::FileBlock cmt;
            cmt.is_service = true;
            cmt.service_type = "CMT";
            cmt.file_name = "CMT";
            cmt.unp_size = comment->size();
            cmt.pack_size = static_cast<core::int64>(comment->size());
            cmt.attributes = 0x20;
            cmt.has_crc32 = true;
            crypto::Crc32 cmt_crc;
            cmt_crc.update(comment->data(), comment->size());
            cmt.data_crc32 = cmt_crc.get();
            cmt.method = 0;
            cmt.unp_ver = 0;
            cmt.win_size = 0;
            if (!format::HeaderWriter::write_file_block(cur, cmt, 0, header_encrypt_mode ? &hcw : nullptr)) return false;
            if (cur.write(comment->data(), comment->size()) != comment->size()) return false;
        }

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
                format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
            }
            format::FileBlock slice_fb = pe.fb;
            slice_fb.pack_size = 0;
            // no split flags for empty
            if (!format::HeaderWriter::write_file_block(cur, slice_fb, 0, header_encrypt_mode ? &hcw : nullptr)) return false;
            continue;
        }
        core::uint64 written = 0;
        while (written < total) {
            core::uint64 cur_pos = cur.tell();
            if (vol_size <= cur_pos) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
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
                format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
                cur.close();
                vol_idx++;
                curPath = volume::next_volume_name(curPath, false);
                if (!start_vol(curPath, vol_idx)) return false;
                continue;
            }
            core::uint64 remaining = total - written;
            core::uint64 slice = std::min(remaining, static_cast<core::uint64>(maxSlice));
            if (pe.fb.is_encrypted && written + slice < total) {
                slice = (slice / 16) * 16;
            }
            if (slice == 0) {
                format::EndArcBlock eb;
                eb.end_flags = 0x0001;
                format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
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
            if (!format::HeaderWriter::write_file_block(cur, slice_fb, extra_flags, header_encrypt_mode ? &hcw : nullptr)) {
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
                format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
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
        format::HeaderWriter::write_end_block(cur, eb, header_encrypt_mode ? &hcw : nullptr);
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
                                         bool encrypt_headers, bool solid,
                                         core::uint64 dict_size,
                                         const compress::FilterConfig& filter_cfg) {
    if (vol_size != 0 && vol_size != volume::VOLSIZE_AUTO) {
        return add_file_to_archive_vol(arc_path, src_file, arc_entry_name, method, vol_size,
                                       password, solid, dict_size, filter_cfg, encrypt_headers);
    }
    return add_or_move_file(arc_path, src_file, arc_entry_name, false, method, sfx_stub_path,
                            password, encrypt_headers, dict_size, filter_cfg);
}

bool ArchiveMutator::move_file_to_archive_vol(const std::filesystem::path& arc_path,
                                              const std::filesystem::path& src_file,
                                              const std::string& arc_entry_name, int method,
                                              core::uint64 vol_size, const std::string& password,
                                              bool solid, core::uint64 dict_size,
                                              const compress::FilterConfig& filter_cfg,
                                              bool encrypt_headers,
                                              const std::vector<core::byte>* comment,
                                              bool lock) {
    bool ok = add_file_to_archive_vol(arc_path, src_file, arc_entry_name, method, vol_size,
                                      password, solid, dict_size, filter_cfg,
                                      encrypt_headers, comment, lock);
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
                                          const std::string& password, bool encrypt_headers,
                                          const compress::FilterConfig& filter_cfg) {
    return add_or_move_file(arc_path, src_file, arc_entry_name, true, method, sfx_stub_path,
                            password, encrypt_headers, 0, filter_cfg);
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

bool ArchiveMutator::convert_to_sfx(const std::filesystem::path& arc_path,
                                    const std::filesystem::path& sfx_stub_path,
                                    std::string& err_detail) {
    ArchiveReader reader;
    int status = RAR_OK;
    std::string detail;
    if (!reader.open_ex(arc_path, "", status, detail, {}, false)) {
        err_detail = "cannot open " + io::u8_str(arc_path);
        return false;
    }
    if (reader.sfx_offset() > 0) {
        err_detail = "cannot convert SFX archive: already an SFX module";
        return false;
    }
    if (reader.is_volume()) {
        err_detail = "cannot convert multi-volume archive to SFX";
        return false;
    }
    reader.close();

    std::error_code ec;
    if (!std::filesystem::exists(sfx_stub_path, ec)) {
        err_detail = "cannot open " + io::u8_str(sfx_stub_path);
        return false;
    }
    auto stub_sz = std::filesystem::file_size(sfx_stub_path, ec);
    if (ec || stub_sz > MAX_SFX_SIZE) {
        err_detail = "SFX module too large";
        return false;
    }

    std::filesystem::path target_path = apply_sfx_extension(arc_path);
    std::filesystem::path parent = target_path.parent_path();
    if (parent.empty()) parent = ".";
    std::filesystem::path tmp_path =
        parent / (target_path.filename().string() + ".sfx_tmp." +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateAlways)) {
        err_detail = "cannot create temporary file " + io::u8_str(tmp_path);
        return false;
    }
    TempFileCleanupGuard tmp_guard{&out, tmp_path};

    io::FileStream stub;
    if (!stub.open(sfx_stub_path, io::FileMode::ReadOnly)) {
        err_detail = "cannot read SFX module " + io::u8_str(sfx_stub_path);
        return false;
    }
    io::FileStream arc;
    if (!arc.open(arc_path, io::FileMode::ReadOnly)) {
        err_detail = "cannot read archive " + io::u8_str(arc_path);
        return false;
    }

    std::vector<core::byte> buf(64 * 1024);
    core::uint64 rem = stub.size();
    while (rem > 0) {
        size_t take = static_cast<size_t>(std::min<core::uint64>(rem, buf.size()));
        if (stub.read(buf.data(), take) != take || out.write(buf.data(), take) != take) {
            err_detail = "failed writing SFX stub";
            return false;
        }
        rem -= take;
    }
    rem = arc.size();
    while (rem > 0) {
        size_t take = static_cast<size_t>(std::min<core::uint64>(rem, buf.size()));
        if (arc.read(buf.data(), take) != take || out.write(buf.data(), take) != take) {
            err_detail = "failed writing archive payload";
            return false;
        }
        rem -= take;
    }
    out.close();

    if (!atomic_replace(tmp_path, target_path)) {
        err_detail = "atomic replace failed";
        return false;
    }
    tmp_guard.commit();

    std::error_code ec_c1, ec_c2;
    auto c1 = std::filesystem::weakly_canonical(target_path, ec_c1);
    auto c2 = std::filesystem::weakly_canonical(arc_path, ec_c2);
    if ((!ec_c1 && !ec_c2 && c1 != c2) || (target_path != arc_path)) {
        std::filesystem::remove(arc_path, ec);
    }

    return true;
}

} // namespace openrar::archive
