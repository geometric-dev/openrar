#include "archive_reader.hpp"
#include "rar_errors.hpp"
#include "volume.hpp"
#include "../format/header_reader.hpp"
#include "../io/path_util.hpp"
#include "../core/vint.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/blake2sp.hpp"
#include "../crypto/aes256.hpp"
#include "../crypto/pbkdf2.hpp"
#include "../compress/decompressor50.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace openrar::archive {

// Defined out-of-line so the unique_ptr<Decompressor50> member never needs
// the complete type in translation units that only see the header.
ArchiveReader::ArchiveReader() = default;

ArchiveReader::~ArchiveReader() {
    close();
}

namespace {
// Best-effort key-material hygiene (docs/invariants.md §6): volatile stores
// the optimizer cannot elide. std::string may have reallocated during
// assignment, so this covers the current buffer only — documented as
// best-effort.
void secure_zero(void* p, size_t n) {
    volatile core::byte* v = static_cast<volatile core::byte*>(p);
    while (n--) *v++ = 0;
}

// Filename pattern check for RAR5 new-numbering sets: ".partNN." with NN
// parsing to a volume number > 1 (i.e. NOT the first volume). Legacy
// numbering (.rar/.r00) is deliberately not matched — its first-member
// derivation is ambiguous, so it keeps the reader's historical
// open-exactly-what-was-passed behavior.
bool parse_part_number(const std::filesystem::path& p, long& num) {
    std::string f = p.filename().string();
    for (auto& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t pos = f.find(".part");
    if (pos == std::string::npos) return false;
    const size_t d = pos + 5;
    size_t e = d;
    while (e < f.size() && std::isdigit(static_cast<unsigned char>(f[e]))) ++e;
    if (e == d || e >= f.size() || f[e] != '.') return false;
    num = std::atol(f.substr(d, e - d).c_str());
    return true;
}

constexpr size_t kStreamChunk = 64 * 1024; // stored/decrypt slice (multiple of 16)

// RAII cleaner for failed/broken file extractions (B9).
// Reverse-declaration order ensures FileStream closes the OS file handle before
// FileUnlinker calls std::filesystem::remove, avoiding Windows sharing violations.
// Armed only after successful open() to guarantee pre-existing files are never deleted on open failure.
struct FileUnlinker {
    const std::filesystem::path& path;
    bool armed = false;
    bool dismissed = false;

    explicit FileUnlinker(const std::filesystem::path& p, bool keep_broken = false)
        : path(p), dismissed(keep_broken) {}

    ~FileUnlinker() {
        if (armed && !dismissed) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};
} // namespace

size_t ArchiveReader::test_get_solid_window_size() const {
    if (solid_unpacker_) return solid_unpacker_->win_size();
    return 0;
}

void ArchiveReader::close() {
    stream_.close();
    entries_.clear();
    sfx_offset_ = 0;
    bad_password_ = false;
    saw_crypt_header_ = false;
    crypt_unsupported_ = false;
    header_encrypted_ = false;
    header_crypt_ = format::CryptBlock{};
    links_created_.clear();
    solid_unpacker_.reset();
    solid_chain_ok_ = false;
    last_decoded_index_ = -1;
    missing_volume_path_.clear();
    if (!password_.empty()) {
        secure_zero(password_.data(), password_.size());
        password_.clear();
    }
}

bool ArchiveReader::is_open() const {
    return stream_.is_open();
}

bool ArchiveReader::is_locked() const {
    return (main_block_.arc_flags & format::MHFL_LOCK) != 0;
}

bool ArchiveReader::is_volume() const {
    return (main_block_.arc_flags & format::MHFL_VOLUME) != 0;
}

bool ArchiveReader::is_solid() const {
    return (main_block_.arc_flags & format::MHFL_SOLID) != 0;
}

bool ArchiveReader::has_recovery_record() const {
    if ((main_block_.arc_flags & format::MHFL_PROTECT) != 0) return true;
    for (const auto& entry : entries_) {
        if (entry.header.is_service && entry.header.service_type == "RR") return true;
    }
    return false;
}

bool ArchiveReader::open(const std::filesystem::path& arc_path, const std::string& password) {
    int status = RAR_OK;
    std::string detail;
    return open_ex(arc_path, password, status, detail, ReaderHooks{}, /*strict_volumes=*/false);
}

bool ArchiveReader::open_ex(const std::filesystem::path& arc_path, const std::string& password,
                            int& status_out, std::string& detail_out, const ReaderHooks& hooks,
                            bool strict_volumes) {
    close();
    status_out = RAR_OK;
    detail_out.clear();
    path_ = arc_path;
    password_ = password;

    // Multi-volume rewind: a middle volume path (.partNN.rar, NN > 1) opens
    // the derived first volume instead. The rewind only fires when the
    // derived name exists, except that a missing first volume of a genuine
    // .partNN set is a hard MISSING_VOLUME error (the host asked to open a
    // set it can't locate the head of).
    long part_num = 0;
    if (parse_part_number(arc_path, part_num) && part_num > 1) {
        std::filesystem::path first = volume::vol_name_to_first_name(arc_path, false);
        std::error_code ec;
        if (std::filesystem::exists(first, ec)) {
            path_ = first;
        } else {
            status_out = RAR_ERR_MISSING_VOLUME;
            detail_out = "cannot open first volume: " + io::u8_str(first);
            missing_volume_path_ = first;
            close();
            missing_volume_path_ = first; // survive close()'s state reset
            return false;
        }
    }

    if (!stream_.open(path_, io::FileMode::ReadOnly)) {
        status_out = RAR_ERR_IO;
        detail_out = "cannot open " + io::u8_str(path_);
        close();
        return false;
    }

    if (!scan_archive(hooks, strict_volumes, status_out, detail_out)) {
        // Preserve the wrong-password verdict for the caller: close()
        // resets all per-archive state.
        bool bad = bad_password_;
        std::filesystem::path missing = missing_volume_path_;
        close();
        bad_password_ = bad;
        missing_volume_path_ = missing;
        return false;
    }

    return true;
}

std::filesystem::path ArchiveReader::derive_next_volume_name(const std::filesystem::path& cur,
                                                             bool old_numbering) {
    return volume::next_volume_name(cur, old_numbering);
}

std::filesystem::path ArchiveReader::derive_first_volume_name(const std::filesystem::path& cur,
                                                              bool old_numbering) {
    return volume::vol_name_to_first_name(cur, old_numbering);
}

bool ArchiveReader::scan_archive(const ReaderHooks& hooks, bool strict_volumes, int& status_out,
                                 std::string& detail_out) {
    auto fail = [&](int status, std::string detail) {
        status_out = status;
        detail_out = std::move(detail);
        return false;
    };
    // Preserve original stream for single-volume compat, but multivolume scan uses per-volume streams
    core::uint64 file_size = stream_.size();
    if (file_size < format::RAR5_SIGNATURE_SIZE) {
        return fail(RAR_ERR_NOT_RAR, "not a RAR5 archive");
    }

    // Check offset 0 first on first volume
    core::byte sig_buf[8];
    if (stream_.read(sig_buf, 8) != 8) return fail(RAR_ERR_IO, "short read");

    if (std::memcmp(sig_buf, format::rar5_signature(), 8) == 0) {
        sfx_offset_ = 0;
    } else {
        // SFX scan: search up to 4MB (0x400000) on first volume
        core::uint64 scan_limit = std::min(file_size, static_cast<core::uint64>(0x400000));
        std::vector<core::byte> scan_buf(65536);
        bool found = false;

        core::uint64 pos = 0;
        while (pos < scan_limit) {
            if (hooks.cancelled()) return fail(RAR_ERR_ABORTED, "open aborted");
            stream_.seek(static_cast<core::int64>(pos), io::SeekOrigin::Begin);
            size_t bytes_to_read = static_cast<size_t>(
                std::min(static_cast<core::uint64>(scan_buf.size()), scan_limit - pos));
            size_t read_bytes = stream_.read(scan_buf.data(), bytes_to_read);
            if (read_bytes < 8) break;
            hooks.emit(pos + read_bytes, scan_limit);

            for (size_t i = 0; i <= read_bytes - 8; ++i) {
                if (std::memcmp(scan_buf.data() + i, format::rar5_signature(), 8) == 0) {
                    core::uint64 cand_offset = pos + i;
                    stream_.seek(static_cast<core::int64>(cand_offset + 8), io::SeekOrigin::Begin);
                    core::uint64 type = 0, flags = 0, data_size = 0;
                    std::vector<core::byte> body;
                    if (format::HeaderReader::read_block_raw(
                            stream_, type, flags, body, data_size) == format::HeaderResult::Ok &&
                        (type == format::HEAD_MAIN || type == format::HEAD_CRYPT)) {
                        sfx_offset_ = cand_offset;
                        found = true;
                        break;
                    }
                }
            }
            if (found) break;
            pos += (read_bytes > 8) ? (read_bytes - 7) : 1;
        }

        if (!found) return fail(RAR_ERR_NOT_RAR, "not a RAR5 archive (no signature in SFX scan)");
    }

    // Multipass volume scan: stitch split files across volumes
    std::vector<std::filesystem::path> vol_chain;
    vol_chain.push_back(path_);
    // Keep a set of visited to avoid loops
    std::vector<ArchiveEntry> merged;
    format::MainBlock first_main;
    bool first_main_read = false;
    size_t vol_idx = 0;
    bool saw_end = false;
    std::filesystem::path current_vol_path = path_;

    // Cumulative progress bases for the volume walk: done counts bytes
    // consumed across volumes already processed, total counts bytes of
    // volumes opened so far (both monotonic; the walk grows the set).
    core::uint64 vol_done_base = 0;
    core::uint64 vol_total_base = 0;

    // Helper to process a single volume file
    auto process_volume = [&](const std::filesystem::path& vpath, bool is_first, io::FileStream& vs,
                              std::vector<ArchiveEntry>& raw_out, format::MainBlock& vol_main,
                              bool& vol_saw_end) -> bool {
        core::uint64 vsz = vs.size();
        vol_total_base = vol_done_base + vsz;
        core::uint64 start_off = 0;
        if (is_first) {
            start_off = sfx_offset_ + 8;
        } else {
            // Subsequent volumes have no SFX, check signature at 0
            core::byte sig[8];
            vs.seek(0, io::SeekOrigin::Begin);
            if (vs.read(sig, 8) != 8 || std::memcmp(sig, format::rar5_signature(), 8) != 0)
                return false;
            start_off = 8;
        }
        vs.seek(static_cast<core::int64>(start_off), io::SeekOrigin::Begin);
        // Header decryption (-hp): HEAD_CRYPT is stored in clear right after
        // the signature and activates this state; every following header of
        // the volume is then read through it.
        format::HeaderCryptReader hcrypt;
        while (vs.tell() < vsz) {
            if (hooks.cancelled()) return false;
            hooks.emit(vol_done_base + (vs.tell() - start_off), vol_total_base);
            core::uint64 head_start = vs.tell();
            core::uint64 type = 0, flags = 0, data_size = 0;
            std::vector<core::byte> body;
            auto res =
                format::HeaderReader::read_block_raw(vs, type, flags, body, data_size, &hcrypt);
            if (res != format::HeaderResult::Ok) {
                if (res == format::HeaderResult::HeaderCrcMismatch) { /* propagate */
                }
                if (hcrypt.bad_password) bad_password_ = true;
                break;
            }
            core::uint64 head_end = vs.tell();
            if (type == format::HEAD_CRYPT) {
                format::CryptBlock cb;
                if (!format::HeaderReader::parse_crypt_header(body.data(), body.size(), cb)) {
                    break;
                }
                if (hcrypt.active) break; // duplicate HEAD_CRYPT: corrupt
                saw_crypt_header_ = true;
                if (!hcrypt.init(password_, cb)) {
                    if (hcrypt.bad_password)
                        bad_password_ = true;
                    else
                        crypt_unsupported_ = true; // unknown crypto version
                    break;
                }
                header_encrypted_ = true;
                header_crypt_ = cb;
                continue;
            }
            if (type == format::HEAD_MAIN) {
                // Propagate failure: a CRC-valid but structurally malformed
                // main header must not be admitted as a default-constructed
                // block (empty archive name, lost volume flags).
                if (!format::HeaderReader::parse_main_header(body.data(), body.size(), vol_main))
                    break;
                if (!first_main_read) {
                    first_main = vol_main;
                    first_main_read = true;
                }
            } else if (type == format::HEAD_FILE || type == format::HEAD_SERVICE) {
                ArchiveEntry e;
                e.header_offset = head_start;
                e.header_size = head_end - head_start;
                e.data_offset = head_end;
                if (!format::HeaderReader::parse_file_header(body.data(), body.size(), e.header))
                    break;
                e.split_before = (flags & format::HFL_SPLITBEFORE) != 0;
                e.split_after = (flags & format::HFL_SPLITAFTER) != 0;
                // data_size is attacker-controlled and can be up to 2^64-1.
                // Admit only the bytes actually present in this volume: an
                // extent claiming past-EOF bytes would later drive an
                // attacker-sized allocation in read_packed_data (uncaught
                // bad_alloc -> terminate). A data area extending past the
                // volume means a truncated volume, so stop scanning rather
                // than seeking to a wrapped position (the cast to int64
                // would wrap negative).
                core::uint64 avail = data_size > 0 && vsz > vs.tell() ? vsz - vs.tell() : 0;
                core::uint64 admit = data_size < avail ? data_size : avail;
                e.data_size = admit;
                e.extents.push_back({vpath, e.data_offset, admit});
                raw_out.push_back(std::move(e));
                if (data_size > avail) break;
                if (admit > 0) {
                    vs.seek(static_cast<core::int64>(admit), io::SeekOrigin::Current);
                }
            } else if (type == format::HEAD_ENDARC) {
                format::EndArcBlock eb;
                if (!format::HeaderReader::parse_end_header(body.data(), body.size(), eb)) break;
                if ((eb.end_flags & 0x0001) == 0) vol_saw_end = true;
                // No data area for ENDARC
                if (vol_saw_end) break;
            } else {
                if (data_size > 0) {
                    core::uint64 avail = vsz > vs.tell() ? vsz - vs.tell() : 0;
                    if (data_size > avail) break;
                    vs.seek(static_cast<core::int64>(data_size), io::SeekOrigin::Current);
                }
            }
        }
        return true;
    };

    // Scan volumes sequentially, merging split files. Cap the walk at the
    // RAR5 spec ceiling (65535 combined data+recovery volumes) so a corrupt
    // or self-referencing chain can't loop indefinitely, while still
    // allowing legitimate archives to span thousands of parts.
    constexpr size_t MAX_VOLUME_CHAIN = 65535;
    std::vector<ArchiveEntry> raw_all;
    bool scan_aborted = false;
    bool missing_required = false;
    while (vol_idx < vol_chain.size() && vol_idx < MAX_VOLUME_CHAIN) {
        if (hooks.cancelled()) {
            scan_aborted = true;
            break;
        }
        std::filesystem::path vpath = vol_chain[vol_idx];
        io::FileStream vs;
        if (!vs.open(vpath, io::FileMode::ReadOnly)) {
            // try old numbering fallback once
            if (vol_idx > 0) {
                auto alt = derive_next_volume_name(vol_chain[vol_idx - 1], true);
                std::error_code alt_ec;
                if (alt != vpath && std::filesystem::exists(alt, alt_ec) &&
                    vs.open(alt, io::FileMode::ReadOnly)) {
                    vpath = alt;
                    vol_chain[vol_idx] = vpath;
                } else {
                    // The chain (split flags / probing) required this volume.
                    missing_required = true;
                    missing_volume_path_ = vpath;
                    break;
                }
            } else
                break;
        }
        bool vol_saw_end = false;
        format::MainBlock vol_main;
        std::vector<ArchiveEntry> vol_raw;
        if (!process_volume(vpath, vol_idx == 0, vs, vol_raw, vol_main, vol_saw_end)) {
            if (hooks.cancelled()) scan_aborted = true;
            break;
        }
        vol_done_base = vol_total_base;
        // Append vol_raw to raw_all with split merging across boundary
        for (auto& re : vol_raw) {
            if (re.header.is_service && re.header.service_type == "QO") {
                // QO is not needed for multivolume; spec says locator 0 but it may appear in the first volume
                // Keep only first QO to avoid blocking merges
                bool already_have_qo = false;
                for (auto& e : raw_all)
                    if (e.header.is_service && e.header.service_type == "QO")
                        already_have_qo = true;
                if (!already_have_qo) raw_all.push_back(std::move(re));
                continue;
            }
            // Find previous file entry with same name that has split_after (skip QO/service interleaving)
            ArchiveEntry* merge_target = nullptr;
            for (auto it = raw_all.rbegin(); it != raw_all.rend(); ++it) {
                if (it->header.is_service) continue;
                if (it->header.file_name == re.header.file_name && it->split_after) {
                    merge_target = &(*it);
                    break;
                }
                // If we encounter a non-matching file before find, stop search
                if (!it->header.is_service) break;
            }
            bool can_merge = re.split_before && merge_target != nullptr;
            if (can_merge) {
                auto& prev = *merge_target;
                prev.extents.insert(prev.extents.end(), re.extents.begin(), re.extents.end());
                prev.data_size += re.data_size;
                prev.split_after = re.split_after;
                prev.header.has_crc32 = re.header.has_crc32;
                prev.header.data_crc32 = re.header.data_crc32;
                prev.header.has_blake2sp = re.header.has_blake2sp;
                if (re.header.has_blake2sp) prev.header.blake2sp = re.header.blake2sp;
            } else {
                raw_all.push_back(std::move(re));
            }
        }
        if (vol_saw_end) {
            saw_end = true;
            break;
        }
        // Need next volume? Check if last raw entry had SPLITAFTER or volume indicates not last
        bool need_next = false;
        if (!vol_raw.empty()) {
            // If any entry in this volume had SPLITAFTER, need next
            for (auto& e : vol_raw)
                if (e.split_after) need_next = true;
        }
        // Also if vol_main indicates volume and end not seen, assume need next if next file exists
        if (!need_next) {
            // Peek if next volume file exists (probing). error_code overloads
            // throughout: an unreadable parent must degrade to "missing" (and
            // eventually MISSING_VOLUME), never throw across the C ABI.
            auto nxt = derive_next_volume_name(vpath, false);
            std::error_code nxt_ec;
            if (std::filesystem::exists(nxt, nxt_ec))
                need_next = true;
            else {
                auto nxt_old = derive_next_volume_name(vpath, true);
                if (std::filesystem::exists(nxt_old, nxt_ec)) need_next = true;
            }
        }
        if (need_next) {
            auto nxt = derive_next_volume_name(vpath, false);
            // Legacy naming ceiling: next_volume_name returns the same name
            // when no next volume exists (e.g. `.z99` is the last legacy
            // name). Mirrors archive_mutator's `if (nxt == p) break;` —
            // without it a self-referential name re-enters the chain below
            // and the same volume is rescanned up to MAX_VOLUME_CHAIN times.
            if (nxt == vpath) break;
            if (vol_chain.size() <= vol_idx + 1) {
                std::error_code nxt_ec;
                if (std::filesystem::exists(nxt, nxt_ec))
                    vol_chain.push_back(nxt);
                else {
                    auto nxt_old = derive_next_volume_name(vpath, true);
                    if (nxt_old != vpath && std::filesystem::exists(nxt_old, nxt_ec))
                        vol_chain.push_back(nxt_old);
                    else {
                        // need_next came from split_after / ENDARC NEXTVOL
                        // flags (the probe path already proved the file
                        // exists), so the set is genuinely incomplete.
                        missing_required = true;
                        missing_volume_path_ = nxt;
                        break;
                    }
                }
            }
        } else {
            break;
        }
        ++vol_idx;
        if (vol_idx >= vol_chain.size()) break;
    }

    if (scan_aborted) return fail(RAR_ERR_ABORTED, "open aborted");
    if (missing_required && strict_volumes)
        return fail(RAR_ERR_MISSING_VOLUME, "missing volume: " + io::u8_str(missing_volume_path_));
    if (!first_main_read) {
        // A HEAD_CRYPT block that could not be passed (no password, wrong
        // password, unknown crypto version) dies before the main header —
        // map those to the dedicated codes instead of NOT_RAR. On -hp
        // archives structural corruption also presents as BAD_PASSWORD
        // (header CRC / PswCheck failure), per the documented contract.
        if (saw_crypt_header_) {
            if (crypt_unsupported_)
                return fail(RAR_ERR_UNSUPPORTED_FEATURE, "unsupported HEAD_CRYPT crypto version");
            if (bad_password_)
                return fail(RAR_ERR_BAD_PASSWORD, "wrong password for encrypted headers");
            if (password_.empty())
                return fail(RAR_ERR_ENCRYPTED,
                            "archive headers are encrypted (password required to list)");
        }
        return fail(RAR_ERR_NOT_RAR, "not a RAR5 archive (no main header)");
    }
    main_block_ = first_main;
    entries_.clear();
    for (auto& e : raw_all) {
        // Ensure legacy fields consistent
        if (!e.extents.empty()) {
            e.data_offset = e.extents.front().offset;
            // data_size already sum
        }
        entries_.push_back(std::move(e));
    }
    // Keep stream_ open to first volume for compat single-volume reads
    stream_.close();
    stream_.open(path_, io::FileMode::ReadOnly);
    stream_.seek(static_cast<core::int64>(sfx_offset_ + 8), io::SeekOrigin::Begin);
    (void)saw_end;
    hooks.emit(vol_done_base, vol_done_base); // exactly one final (total, total)
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming file-handle APIs (v1.3.0)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::filesystem::path> ArchiveReader::volume_paths() const {
    std::vector<std::filesystem::path> out;
    if (!path_.empty()) out.push_back(path_);
    for (const auto& e : entries_) {
        for (const auto& ext : e.extents) {
            bool seen = false;
            for (const auto& p : out) {
                std::error_code ec;
                if (p == ext.volume_path ||
                    (!ec && std::filesystem::equivalent(p, ext.volume_path, ec))) {
                    seen = true;
                    break;
                }
            }
            if (!seen) out.push_back(ext.volume_path);
        }
    }
    return out;
}

size_t ArchiveReader::prev_chain_index(size_t idx) const {
    for (size_t j = idx; j-- > 0;) {
        const auto& h = entries_[j].header;
        if (h.is_service || (h.file_flags & format::FHFL_DIRECTORY) || h.method == 0) continue;
        return j;
    }
    return static_cast<size_t>(-1);
}

size_t ArchiveReader::solid_run_head(size_t idx) const {
    size_t h = idx;
    while (h > 0) {
        const auto& hd = entries_[h].header;
        if (hd.is_service || (hd.file_flags & format::FHFL_DIRECTORY) || hd.method == 0 ||
            hd.is_solid) {
            --h;
            continue;
        }
        break;
    }
    return h;
}

int ArchiveReader::derive_entry_keys(const ArchiveEntry& entry, crypto::Rar5Keys& keys) {
    if (!entry.header.is_encrypted) return RAR_OK;
    if (password_.empty()) return RAR_ERR_ENCRYPTED;
    if (entry.header.lg2_count >= 25) return RAR_ERR_UNSUPPORTED_FEATURE;
    crypto::Pbkdf2Rar5::derive_keys(password_, entry.header.salt.data(), 16,
                                    1U << entry.header.lg2_count, keys);
    if (entry.header.has_psw_check && !crypto::Pbkdf2Rar5::constant_time_equal(
                                          keys.psw_check, entry.header.psw_check.data(), 8)) {
        bad_password_ = true;
        return RAR_ERR_BAD_PASSWORD;
    }
    return RAR_OK;
}

int ArchiveReader::ensure_solid_position(size_t idx, const ReaderHooks& hooks) {
    const ArchiveEntry& entry = entries_[idx];
    const bool chain = is_solid() || entry.header.is_solid;
    if (!chain || entry.header.method == 0) return RAR_OK;

    // Fast path: the chain state reflects this entry's immediate compressed
    // predecessor (docs/invariants.md §1).
    const size_t prev = prev_chain_index(idx);
    if (prev != static_cast<size_t>(-1) && solid_chain_ok_ &&
        last_decoded_index_ == static_cast<long long>(prev)) {
        return RAR_OK;
    }

    // Rewind path: decode the run prefix [head, idx) through a discard sink.
    const size_t head = solid_run_head(idx);
    ReaderHooks silent = hooks; // catch-up cancels but stays progress-silent
    silent.progress = nullptr;
    silent.progress_user = nullptr;
    const uint64_t total = entry.header.unp_size;
    for (size_t i = head; i < idx; ++i) {
        const ArchiveEntry& mid = entries_[i];
        if (mid.header.is_service || (mid.header.file_flags & format::FHFL_DIRECTORY) ||
            mid.header.method == 0)
            continue;
        hooks.emit(0, total); // heartbeat: catch-up running, target not started
        crypto::Rar5Keys mid_keys{};
        crypto::Rar5Keys* mk = nullptr;
        if (mid.header.is_encrypted) {
            int rc = derive_entry_keys(mid, mid_keys);
            if (rc != RAR_OK) return rc;
            mk = &mid_keys;
        }
        SinkStatus st;
        int rc = stream_payload(i, mk, [](const core::byte*, size_t) { return true; }, st, silent);
        if (mk) secure_zero(mid_keys.aes_key, sizeof(mid_keys.aes_key));
        if (rc != RAR_OK) {
            solid_chain_ok_ = false;
            return rc;
        }
        // stream_payload advanced last_decoded_index_ to i on success.
    }
    return RAR_OK;
}

namespace {
// Pull source over an entry's extents feeding the decompressor's src_cb
// contract: fill dest with up to `want` bytes, return the count (0 = end).
// Volumes open on demand per extent and close when exhausted
// (docs/invariants.md §3); decryption happens in 64 KiB slices with the CBC
// IV carried across them (§2), so memory stays bounded regardless of size.
class ExtentPullSource {
public:
    ExtentPullSource(const ArchiveEntry& entry, const std::filesystem::path& fallback_path,
                     const crypto::Rar5Keys* keys)
        : keys_(keys) {
        if (entry.extents.empty())
            exts_.push_back({fallback_path, entry.data_offset, entry.data_size});
        else
            exts_ = entry.extents;
        if (keys_) std::memcpy(iv_, entry.header.init_v.data(), 16);
        staging_.resize(kStreamChunk);
    }

    size_t pull(core::byte* dest, size_t want) {
        size_t got = 0;
        while (got < want) {
            if (plain_pos_ < plain_len_) {
                const size_t n = std::min(want - got, plain_len_ - plain_pos_);
                std::memcpy(dest + got, staging_.data() + plain_pos_, n);
                plain_pos_ += n;
                got += n;
                continue;
            }
            if (eof_ || error_ || !refill()) break;
        }
        return got;
    }

    bool error() const { return error_; }
    bool missing() const { return missing_; }
    const std::filesystem::path& missing_path() const { return missing_path_; }

private:
    bool refill() {
        for (;;) {
            if (ext_idx_ >= exts_.size()) {
                stream_.close();
                eof_ = true;
                return false;
            }
            if (!stream_.is_open()) {
                const auto& ext = exts_[ext_idx_];
                if (!stream_.open(ext.volume_path, io::FileMode::ReadOnly)) {
                    error_ = missing_ = true;
                    missing_path_ = ext.volume_path;
                    return false;
                }
                ext_remain_ = ext.size;
                if (!stream_.seek(static_cast<core::int64>(ext.offset), io::SeekOrigin::Begin)) {
                    error_ = true;
                    return false;
                }
            }
            const size_t take =
                static_cast<size_t>(std::min<core::uint64>(ext_remain_, staging_.size()));
            if (take == 0) {
                stream_.close();
                ++ext_idx_;
                continue;
            }
            if (stream_.read(staging_.data(), take) != take) {
                error_ = true;
                return false;
            }
            size_t plain = take;
            if (keys_) {
                // Encrypted payload length is a multiple of the AES block
                // size and 64 KiB slices keep every boundary aligned, so a
                // short tail cannot occur in a well-formed archive.
                // decrypt_cbc leaves the slice's last ciphertext block in
                // iv_ — exactly the next slice's IV.
                plain = take - (take % 16);
                if (plain != take) {
                    error_ = true;
                    return false;
                }
                crypto::Aes256 aes(keys_->aes_key);
                if (!aes.decrypt_cbc(staging_.data(), plain, iv_)) {
                    error_ = true;
                    return false;
                }
            }
            plain_len_ = plain;
            plain_pos_ = 0;
            ext_remain_ -= take;
            return true;
        }
    }

    std::vector<VolumeExtent> exts_;
    size_t ext_idx_{0};
    io::FileStream stream_;
    core::uint64 ext_remain_{0};
    const crypto::Rar5Keys* keys_;
    core::byte iv_[16]{};
    std::vector<core::byte> staging_;
    size_t plain_pos_{0};
    size_t plain_len_{0};
    bool eof_{false};
    bool error_{false};
    bool missing_{false};
    std::filesystem::path missing_path_;
};
} // namespace

int ArchiveReader::stream_payload(size_t idx, crypto::Rar5Keys* keys,
                                  const std::function<bool(const core::byte*, size_t)>& out_sink,
                                  SinkStatus& status, const ReaderHooks& hooks) {
    const ArchiveEntry& entry = entries_[idx];
    const uint64_t total = entry.header.unp_size;

    // Hash selection mirrors test_entry: a present BLAKE2sp record is
    // authoritative; the header CRC32 beside it is not evaluated.
    // Tweaked-checksum exception (RAR5-FORMAT.md, encryption extra 0x0002):
    // when the writer marked the checksums as key-dependent, the stored
    // value is NOT the plaintext hash and must not be compared here — the
    // PswCheck embedded in the archive already authenticates the decryption
    // key. Skipping keeps valid third-party encrypted archives from failing
    // with a false RAR_ERR_CRC_MISMATCH (they were fail-closed before).
    // Our own writer never sets 0x0002 — it stores the plaintext CRC — so
    // its encrypted output stays fully verified on this path.
    const bool tweaked_checksums =
        entry.header.is_encrypted && (entry.header.crypt_flags & 0x0002) != 0;
    const bool use_blake = entry.header.has_blake2sp && !tweaked_checksums;
    const bool use_crc = !use_blake && entry.header.has_crc32 && !tweaked_checksums;
    crypto::Blake2sp b2;
    crypto::Crc32 crc;
    uint64_t produced = 0;

    auto core_sink = [&](const core::byte* p, size_t n) -> bool {
        if (hooks.cancelled()) {
            status.aborted = true;
            return false;
        }
        if (use_blake) b2.update(p, n);
        if (use_crc) crc.update(p, n);
        if (!out_sink(p, n)) return false;
        produced += n;
        hooks.emit(produced, total);
        return true;
    };

    int rc = RAR_OK;
    if (entry.header.method == 0) {
        // Stored: stream extents in 64 KiB chunks, decrypting in place.
        std::vector<VolumeExtent> exts;
        if (entry.extents.empty())
            exts.push_back({path_, entry.data_offset, entry.data_size});
        else
            exts = entry.extents;
        core::byte iv[16];
        std::unique_ptr<crypto::Aes256> aes;
        if (keys) {
            std::memcpy(iv, entry.header.init_v.data(), 16);
            aes = std::make_unique<crypto::Aes256>(keys->aes_key);
        }
        std::vector<core::byte> buf(kStreamChunk);
        for (const auto& ext : exts) {
            io::FileStream vs;
            if (!vs.open(ext.volume_path, io::FileMode::ReadOnly)) {
                missing_volume_path_ = ext.volume_path;
                return RAR_ERR_MISSING_VOLUME;
            }
            if (!vs.seek(static_cast<core::int64>(ext.offset), io::SeekOrigin::Begin))
                return RAR_ERR_IO;
            core::uint64 remain = ext.size;
            while (remain > 0) {
                const size_t take = static_cast<size_t>(std::min<core::uint64>(remain, buf.size()));
                if (hooks.cancelled()) {
                    status.aborted = true;
                    return RAR_ERR_ABORTED;
                }
                if (vs.read(buf.data(), take) != take) return RAR_ERR_TRUNCATED;
                if (aes) {
                    // Payload length is a multiple of the AES block size and
                    // 64 KiB slices keep every boundary aligned; a short tail
                    // means a corrupt set, not decryptable data.
                    if (take % 16 != 0) return RAR_ERR_TRUNCATED;
                    if (!aes->decrypt_cbc(buf.data(), take, iv)) return RAR_ERR_TRUNCATED;
                }
                // Encrypted payloads are block-padded: the final slice may
                // decrypt past unp_size — emit only the real bytes (mirrors
                // extract_store_entry's write_len cap).
                size_t emit = take;
                if (aes && produced + emit > total) emit = static_cast<size_t>(total - produced);
                if (!core_sink(buf.data(), emit)) {
                    return status.aborted ? RAR_ERR_ABORTED : RAR_ERR_IO;
                }
                remain -= take;
            }
        }
    } else {
        ExtentPullSource src(entry, path_, keys);
        bool ok = decode_compressed(
            entry, [&src](core::byte* buf, size_t want) -> size_t { return src.pull(buf, want); },
            static_cast<size_t>(entry.data_size), core_sink);
        if (!ok) {
            if (status.aborted)
                rc = RAR_ERR_ABORTED;
            else if (status.failed)
                rc = RAR_ERR_IO;
            else if (src.missing()) {
                missing_volume_path_ = src.missing_path();
                rc = RAR_ERR_MISSING_VOLUME;
            } else if (src.error())
                rc = RAR_ERR_IO;
            else
                rc = RAR_ERR_TRUNCATED;
        }
    }
    if (rc != RAR_OK) return rc;
    if (status.aborted) return RAR_ERR_ABORTED;
    if (status.failed) return RAR_ERR_IO;
    if (produced != total) return RAR_ERR_TRUNCATED;

    if (use_blake) {
        core::byte digest[32];
        b2.finish(digest);
        if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) return RAR_ERR_CRC_MISMATCH;
    }
    if (use_crc && crc.get() != entry.header.data_crc32) return RAR_ERR_CRC_MISMATCH;

    // The shared window state now reflects this entry (docs/invariants.md §1).
    if (is_solid() || entry.header.is_solid) last_decoded_index_ = static_cast<long long>(idx);
    return RAR_OK;
}

int ArchiveReader::extract_entry_stream(size_t entry_index, io::FileStream& out,
                                        const ReaderHooks& hooks) {
    bad_password_ = false;
    if (entry_index >= entries_.size()) return RAR_ERR_INVALID_ARG;
    const ArchiveEntry& entry = entries_[entry_index];

    crypto::Rar5Keys keys{};
    int rc = derive_entry_keys(entry, keys);
    if (rc == RAR_OK) rc = ensure_solid_position(entry_index, hooks);
    if (rc != RAR_OK) {
        secure_zero(&keys, sizeof(keys));
        return rc;
    }

    const uint64_t total = entry.header.unp_size;
    if (total > 0) hooks.emit(0, total);

    SinkStatus st;
    rc = stream_payload(
        entry_index, entry.header.is_encrypted ? &keys : nullptr,
        [&](const core::byte* p, size_t n) -> bool { return out.write(p, n) == n; }, st, hooks);
    secure_zero(&keys, sizeof(keys));
    if (rc == RAR_OK && total == 0) hooks.emit(0, 0);
    return rc;
}

int ArchiveReader::extract_entry_to_memory(size_t entry_index, std::vector<core::byte>& out,
                                           uint64_t max_bytes, const ReaderHooks& hooks) {
    bad_password_ = false;
    if (entry_index >= entries_.size()) return RAR_ERR_INVALID_ARG;
    const ArchiveEntry& entry = entries_[entry_index];
    if (entry.header.unp_size > max_bytes) return RAR_ERR_NOMEM;

    crypto::Rar5Keys keys{};
    int rc = derive_entry_keys(entry, keys);
    if (rc == RAR_OK) rc = ensure_solid_position(entry_index, hooks);
    if (rc != RAR_OK) {
        secure_zero(&keys, sizeof(keys));
        return rc;
    }

    const uint64_t total = entry.header.unp_size;
    if (total > 0) hooks.emit(0, total);

    SinkStatus st;
    rc = stream_payload(
        entry_index, entry.header.is_encrypted ? &keys : nullptr,
        [&](const core::byte* p, size_t n) -> bool {
            out.insert(out.end(), p, p + n);
            return true;
        },
        st, hooks);
    secure_zero(&keys, sizeof(keys));
    if (rc == RAR_OK && total == 0) hooks.emit(0, 0);
    return rc;
}

int ArchiveReader::test_entry_stream(size_t entry_index, const ReaderHooks& hooks) {
    bad_password_ = false;
    if (entry_index >= entries_.size()) return RAR_ERR_INVALID_ARG;
    const ArchiveEntry& entry = entries_[entry_index];
    // Directory and link entries carry no verifiable payload (and links must
    // not be followed — the target may not exist yet).
    if ((entry.header.file_flags & format::FHFL_DIRECTORY) || entry.header.redir_type != 0)
        return RAR_OK;

    crypto::Rar5Keys keys{};
    int rc = derive_entry_keys(entry, keys);
    if (rc == RAR_OK) rc = ensure_solid_position(entry_index, hooks);
    if (rc != RAR_OK) {
        secure_zero(&keys, sizeof(keys));
        return rc;
    }

    SinkStatus st;
    rc = stream_payload(
        entry_index, entry.header.is_encrypted ? &keys : nullptr,
        [](const core::byte*, size_t) { return true; }, st, hooks);
    secure_zero(&keys, sizeof(keys));
    if (rc == RAR_OK && entry.header.unp_size == 0) hooks.emit(0, 0);
    return rc;
}


bool ArchiveReader::read_packed_data(const ArchiveEntry& entry,
                                     std::vector<core::byte>& out) const {
    out.clear();
    if (entry.in_memory) {
        out = entry.memory_data;
        return true;
    }
    if (!entry.extents.empty()) {
        // Checked accumulate: extents are clamped to volume sizes at scan
        // time, but the volume files can change underneath us, so re-clamp
        // each read to the bytes the file actually has and refuse sizes that
        // cannot exist on this platform.
        core::uint64 total64 = 0;
        for (const auto& e : entry.extents) {
            total64 += e.size;
            if (total64 > static_cast<size_t>(-1)) return false;
        }
        size_t total = static_cast<size_t>(total64);
        out.reserve(total);
        for (auto& e : entry.extents) {
            io::FileStream vs;
            if (!vs.open(e.volume_path, io::FileMode::ReadOnly)) return false;
            core::uint64 fsz = vs.size();
            core::uint64 remain =
                e.offset < fsz ? std::min<core::uint64>(e.size, fsz - e.offset) : 0;
            core::uint64 off = e.offset;
            while (remain > 0) {
                size_t take = static_cast<size_t>(std::min<core::uint64>(remain, 1u << 20));
                std::vector<core::byte> chunk(take);
                if (!vs.seek(static_cast<core::int64>(off), io::SeekOrigin::Begin)) return false;
                if (vs.read(chunk.data(), take) != take) return false;
                out.insert(out.end(), chunk.begin(), chunk.end());
                off += take;
                remain -= take;
            }
            if (off - e.offset < e.size) return false; // extent truncated on disk
        }
        return true;
    }
    // Legacy single extent fallback. Read through a private handle: this is a
    // const, logically read-only operation, so it must not reposition the
    // shared stream_ (a data race with concurrent readers — report L16).
    io::FileStream local;
    if (!local.open(path_, io::FileMode::ReadOnly)) return false;
    local.seek(static_cast<core::int64>(entry.data_offset), io::SeekOrigin::Begin);
    // Bound the allocation by the bytes the file actually holds; data_size is
    // untrusted header data and must not size a vector directly.
    core::uint64 fsz = local.size();
    core::uint64 readable = fsz > entry.data_offset ? fsz - entry.data_offset : 0;
    if (entry.data_size > readable) return false;
    out.resize(static_cast<size_t>(entry.data_size));
    return local.read(out.data(), out.size()) == out.size();
}

namespace {

// Stream every byte of a stored (method 0) entry through a callable
// `chunk_sink(const byte*, size_t)`. Reads a 64 KiB buffer at a time from
// each extent and never materialises the whole payload — matters when a
// single entry is 20 GiB and callers just want to verify a checksum.
// Returns false on any I/O failure.
template <class Fn>
bool stream_stored_entry_chunks(const archive::ArchiveEntry& entry, Fn&& chunk_sink) {
    core::byte buf[64 * 1024];
    if (entry.in_memory) {
        chunk_sink(entry.memory_data.data(), entry.memory_data.size());
        return true;
    }
    if (!entry.extents.empty()) {
        for (const auto& ext : entry.extents) {
            io::FileStream vs;
            if (!vs.open(ext.volume_path, io::FileMode::ReadOnly)) return false;
            if (!vs.seek(static_cast<core::int64>(ext.offset), io::SeekOrigin::Begin)) return false;
            core::uint64 remain = ext.size;
            while (remain > 0) {
                size_t take = static_cast<size_t>(std::min<core::uint64>(remain, sizeof(buf)));
                if (vs.read(buf, take) != take) return false;
                chunk_sink(buf, take);
                remain -= take;
            }
        }
        return true;
    }
    // No extents recorded (legacy single-file path); fall back to
    // data_offset + data_size against the reader's own stream.
    return false;
}

} // namespace

bool ArchiveReader::select_unpacker(const ArchiveEntry& entry, UnpackerSelection& sel) {
    size_t win = static_cast<size_t>(entry.header.win_size);
    bool chain = is_solid() || entry.header.is_solid;

    if (chain) {
        if (win == 0 && solid_unpacker_) {
            win = solid_unpacker_->win_size();
        } else if (win == 0) {
            win = 32 * 1024 * 1024;
        }

        if (entry.header.is_solid && solid_chain_ok_ && solid_unpacker_ &&
            solid_unpacker_->win_size() >= win) {
            sel.unpacker = solid_unpacker_.get();
            sel.solid = true;
        } else {
            if (!solid_unpacker_ || solid_unpacker_->win_size() < win) {
                solid_unpacker_ = std::make_unique<compress::Decompressor50>(win);
                if (solid_unpacker_->last_error() != compress::DecompressErrorCode::Ok) {
                    solid_unpacker_.reset();
                    solid_chain_ok_ = false;
                    return false;
                }
            }
            sel.unpacker = solid_unpacker_.get();
            sel.solid = false;
        }
    } else {
        if (win == 0) {
            win = 32 * 1024 * 1024;
        }
        sel.local = std::make_unique<compress::Decompressor50>(win);
        if (sel.local->last_error() != compress::DecompressErrorCode::Ok) {
            return false;
        }
        sel.unpacker = sel.local.get();
        sel.solid = false;
    }
    return true;
}

bool ArchiveReader::decode_compressed(const ArchiveEntry& entry, const core::byte* src,
                                      size_t src_size,
                                      std::function<bool(const core::byte*, size_t)> flush_cb) {
    bool chain = is_solid() || entry.header.is_solid;
    UnpackerSelection sel;
    if (!select_unpacker(entry, sel)) return false;
    bool ok = sel.unpacker->decompress(src, src_size, static_cast<size_t>(entry.header.unp_size),
                                       sel.solid, flush_cb);
    if (chain) solid_chain_ok_ = ok;
    return ok;
}

bool ArchiveReader::decode_compressed(const ArchiveEntry& entry,
                                      std::function<size_t(core::byte*, size_t)> src_cb,
                                      size_t src_size,
                                      std::function<bool(const core::byte*, size_t)> flush_cb) {
    bool chain = is_solid() || entry.header.is_solid;
    UnpackerSelection sel;
    if (!select_unpacker(entry, sel)) return false;
    bool ok = sel.unpacker->decompress(src_cb, src_size, static_cast<size_t>(entry.header.unp_size),
                                       sel.solid, flush_cb);
    if (chain) solid_chain_ok_ = ok;
    return ok;
}

bool ArchiveReader::test_entry(const ArchiveEntry& entry) {
    // bad_password_ is a result of the *last* password-checked call, not a
    // permanent property of this reader. Reset it before doing any work so
    // a caller that retries with a corrected password (or tests a different,
    // non-encrypted entry) doesn't see a stale failure from a prior call.
    bad_password_ = false;
    if (entry.header.is_encrypted) {
        // Encrypted entries are not verified here (password-gated decode);
        // skipping the decode abandons the shared solid LZ stream position.
        solid_chain_ok_ = false;
        return true;
    }
    if (entry.header.has_blake2sp) {
        // A present BLAKE2sp record is authoritative; the header CRC32
        // beside it is not evaluated during extraction.
        if (entry.header.method == 0) {
            // Stream directly through Blake2sp::update — no whole-payload
            // vector needed for stored entries regardless of unp_size.
            crypto::Blake2sp b2;
            if (entry.in_memory) {
                b2.update(entry.memory_data.data(), entry.memory_data.size());
            } else {
                bool io_ok = stream_stored_entry_chunks(
                    entry, [&](const core::byte* p, size_t n) { b2.update(p, n); });
                if (!io_ok) {
                    // Fall back to buffered read for the legacy single-stream
                    // path that stream_stored_entry_chunks doesn't cover.
                    std::vector<core::byte> packed;
                    if (!read_packed_data(entry, packed)) return false;
                    b2.update(packed.data(), packed.size());
                }
            }
            core::byte digest[32];
            b2.finish(digest);
            return std::memcmp(digest, entry.header.blake2sp.data(), 32) == 0;
        }
        // Compressed: filters may transform bytes in place, so we still
        // need the whole unpacked buffer before hashing.
        std::vector<core::byte> packed;
        if (!read_packed_data(entry, packed)) {
            solid_chain_ok_ = false;
            return false;
        }
        crypto::Blake2sp b2;
        auto cb = [&](const core::byte* data, size_t size) -> bool {
            b2.update(data, size);
            return true;
        };
        if (!decode_compressed(entry, packed.data(), packed.size(), cb)) return false;
        core::byte digest[32];
        b2.finish(digest);
        return std::memcmp(digest, entry.header.blake2sp.data(), 32) == 0;
    }
    if (!entry.header.has_crc32) {
        return true;
    }

    if (entry.header.method == 0) {
        // Stored: multi-volume-split or single-extent, we can CRC entirely
        // in a streaming fashion. Previously this materialised the whole
        // packed payload into a std::vector<byte> just to run one crc.update
        // call over it — catastrophic for a 20 GiB stored entry.
        crypto::Crc32 crc;
        if (entry.in_memory) {
            crc.update(entry.memory_data.data(), entry.memory_data.size());
        } else {
            bool io_ok = stream_stored_entry_chunks(
                entry, [&](const core::byte* p, size_t n) { crc.update(p, n); });
            if (!io_ok) {
                std::vector<core::byte> packed;
                if (!read_packed_data(entry, packed)) return false;
                crc.update(packed.data(), packed.size());
            }
        }
        return crc.get() == entry.header.data_crc32;
    }

    // Compressed path: same rationale as above — filters make in-place
    // transforms so the full unpacked buffer is unavoidable here.
    std::vector<core::byte> packed;
    if (!read_packed_data(entry, packed)) {
        solid_chain_ok_ = false;
        return false;
    }
    crypto::Crc32 crc;
    auto cb = [&](const core::byte* data, size_t size) -> bool {
        crc.update(data, size);
        return true;
    };
    if (!decode_compressed(entry, packed.data(), packed.size(), cb)) return false;
    return crc.get() == entry.header.data_crc32;
}

namespace {
static std::string strip_trailing_slashes(const std::string& p) {
    std::string r = p;
    while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    return r;
}
static std::string normalize_separators_to_slash(const std::string& p) {
    std::string r = p;
    for (char& c : r)
        if (c == '\\') c = '/';
    return r;
}
static std::string slash_to_native_str(const std::string& p) {
    std::string r = strip_trailing_slashes(p);
    // Convert forward slashes to native preferred separator
    for (char& c : r)
        if (c == '/') c = std::filesystem::path::preferred_separator;
    // Also keep backslashes as native (already)
    return r;
}
bool is_full_path(const std::string& p) {
    if (p.empty()) return false;
    std::string n = normalize_separators_to_slash(p);
    if (n.empty()) return false;
    if (n[0] == '/') return true;
    // Drive-relative targets ("C:evil") resolve against the process's current
    // directory on that drive - outside the extraction root (report L15).
    if (n.size() >= 2 && n[1] == ':' && std::isalpha(static_cast<unsigned char>(n[0]))) return true;
    if (n.rfind("//", 0) == 0) return true;
    if (p.rfind("\\??\\", 0) == 0) return true; // raw check for NT prefix
    if (n.rfind("//", 0) == 0) return true;
    return false;
}
int calc_allowed_depth(const std::string& arc_name) {
    std::string norm = normalize_separators_to_slash(arc_name);
    // strip trailing slash for directory entries
    bool is_dir = (!arc_name.empty() && (arc_name.back() == '/' || arc_name.back() == '\\'));
    while (!norm.empty() && norm.back() == '/') norm.pop_back();
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < norm.size()) {
        size_t nxt = norm.find('/', pos);
        std::string comp =
            (nxt == std::string::npos) ? norm.substr(pos) : norm.substr(pos, nxt - pos);
        pos = (nxt == std::string::npos) ? norm.size() : nxt + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") {
            if (!parts.empty()) parts.pop_back();
            // if empty, ".." would escape root -> keep depth 0 (don't push)
        } else {
            parts.push_back(comp);
        }
    }
    if (parts.empty()) return 0;
    if (is_dir) return static_cast<int>(parts.size());
    // last component is file name
    int d = static_cast<int>(parts.size()) - 1;
    return d < 0 ? 0 : d;
}
bool is_relative_symlink_safe(const std::string& arc_name, const std::string& target) {
    int depth = calc_allowed_depth(arc_name);
    std::string t = normalize_separators_to_slash(strip_trailing_slashes(target));
    size_t pos = 0;
    int dotdots = 0;
    while (pos < t.size()) {
        size_t nxt = t.find('/', pos);
        std::string comp = (nxt == std::string::npos) ? t.substr(pos) : t.substr(pos, nxt - pos);
        pos = (nxt == std::string::npos) ? t.size() : nxt + 1;
        if (comp.empty() || comp == ".") {
            if (nxt == std::string::npos) break;
            continue;
        }
        if (comp == "..") ++dotdots;
        if (nxt == std::string::npos) break;
    }
    return dotdots <= depth;
}
bool has_symlink_parent(const std::filesystem::path& dest_path) {
    std::error_code ec;
    std::filesystem::path parent = dest_path.parent_path();
    for (auto p = parent; !p.empty(); p = p.parent_path()) {
        auto st = std::filesystem::symlink_status(p, ec);
        if (!ec && std::filesystem::is_symlink(st)) return true;
        if (p == p.root_path() || p.parent_path() == p) break;
    }
    return false;
}
std::filesystem::path get_dest_root(const std::filesystem::path& dest_path,
                                    const std::string& arc_name) {
    // Collect the entry name's components the way sanitize_archive_path
    // resolves them ("." skipped, ".." pops). The caller builds dest_path as
    // out_root / sanitized(name), so stripping exactly these components lands
    // on the extraction root. Resolving ".." lexically here is what keeps a
    // redir source from walking above out_root (report H3): the raw archive
    // name may contain any number of "..", and walking one real parent per
    // raw component let a link/copy source reach anywhere on the drive.
    std::string norm = normalize_separators_to_slash(arc_name);
    while (!norm.empty() && norm.back() == '/') norm.pop_back();
    size_t depth = 0;
    size_t pos = 0;
    while (pos < norm.size()) {
        size_t nxt = norm.find('/', pos);
        std::string comp =
            (nxt == std::string::npos) ? norm.substr(pos) : norm.substr(pos, nxt - pos);
        pos = (nxt == std::string::npos) ? norm.size() : nxt + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") {
            if (depth > 0) --depth;
            continue;
        }
        ++depth;
    }
    std::filesystem::path root = dest_path;
    for (size_t i = 0; i < depth; ++i) {
        auto pr = root.parent_path();
        if (pr.empty() && !root.empty()) break;
        root = pr;
        if (root.empty()) break;
    }
    if (root.empty()) root = dest_path.root_path();
    return root;
}
// Resolve `rel_generic` against `root`, refusing results that climb above it
// (report H3: hardlink/FILECOPY sources must stay inside the extraction
// root). Returns false for absolute/drive-qualified targets and for targets
// whose ".." components pop past the root. On success `result` holds the
// resolved path.
bool resolve_under_root(const std::filesystem::path& root, const std::string& rel_generic,
                        std::filesystem::path& result) {
    std::filesystem::path rel(rel_generic);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return false;

    // Split the root into untouchable components.
    std::vector<std::string> stack;
    {
        std::string rs = root.generic_string();
        size_t rp = 0;
        while (rp < rs.size()) {
            size_t nxt = rs.find('/', rp);
            std::string comp = (nxt == std::string::npos) ? rs.substr(rp) : rs.substr(rp, nxt - rp);
            rp = (nxt == std::string::npos) ? rs.size() : nxt + 1;
            if (!comp.empty() && comp != ".") stack.push_back(comp);
        }
    }
    if (stack.empty()) return false; // refuse to confine against a root we cannot identify

    size_t root_depth = stack.size();
    std::string t = normalize_separators_to_slash(strip_trailing_slashes(rel_generic));
    size_t pos = 0;
    while (pos < t.size()) {
        size_t nxt = t.find('/', pos);
        std::string comp = (nxt == std::string::npos) ? t.substr(pos) : t.substr(pos, nxt - pos);
        pos = (nxt == std::string::npos) ? t.size() : nxt + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") {
            if (stack.size() <= root_depth) return false; // escapes the extraction root
            stack.pop_back();
            continue;
        }
        stack.push_back(comp);
    }
    if (stack.size() <= root_depth) return false; // target is the root itself, not a file

    // Re-join: keep the root's own string form verbatim (so "C:/", UNC and
    // trailing-slash forms survive), then append the components resolved
    // relative to it.
    std::string joined = root.generic_string();
    while (joined.size() > 1 && joined.back() == '/') joined.pop_back();
    for (size_t i = root_depth; i < stack.size(); ++i) joined += "/" + stack[i];
    result = std::filesystem::path(joined);
    return true;
}
} // namespace

// Safe links-to-directories conversion semantics (report M9): when a later entry needs the
// parent chain of its destination as real directories, only links THIS
// reader created during the current session are converted - the old
// implementation walked from the destination to the filesystem root and
// deleted any pre-existing user symlink/junction it met. Links that no
// longer exist or were replaced are skipped.
void ArchiveReader::convert_self_links(const std::filesystem::path& dest_path) {
    std::error_code ec;
    std::filesystem::path parent = dest_path.parent_path();
    for (const auto& link : links_created_) {
        for (auto anc = parent; !anc.empty(); anc = anc.parent_path()) {
            if (anc == link) {
                auto st = std::filesystem::symlink_status(link, ec);
                if (!ec && std::filesystem::is_symlink(st)) {
                    std::filesystem::remove(link, ec);
                    std::filesystem::create_directories(link, ec);
                }
                break;
            }
            if (anc == anc.root_path() || anc.parent_path() == anc) break;
        }
    }
}

bool ArchiveReader::ensure_parent_dir(const std::filesystem::path& dest_path,
                                      const std::string& entry_name) {
    if (!dest_path.has_parent_path() || dest_path.parent_path().empty()) {
        return true; // Flat path in current directory
    }
    std::error_code ec;
    std::filesystem::create_directories(dest_path.parent_path(), ec);
    if (ec) {
        // Non-throwing error reporting: ec.message() without dest_path.parent_path().string()
        // which could throw std::system_error on Windows if the path contains non-ASCII characters
        (void)entry_name;
        return false;
    }
    return true;
}

bool ArchiveReader::extract_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                                  const std::string& password) {
    std::string pass = password.empty() ? password_ : password;

    // Same rationale as test_entry(): don't let a wrong-password failure
    // from a previous call linger and mislead a caller that retries this
    // reader instance with the correct password.
    bad_password_ = false;

    // Directory record (FHFL_DIRECTORY, no data area): materialize the
    // directory itself so empty directories survive extraction. Best-effort —
    // failure to create (existing file in the way, permissions) is a skip,
    // not an extraction error.
    if (entry.header.file_flags & format::FHFL_DIRECTORY) {
        std::error_code mk_ec;
        std::filesystem::create_directories(dest_path, mk_ec);
        return true;
    }

    // 07-services.md: Redirection – symlinks/junctions/hardlinks/filecopy
    if (entry.header.redir_type != 0) {
        if (!extract_symlinks_) {
            return true; // skipped per -ol-
        }
        // NameSize already validated <2048 in header_reader
        const std::string& raw_target = entry.header.redir_target;
        std::string target = strip_trailing_slashes(raw_target);
        [[maybe_unused]] bool is_dir_target = entry.header.redir_dir_target; // used on _WIN32
        int rtype = entry.header.redir_type;                                 // 1..5
        // Normalize for safety: forward slash -> '/' already, but check both separators
        std::string target_for_check = normalize_separators_to_slash(target);
        // Safety: reject absolute or escaping relative links unless explicitly allowed
        if (is_full_path(target_for_check) ||
            !is_relative_symlink_safe(entry.header.file_name, target_for_check)) {
            // Spec 07: warn UIERROR_SKIPUNSAFELINK, skip
            return true; // skip but not fail
        }
        // LinksToDirs scan: deny if any parent component is symlink (per 07:256)
        if (has_symlink_parent(dest_path)) {
            return true; // skip unsafe
        }
        if (!ensure_parent_dir(dest_path, entry.header.file_name)) return false;
        // Remove existing file/dir if present
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        // Convert forward slash -> native and strip trailing slash already done
        std::string native_target = slash_to_native_str(target);
        // For filesystem APIs, use native_target path; on Unix forward slash is native so unchanged
        if (rtype == 1) { // UNIXSYMLINK
#ifndef _WIN32
            if (::symlink(native_target.c_str(), dest_path.c_str()) != 0) return false;
            links_created_.push_back(dest_path);
            return true;
#else
            std::error_code ec2;
            // Use native-converted target for Windows symlink; forward slash already converted
            std::filesystem::path p_native(native_target);
            if (is_dir_target)
                std::filesystem::create_directory_symlink(p_native, dest_path, ec2);
            else
                std::filesystem::create_symlink(p_native, dest_path, ec2);
            if (ec2) {
                if (ec2.value() == 1314) { // ERROR_PRIVILEGE_NOT_HELD
                    std::cerr << "W: symlink privilege not held; skipping link: "
                              << dest_path.generic_string() << "\n";
                    return true;
                }
                return false;
            }
            links_created_.push_back(dest_path);
            return true;
#endif
        } else if (rtype == 2 || rtype == 3) { // WINSYMLINK / JUNCTION
#ifdef _WIN32
            std::error_code ec2;
            std::filesystem::path p_native(native_target);
            if (rtype == 3) {
                std::filesystem::create_directory_symlink(p_native, dest_path, ec2);
            } else {
                if (is_dir_target)
                    std::filesystem::create_directory_symlink(p_native, dest_path, ec2);
                else
                    std::filesystem::create_symlink(p_native, dest_path, ec2);
            }
            if (ec2) {
                if (ec2.value() == 1314) { // ERROR_PRIVILEGE_NOT_HELD
                    std::cerr << "W: symlink privilege not held; skipping link: "
                              << dest_path.generic_string() << "\n";
                    return true;
                }
                return false;
            }
            links_created_.push_back(dest_path);
            return true;
#else
            if (::symlink(native_target.c_str(), dest_path.c_str()) != 0) return false;
            links_created_.push_back(dest_path);
            return true;
#endif
        } else if (rtype == 4) { // HARDLINK
            // Resolve DestRoot correctly (use out_root parent of archive extraction, not dest_path.parent)
            std::filesystem::path dest_root = get_dest_root(dest_path, entry.header.file_name);
            std::string tgt_generic = normalize_separators_to_slash(target);
            // tgt_generic already stripped? ensure stripped
            tgt_generic = strip_trailing_slashes(tgt_generic);
            // Convert to native relative
            std::string tgt_native = slash_to_native_str(tgt_generic);
            if (tgt_native.empty()) return true;
            // Build src = DestRoot + RedirName (SlashToNative, ConvertPath),
            // confined to the extraction root: a target that is absolute or
            // climbs above DestRoot is skipped (report H3).
            std::filesystem::path hard_src;
            if (!resolve_under_root(dest_root, tgt_generic, hard_src)) return true;
            std::error_code ec2;
            // RefList avoidance: if already extracted hardlink target exists, link to it; if src missing, skip
            if (!std::filesystem::exists(hard_src, ec2)) {
                return true; // skip, target not yet available (avoid double extraction)
            }
            // Also deny if hard_src parent chain contains symlink (LinksToDirs)
            if (has_symlink_parent(hard_src)) return true;
            std::filesystem::create_hard_link(hard_src, dest_path, ec2);
            if (ec2) return false;
            return true;
        } else if (rtype == 5) { // FILECOPY
            std::filesystem::path dest_root = get_dest_root(dest_path, entry.header.file_name);
            std::string tgt_generic = normalize_separators_to_slash(target);
            tgt_generic = strip_trailing_slashes(tgt_generic);
            if (tgt_generic.empty()) return true;
            std::filesystem::path src;
            if (!resolve_under_root(dest_root, tgt_generic, src)) return true;
            std::error_code ec2;
            if (!std::filesystem::exists(src, ec2)) return true;
            if (has_symlink_parent(src)) return true;
            std::filesystem::copy_file(src, dest_path,
                                       std::filesystem::copy_options::overwrite_existing, ec2);
            if (ec2) return false;
            return true;
        }
        return true;
    }

    // Regular file extraction:
    // Sanitize -> symlink checks on the sanitized path (B3).
    // TOCTOU notice: Check-then-open has an inherent residual race window against external
    // concurrent processes modifying symlinks on the filesystem between symlink_status() and open().
    convert_self_links(dest_path);
    if (has_symlink_parent(dest_path)) {
        return false;
    }
    std::error_code ec_sym;
    auto st_dest = std::filesystem::symlink_status(dest_path, ec_sym);
    if (!ec_sym && std::filesystem::is_symlink(st_dest)) {
        std::filesystem::remove(dest_path, ec_sym);
    }

    if (entry.in_memory) {
        // Currently unreachable (no producer sets in_memory) but kept
        // consistent with the file paths: same write checks, same hash
        // policy — so a future producer cannot reintroduce the
        // verification asymmetry silently.
        if (!ensure_parent_dir(dest_path, entry.header.file_name)) return false;
        FileUnlinker unlinker(dest_path, keep_broken_);
        io::FileStream out;
        if (!out.open(dest_path, io::FileMode::CreateAlways)) return false;
        unlinker.armed = true;
        const bool m_use_blake = entry.header.has_blake2sp;
        const bool m_use_crc = !m_use_blake && entry.header.has_crc32;
        crypto::Blake2sp m_b2;
        crypto::Crc32 m_crc;
        if (entry.header.method == 0) {
            size_t write_len = static_cast<size_t>(entry.header.unp_size);
            if (write_len > entry.memory_data.size()) write_len = entry.memory_data.size();
            if (out.write(entry.memory_data.data(), write_len) != write_len) {
                out.close();
                return false;
            }
            if (m_use_blake) m_b2.update(entry.memory_data.data(), write_len);
            if (m_use_crc) m_crc.update(entry.memory_data.data(), write_len);
        } else {
            auto cb = [&](const core::byte* data, size_t size) -> bool {
                if (m_use_blake) m_b2.update(data, size);
                if (m_use_crc) m_crc.update(data, size);
                return out.write(data, size) == size;
            };
            if (!decode_compressed(entry, entry.memory_data.data(), entry.memory_data.size(), cb)) {
                out.close();
                return false;
            }
        }
        if (m_use_blake) {
            core::byte digest[32];
            m_b2.finish(digest);
            if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) {
                out.close();
                return false;
            }
        } else if (m_use_crc && m_crc.get() != entry.header.data_crc32) {
            out.close();
            return false;
        }
        out.close();
        unlinker.dismissed = true;
        return true;
    }

    if (entry.header.is_encrypted) {
        if (pass.empty()) {
            bad_password_ = true;
            solid_chain_ok_ = false;
            return false;
        }
        if (entry.header.lg2_count >= 25) {
            solid_chain_ok_ = false;
            return false;
        }
        crypto::Rar5Keys keys;
        crypto::Pbkdf2Rar5::derive_keys(pass, entry.header.salt.data(), 16,
                                        1U << entry.header.lg2_count, keys);
        if (entry.header.has_psw_check) {
            // Constant-time compare, matching the header-decrypt path: an
            // early-exit memcmp would leak the count of leading matching
            // PswCheck bytes through extraction timing.
            if (!crypto::Pbkdf2Rar5::constant_time_equal(keys.psw_check,
                                                         entry.header.psw_check.data(), 8)) {
                bad_password_ = true;
                solid_chain_ok_ = false;
                return false;
            }
        }
        std::vector<core::byte> cipher;
        if (!read_packed_data(entry, cipher)) {
            solid_chain_ok_ = false;
            return false;
        }
        if (cipher.size() % 16 != 0) {
            // A trailing partial cipher block means truncated or corrupt
            // ciphertext. AES-CBC has no stream mode: the old floor to whole
            // blocks dropped the tail without any diagnostic (Q5).
            solid_chain_ok_ = false;
            return false;
        }
        core::byte iv[16];
        std::memcpy(iv, entry.header.init_v.data(), 16);
        crypto::Aes256 aes(keys.aes_key);
        if (!aes.decrypt_cbc(cipher.data(), cipher.size(), iv)) {
            solid_chain_ok_ = false;
            return false;
        }

        // Encrypted-entry verification: when the writer stored the plaintext
        // CRC (our writer does — crypt_flags without 0x0002) it is comparable
        // and verified below. When the tweaked-checksum flag (0x0002, see
        // RAR5-FORMAT.md) is set, the stored value is key-dependent and not
        // comparable — the PswCheck above already authenticated the key, and
        // stream_payload applies the same exception.
        const bool tweaked_checksums = (entry.header.crypt_flags & 0x0002) != 0;
        const bool v_use_blake = entry.header.has_blake2sp && !tweaked_checksums;
        const bool v_use_crc = !v_use_blake && entry.header.has_crc32 && !tweaked_checksums;
        crypto::Blake2sp v_b2;
        crypto::Crc32 v_crc;

        if (!ensure_parent_dir(dest_path, entry.header.file_name)) return false;
        FileUnlinker unlinker(dest_path, keep_broken_);
        io::FileStream out;
        if (!out.open(dest_path, io::FileMode::CreateAlways)) return false;
        unlinker.armed = true;
        if (entry.header.method == 0) {
            size_t write_len = static_cast<size_t>(entry.header.unp_size);
            if (write_len > cipher.size()) write_len = cipher.size();
            if (out.write(cipher.data(), write_len) != write_len) {
                out.close();
                return false;
            }
            if (v_use_blake) v_b2.update(cipher.data(), write_len);
            if (v_use_crc) v_crc.update(cipher.data(), write_len);
        } else {
            auto cb = [&](const core::byte* data, size_t size) -> bool {
                if (v_use_blake) v_b2.update(data, size);
                if (v_use_crc) v_crc.update(data, size);
                return out.write(data, size) == size;
            };
            if (!decode_compressed(entry, cipher.data(), cipher.size(), cb)) {
                out.close();
                return false;
            }
        }
        if (v_use_blake) {
            core::byte digest[32];
            v_b2.finish(digest);
            if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) {
                out.close();
                return false;
            }
        } else if (v_use_crc && v_crc.get() != entry.header.data_crc32) {
            out.close();
            return false;
        }
        out.close();
        unlinker.dismissed = true;
        return true;
    }

    if (entry.header.method == 0) {
        return extract_store_entry(entry, dest_path, pass);
    }

    if (!ensure_parent_dir(dest_path, entry.header.file_name)) return false;
    std::vector<core::byte> packed;
    if (!read_packed_data(entry, packed)) {
        solid_chain_ok_ = false;
        return false;
    }
    FileUnlinker unlinker(dest_path, keep_broken_);
    io::FileStream out;
    if (!out.open(dest_path, io::FileMode::CreateAlways)) {
        return false;
    }
    unlinker.armed = true;

    // Verify the decoded stream while writing (verification-asymmetry fix):
    // the stored path and the streaming path both refuse corrupt payload, so
    // the compressed path must not silently write wrong bytes and report OK.
    // Hash policy matches stream_payload/test_entry: BLAKE2sp authoritative
    // when present, CRC32 otherwise.
    const bool v_use_blake = entry.header.has_blake2sp;
    const bool v_use_crc = !v_use_blake && entry.header.has_crc32;
    crypto::Blake2sp v_b2;
    crypto::Crc32 v_crc;
    auto cb = [&](const core::byte* data, size_t size) -> bool {
        if (v_use_blake) v_b2.update(data, size);
        if (v_use_crc) v_crc.update(data, size);
        return out.write(data, size) == size;
    };
    if (!decode_compressed(entry, packed.data(), packed.size(), cb)) {
        out.close();
        return false;
    }
    if (v_use_blake) {
        core::byte digest[32];
        v_b2.finish(digest);
        if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) {
            out.close();
            return false; // FileUnlinker removes the partial output
        }
    } else if (v_use_crc && v_crc.get() != entry.header.data_crc32) {
        out.close();
        return false;
    }

    out.close();
    unlinker.dismissed = true;
    return true;
}

bool ArchiveReader::extract_store_entry(const ArchiveEntry& entry,
                                        const std::filesystem::path& dest_path,
                                        const std::string& password) {
    if (entry.header.method != 0) {
        return false;
    }
    if (entry.header.is_encrypted) {
        return extract_entry(entry, dest_path, password);
    }

    // Sanitize -> symlink checks on the sanitized path (B3).
    // TOCTOU notice: Check-then-open has an inherent residual race window against external
    // concurrent processes modifying symlinks on the filesystem between symlink_status() and open().
    convert_self_links(dest_path);
    if (has_symlink_parent(dest_path)) {
        return false;
    }
    std::error_code ec_sym;
    auto st_dest = std::filesystem::symlink_status(dest_path, ec_sym);
    if (!ec_sym && std::filesystem::is_symlink(st_dest)) {
        std::filesystem::remove(dest_path, ec_sym);
    }

    if (!ensure_parent_dir(dest_path, entry.header.file_name)) return false;

    FileUnlinker unlinker(dest_path, keep_broken_);
    io::FileStream out;
    if (!out.open(dest_path, io::FileMode::CreateAlways)) {
        return false;
    }
    unlinker.armed = true;

    if (!entry.extents.empty()) {
        // Multivolume store: stitch extents. Same payload verification as the
        // contiguous path below — corrupt stored data is an extraction
        // failure, never a silent successful write of wrong bytes. BLAKE2sp
        // is authoritative when present (some third-party writers store it
        // instead of CRC32); CRC32 otherwise.
        crypto::Blake2sp blake;
        crypto::Crc32 crc;
        const bool use_blake = entry.header.has_blake2sp;
        const bool use_crc = !use_blake && entry.header.has_crc32;
        for (auto& e : entry.extents) {
            io::FileStream vs;
            if (!vs.open(e.volume_path, io::FileMode::ReadOnly)) {
                out.close();
                return false;
            }
            vs.seek(static_cast<core::int64>(e.offset), io::SeekOrigin::Begin);
            core::byte buf[8192];
            core::uint64 remaining = e.size;
            while (remaining > 0) {
                size_t take = static_cast<size_t>(
                    std::min(remaining, static_cast<core::uint64>(sizeof(buf))));
                if (vs.read(buf, take) != take) {
                    out.close();
                    return false;
                }
                if (out.write(buf, take) != take) {
                    out.close();
                    return false;
                }
                if (use_blake) blake.update(buf, take);
                if (use_crc) crc.update(buf, take);
                remaining -= take;
            }
        }
        out.close();
        if (use_blake) {
            core::byte digest[32];
            blake.finish(digest);
            if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) return false;
        } else if (use_crc && crc.get() != entry.header.data_crc32) {
            return false;
        }
        unlinker.dismissed = true;
        return true;
    }

    stream_.seek(static_cast<core::int64>(entry.data_offset), io::SeekOrigin::Begin);
    core::byte buf[8192];
    core::uint64 remaining = entry.data_size;
    crypto::Blake2sp blake;
    crypto::Crc32 crc;
    const bool use_blake = entry.header.has_blake2sp;
    const bool use_crc = !use_blake && entry.header.has_crc32;

    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(sizeof(buf))));
        if (stream_.read(buf, take) != take) {
            out.close();
            return false;
        }
        if (out.write(buf, take) != take) {
            out.close();
            return false;
        }
        if (use_blake) blake.update(buf, take);
        if (use_crc) crc.update(buf, take);
        remaining -= take;
    }

    out.close();
    if (use_blake) {
        core::byte digest[32];
        blake.finish(digest);
        if (std::memcmp(digest, entry.header.blake2sp.data(), 32) != 0) return false;
    } else if (use_crc && crc.get() != entry.header.data_crc32) {
        // Same verdict as the streaming verify path (T5): corrupt payload is
        // an extraction failure, never a silent successful write of wrong
        // bytes. The FileUnlinker destructor removes the partial output
        // unless the caller opted into keep_broken.
        return false;
    }
    unlinker.dismissed = true;
    return true;
}

} // namespace openrar::archive
