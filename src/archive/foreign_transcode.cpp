#include "foreign_transcode.hpp"

#include "archive_mutator.hpp"
#include "../compress/inflate.hpp"
#include "../crypto/rng.hpp"
#include "../crypto/blake2sp.hpp"
#include "../crypto/crc32.hpp"
#include "../io/file_stream.hpp"
#include "../recovery/recovery_writer.hpp"
#include "collision_detector.hpp"
#include "foreign_gzip.hpp"
#include "foreign_tar.hpp"
#include "foreign_zip.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <set>

namespace openrar::archive::foreign {

namespace {

// ── staging area (plan D10) ────────────────────────────────────────────────
// Engine-named `.openrar-cv-*` files under the OS temp dir; RAII sweep on
// every exit. Crash orphans are engine-shaped names in the temp dir; a
// next-run sweep of EXACT-shape records only — never pattern-matched user
// paths (the §3.3 forged-journal lesson).

std::filesystem::path make_staging_dir() {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        core::byte rnd[8];
        crypto::secure_random_bytes(rnd, sizeof(rnd));
        char name[64];
        std::snprintf(name, sizeof(name), "openrar-cv-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0],
                      rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
        const std::filesystem::path dir = base / name;
        std::error_code ec;
        if (std::filesystem::create_directory(dir, ec)) return dir;
    }
    return {};
}

struct StagingGuard {
    std::filesystem::path dir;
    ~StagingGuard() {
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
};

// Portable C++17 mtime set: both clocks sampled once; the delta transfers
// the wall-clock target onto the fs clock (sub-second precision where the
// FS supports it).
bool set_staging_mtime(const std::filesystem::path& p, int64_t epoch_sec, int32_t nsec) {
    const auto now_fs = std::filesystem::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now().time_since_epoch();
    const auto target = std::chrono::seconds(epoch_sec) + std::chrono::nanoseconds(nsec);
    const auto delta = target - now_sys;
    std::error_code ec;
    std::filesystem::last_write_time(
        p, now_fs + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(delta),
        ec);
    return !ec;
}

// Percent-encode invalid UTF-8 (comment migration; the name pipeline's rule
// applied to comment bytes so the CMT service always carries valid UTF-8).
std::string sanitize_comment_utf8(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        size_t len = 0;
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0 && i + 1 < raw.size() &&
                 (static_cast<unsigned char>(raw[i + 1]) & 0xC0) == 0x80)
            len = 2;
        else if ((c & 0xF0) == 0xE0 && i + 2 < raw.size() &&
                 (static_cast<unsigned char>(raw[i + 1]) & 0xC0) == 0x80 &&
                 (static_cast<unsigned char>(raw[i + 2]) & 0xC0) == 0x80)
            len = 3;
        else if ((c & 0xF8) == 0xF0 && i + 3 < raw.size() &&
                 (static_cast<unsigned char>(raw[i + 1]) & 0xC0) == 0x80 &&
                 (static_cast<unsigned char>(raw[i + 2]) & 0xC0) == 0x80 &&
                 (static_cast<unsigned char>(raw[i + 3]) & 0xC0) == 0x80)
            len = 4;
        if (len == 0) {
            static const char* kHex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
            ++i;
        } else {
            out.append(raw, i, len);
            i += len;
        }
    }
    return out;
}

std::string entry_status_for(ForeignStatus s) {
    switch (s) {
    case ForeignStatus::Ok:
        return "extracted";
    case ForeignStatus::EncryptedRefused:
        return "skipped";
    case ForeignStatus::UnsupportedMethod:
        return "skipped";
    case ForeignStatus::SparseRefused:
        return "skipped";
    case ForeignStatus::NameEmpty:
        return "skipped";
    default:
        return "failed";
    }
}

std::string entry_reason_for(ForeignStatus s, const std::string& method_name) {
    switch (s) {
    case ForeignStatus::EncryptedRefused:
        return method_name == "encrypted" ? "encrypted entry refused by policy"
                                          : "encrypted entry refused";
    case ForeignStatus::UnsupportedMethod:
        return "unsupported method: " + method_name;
    case ForeignStatus::SparseRefused:
        return "sparse member refused (documented subset)";
    case ForeignStatus::NameEmpty:
        return "name sanitizes to empty";
    case ForeignStatus::Truncated:
        return "source member truncated";
    case ForeignStatus::CrcMismatch:
        return "source payload checksum mismatch";
    case ForeignStatus::LimitExceeded:
        return "resource limit exceeded";
    case ForeignStatus::Aborted:
        return "aborted";
    case ForeignStatus::IoError:
        return "staging io error";
    default:
        return "";
    }
}

} // namespace

ForeignStatus transcode(const std::filesystem::path& source, const std::filesystem::path& dest,
                        const TranscodeOptions& options, TranscodeResult& result,
                        ExtractionReport& report, ReaderHooks hooks) {
    StagingGuard staging_guard;
    result.status = ForeignStatus::NotOpened;

    // 1. Dispatch (D8). RAR5 / legacy RAR refusals are explicit.
    const SourceFormat fmt = detect_foreign_format(source);
    switch (fmt) {
    case SourceFormat::Rar5:
        result.detail = "source is already a RAR 5.0 archive (use a/u/f to re-pack)";
        result.usage_refused = true;
        return ForeignStatus::Unparseable; // usage class, mapped by the CLI
    case SourceFormat::LegacyRar:
        result.detail = "legacy RAR (1.5-4.0) is not supported by cv";
        result.usage_refused = true;
        return ForeignStatus::Unparseable; // explicit refusal, never "not an archive"
    case SourceFormat::None:
        result.detail = "unrecognized archive format";
        return ForeignStatus::Unparseable;
    case SourceFormat::Zip:
    case SourceFormat::Tar:
    case SourceFormat::Gzip:
        break;
    }

    // 2. Reader open (full structural pass; ZIP runs the CD-vs-LFH gate).
    std::unique_ptr<ForeignReader> reader;
    if (fmt == SourceFormat::Zip) {
        reader = std::make_unique<ZipReader>();
        result.format = "zip";
    } else if (fmt == SourceFormat::Tar) {
        reader = std::make_unique<TarReader>();
        result.format = "tar";
    } else {
        reader = std::make_unique<GzipReader>();
        result.format = "gzip";
    }
    std::string detail;
    result.status = reader->open(source, options.limits, hooks, detail);
    result.detail = detail;
    if (result.status != ForeignStatus::Ok) return result.status;

    // 3. Collision pre-check over the TRANSLATED names (§3.2: cv output
    // must not contain an archive our own extractor aborts on).
    {
        std::vector<CollisionEntry> coll_entries;
        coll_entries.reserve(reader->entry_count());
        for (size_t i = 0; i < reader->entry_count(); ++i) {
            const ForeignEntry& e = reader->entry(i);
            if (e.name.empty()) continue; // NameEmpty skips, never emitted
            coll_entries.push_back({e.name, e.type == ForeignType::Dir});
        }
        std::vector<CollisionPair> pairs;
        if (CollisionDetector::detect(coll_entries, pairs)) {
            result.detail = "archive-internal collision: " + pairs.front().first + " vs " +
                            pairs.front().second + " (" + pairs.front().cls + ")";
            result.status = ForeignStatus::StructuralMismatch;
            return result.status;
        }
    }

    // 4. Staging + per-entry migration.
    const std::filesystem::path staging = make_staging_dir();
    if (staging.empty()) {
        result.detail = "cannot create staging directory";
        return ForeignStatus::IoError;
    }
    staging_guard.dir = staging;

    report.archive = source.string();
    std::vector<ArchiveMutator::PreparedAdd> batch;
    std::vector<std::array<core::byte, 32>> source_hashes; // per migrated file
    std::vector<const ForeignEntry*> migrated_entries;

    auto add_report_entry = [&](const ForeignEntry& e, ForeignStatus s,
                                const std::vector<std::string>& flags) {
        ExtractionReportEntry r;
        r.name = e.name;
        r.status = entry_status_for(s);
        r.reason = entry_reason_for(s, e.method_name);
        r.security_flags = flags;
        if (e.name_escaped) r.security_flags.push_back("name_escaped");
        if (e.timestamp_clamped) r.security_flags.push_back("timestamp_clamped");
        if (!e.comment.empty()) r.security_flags.push_back("comment_skipped");
        report.entries.push_back(std::move(r));
    };

    core::uint64 next_id = 0;
    for (size_t i = 0; i < reader->entry_count(); ++i) {
        if (hooks.cancelled()) {
            result.status = ForeignStatus::Aborted;
            result.detail = "cancelled";
            return result.status;
        }
        const ForeignEntry& e = reader->entry(i);
        hooks.emit(i + 1, reader->entry_count());

        // Name-empty: skipped (never emitted nameless).
        if (e.name.empty()) {
            add_report_entry(e, ForeignStatus::NameEmpty, {});
            ++result.skipped;
            continue;
        }
        // Links: Phase 1 skips with report (the staging pipeline cannot
        // express REDIR records without touching the mutator — plan note).
        if (e.type == ForeignType::Symlink || e.type == ForeignType::Hardlink) {
            add_report_entry(e, ForeignStatus::UnsupportedMethod, {"link_skipped"});
            result.detail = ""; // not an error
            ++result.skipped;
            continue;
        }
        if (e.type == ForeignType::Other) {
            add_report_entry(e, ForeignStatus::UnsupportedMethod, {"special_skipped"});
            ++result.skipped;
            continue;
        }
        if (e.encrypted) {
            result.encrypted_refused_present = true;
            add_report_entry(e, ForeignStatus::EncryptedRefused, {"encrypted_refused"});
            ++result.skipped;
            continue;
        }
        if (e.method_name.rfind("method-", 0) == 0 && e.type == ForeignType::File) {
            add_report_entry(e, ForeignStatus::UnsupportedMethod, {"method_refused"});
            ++result.skipped;
            continue;
        }

        // Directory record: staging dir + prepare_add_dir.
        if (e.type == ForeignType::Dir) {
            const std::filesystem::path sub = staging / ("d" + std::to_string(next_id++));
            std::error_code ec;
            if (!std::filesystem::create_directory(sub, ec)) {
                add_report_entry(e, ForeignStatus::IoError, {});
                ++result.failed;
                result.status = ForeignStatus::IoError;
                result.detail = "cannot create staging directory entry";
                return result.status;
            }
            set_staging_mtime(sub, e.mtime_sec, e.mtime_nsec);
            ArchiveMutator::PreparedAdd pa;
            if (!ArchiveMutator::prepare_add_dir(sub, e.name, pa)) {
                add_report_entry(e, ForeignStatus::IoError, {});
                ++result.failed;
                result.status = ForeignStatus::IoError;
                result.detail = "prepare_add_dir failed";
                return result.status;
            }
            batch.push_back(std::move(pa));
            ++result.migrated_dirs;
            add_report_entry(e, ForeignStatus::Ok, {});
            continue;
        }

        // File: decode to staging (hash on the fly), then prepare + cleanup.
        const std::filesystem::path staging_file = staging / ("f" + std::to_string(next_id++));
        crypto::Blake2sp hash;
        {
            io::FileStream out_file;
            if (!out_file.open(staging_file, io::FileMode::CreateNew)) {
                add_report_entry(e, ForeignStatus::IoError, {});
                ++result.failed;
                result.status = ForeignStatus::IoError;
                result.detail = "cannot create staging file";
                return result.status;
            }
            auto sink = [&](const core::byte* data, size_t size) -> bool {
                if (out_file.write(data, size) != size) return false;
                hash.update(data, size);
                return true;
            };
            LimitState state;
            const ForeignStatus ds = reader->decode(i, sink, options.limits, state, hooks);
            if (ds != ForeignStatus::Ok) {
                out_file.close();
                std::error_code ec;
                std::filesystem::remove(staging_file, ec);
                add_report_entry(e, ds, {});
                if (ds == ForeignStatus::EncryptedRefused ||
                    ds == ForeignStatus::UnsupportedMethod || ds == ForeignStatus::SparseRefused ||
                    ds == ForeignStatus::NameEmpty) {
                    if (ds == ForeignStatus::EncryptedRefused)
                        result.encrypted_refused_present = true;
                    ++result.skipped;
                    continue;
                }
                ++result.failed;
                result.status = ds;
                result.detail = entry_reason_for(ds, e.method_name);
                return result.status;
            }
        }
        std::array<core::byte, 32> digest{};
        hash.finish(digest.data());
        set_staging_mtime(staging_file, e.mtime_sec, e.mtime_nsec);

        ArchiveMutator::PreparedAdd pa;
        if (!ArchiveMutator::prepare_add_file(staging_file, e.name, options.method, "", pa)) {
            std::error_code ec;
            std::filesystem::remove(staging_file, ec);
            add_report_entry(e, ForeignStatus::IoError, {});
            ++result.failed;
            result.status = ForeignStatus::IoError;
            result.detail = "prepare_add_file failed";
            return result.status;
        }
        std::error_code rm_ec;
        std::filesystem::remove(staging_file, rm_ec);
        batch.push_back(std::move(pa));
        source_hashes.push_back(digest);
        migrated_entries.push_back(&e);
        ++result.migrated_files;
        add_report_entry(e, ForeignStatus::Ok, {});
    }

    if (batch.empty()) {
        // Nothing migrated: no output archive is written (exit 10 class).
        result.status = ForeignStatus::Ok;
        result.detail = "no entries migrated";
        return ForeignStatus::Ok;
    }

    // 5. Emit through the SHIPPED writer (zero new emission code).
    std::vector<core::byte> comment;
    bool have_comment = false;
    bool comment_neutralized = false;
    if (!options.comment_override.empty()) {
        comment.assign(options.comment_override.begin(), options.comment_override.end());
        have_comment = true;
    } else if (fmt == SourceFormat::Zip) {
        // The ZIP reader exposes the archive comment via the entry model's
        // extension below; the ZipReader keeps it in archive_comment_.
        // (Retrieved through a small accessor — see foreign_zip.hpp.)
        // Here: migrate it UTF-8-sanitized.
        std::string raw = static_cast<ZipReader*>(reader.get())->archive_comment();
        if (!raw.empty()) {
            const std::string clean = sanitize_comment_utf8(raw);
            comment_neutralized = clean != raw;
            comment.assign(clean.begin(), clean.end());
            have_comment = true;
        }
    }
    result.archive_comment_migrated = have_comment;
    result.archive_comment_skipped = comment_neutralized;

    if (!ArchiveMutator::write_batch_add(dest, batch, {}, options.password, options.encrypt_headers,
                                         {}, options.solid, comment,
                                         /*want_qo=*/false, /*want_ams=*/false, {},
                                         /*max_versions=*/-1)) {
        result.status = ForeignStatus::IoError;
        result.detail = "write_batch_add failed";
        return result.status;
    }

    // -rr: the shipped post-add RecoveryWriter pass (the same machinery the
    // `a` command uses); verify runs AFTER it so -df deletes final bytes.
    if (options.want_rr && options.rr_percent > 0) {
        if (!recovery::RecoveryWriter::add_recovery_record(
                dest, static_cast<core::uint32>(options.rr_percent), options.threads)) {
            result.status = ForeignStatus::IoError;
            result.detail = "recovery record pass failed";
            return result.status;
        }
    }

    // 6. UNCONDITIONAL roundtrip verify (plan D9): fresh reader on the
    // emitted archive; per-entry BLAKE2sp equality + count parity.
    {
        ArchiveReader verify_reader;
        if (!verify_reader.open(dest)) {
            result.status = ForeignStatus::IoError;
            result.detail = "cannot re-open emitted archive";
            return result.status;
        }
        std::vector<size_t> payload_indices;
        for (size_t ei = 0; ei < verify_reader.entries().size(); ++ei) {
            const ArchiveEntry& ae = verify_reader.entries()[ei];
            if (ae.header.is_service) continue;
            if (ae.header.pack_size < 0) continue; // directory record
            payload_indices.push_back(ei);
        }
        if (payload_indices.size() != source_hashes.size()) {
            result.status = ForeignStatus::CrcMismatch;
            result.detail = "verify: entry-count mismatch";
            return result.status;
        }
        for (size_t i = 0; i < payload_indices.size(); ++i) {
            if (hooks.cancelled()) return ForeignStatus::Aborted;
            std::vector<core::byte> payload;
            if (verify_reader.extract_entry_to_memory(payload_indices[i], payload, ~core::uint64(0),
                                                      ReaderHooks{}) != 0) {
                result.status = ForeignStatus::CrcMismatch;
                result.detail = "verify: cannot extract " +
                                verify_reader.entries()[payload_indices[i]].header.file_name;
                return result.status;
            }
            crypto::Blake2sp vh;
            vh.update(payload.data(), payload.size());
            std::array<core::byte, 32> vd{};
            vh.finish(vd.data());
            if (vd != source_hashes[i]) {
                result.status = ForeignStatus::CrcMismatch;
                result.detail = "verify: payload mismatch for " + migrated_entries[i]->name;
                return result.status;
            }
        }
        result.verified = true;
    }

    // 7. -df: delete the source ONLY on a 100% green verify with zero
    // skips/failures (any skip means the migration is incomplete). The
    // reader is released first — on Windows an open handle blocks the
    // remove, and the migration is complete by definition at this point.
    reader.reset();
    if (options.delete_source && result.verified && result.skipped == 0 && result.failed == 0) {
        std::error_code ec;
        std::filesystem::remove(source, ec);
        if (!ec) result.source_deleted = true;
    }

    result.status = ForeignStatus::Ok;
    return ForeignStatus::Ok;
}

} // namespace openrar::archive::foreign
