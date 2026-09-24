#include "extraction_journal.hpp"

#include "../crypto/crc32.hpp"
#include "../crypto/rng.hpp"
#include "path_util.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openrar::io {

namespace {

// ── Journal format ───────────────────────────────────────────────────────────
//
//   OPENRAR-JOURNAL 1\n
//   T <len> <crc32-8hex>\n<len bytes of UTF-8 absolute temp path>
//   ... (append-only)
//
// Every record is checksummed; a torn or garbage tail is ignored (stop at the
// first inconsistent record — only our writers append, and they append
// atomically under the exclusive lock, so corruption is bounded to a torn
// final record from a hard kill). The checksum plus the directory/shape
// validation in sweep_directory is what keeps a forged journal from naming
// arbitrary victims.

constexpr const char* kJournalHeader = "OPENRAR-JOURNAL 1\n";
constexpr size_t kJournalHeaderLen = 18;
constexpr size_t kMaxRecordPathLen = 32768;

std::string hex8(uint32_t v) {
    static const char* d = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
        s[static_cast<size_t>(i)] = d[v & 0xF];
        v >>= 4;
    }
    return s;
}

uint32_t path_checksum(const std::string& path_bytes) {
    crypto::Crc32 crc;
    crc.update(reinterpret_cast<const core::byte*>(path_bytes.data()), path_bytes.size());
    return crc.get();
}

std::string temp_suffix_hex() {
    core::byte rnd[16];
    if (!crypto::secure_random_bytes(rnd, sizeof(rnd))) return "";
    static const char* d = "0123456789abcdef";
    std::string s(32, '0');
    for (size_t i = 0; i < sizeof(rnd); ++i) {
        s[i * 2] = d[(rnd[i] >> 4) & 0xF];
        s[i * 2 + 1] = d[rnd[i] & 0xF];
    }
    return s;
}

#ifdef _WIN32

// Exclusive-locked journal file (share mode 0 is the Windows advisory lock).
class LockedFile {
public:
    LockedFile() = default;
    ~LockedFile() { close(); }

    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;

    // Create-or-open exclusively. False when another process holds the file.
    bool open_create(const std::filesystem::path& p) { return open(p, OPEN_ALWAYS); }
    // Open an existing file exclusively (sweep probe). False when locked,
    // missing, or unreadable.
    bool open_existing(const std::filesystem::path& p) { return open(p, OPEN_EXISTING); }

    bool read_all(std::string& out) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(h_, &li) || li.QuadPart < 0) return false;
        if (li.QuadPart == 0) {
            out.clear();
            return true;
        }
        out.resize(static_cast<size_t>(li.QuadPart));
        if (!SetFilePointerEx(h_, LARGE_INTEGER{}, nullptr, FILE_BEGIN)) return false;
        size_t got = 0;
        while (got < out.size()) {
            DWORD chunk = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - got, 0x40000000));
            if (!ReadFile(h_, out.data() + got, want, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            got += chunk;
        }
        return true;
    }

    bool append(const std::string& data) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h_, zero, nullptr, FILE_END)) return false;
        size_t put = 0;
        while (put < data.size()) {
            DWORD wrote = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - put, 0x40000000));
            if (!WriteFile(h_, data.data() + put, want, &wrote, nullptr) || wrote == 0) {
                return false;
            }
            put += wrote;
        }
        return true;
    }

    bool sync() { return h_ != INVALID_HANDLE_VALUE && FlushFileBuffers(h_) != FALSE; }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    bool open(const std::filesystem::path& p, DWORD disposition) {
        close();
        h_ = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                         0 /* exclusive: the journal lock */, nullptr, disposition,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }

    HANDLE h_ = INVALID_HANDLE_VALUE;
};

unsigned long this_pid() {
    return GetCurrentProcessId();
}

#else // POSIX

// flock-held journal file (flock(LOCK_EX|LOCK_NB) is the advisory lock).
// NOTE: flock conflicts across open file descriptions even within one
// process, so an in-process sweep correctly sees this session's journals as
// locked.
class LockedFile {
public:
    LockedFile() = default;
    ~LockedFile() { close(); }

    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;

    bool open_create(const std::filesystem::path& p) {
        return open(p, O_RDWR | O_CREAT | O_CLOEXEC, true);
    }
    bool open_existing(const std::filesystem::path& p) {
        return open(p, O_RDWR | O_CLOEXEC, false);
    }

    bool read_all(std::string& out) {
        if (fd_ < 0) return false;
        struct stat st;
        if (::fstat(fd_, &st) != 0 || st.st_size < 0) return false;
        out.resize(static_cast<size_t>(st.st_size));
        size_t got = 0;
        if (::lseek(fd_, 0, SEEK_SET) < 0) return false;
        while (got < out.size()) {
            ssize_t r = ::read(fd_, out.data() + got, out.size() - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    bool append(const std::string& data) {
        if (fd_ < 0) return false;
        if (::lseek(fd_, 0, SEEK_END) < 0) return false;
        size_t put = 0;
        while (put < data.size()) {
            ssize_t w = ::write(fd_, data.data() + put, data.size() - put);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            put += static_cast<size_t>(w);
        }
        return true;
    }

    bool sync() { return fd_ >= 0 && ::fsync(fd_) == 0; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    bool open(const std::filesystem::path& p, int flags, bool lock_now) {
        close();
        fd_ = ::open(p.c_str(), flags, 0600);
        if (fd_ < 0) return false;
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            int saved = errno;
            ::close(fd_);
            fd_ = -1;
            errno = saved;
            return false;
        }
        (void)lock_now;
        return true;
    }

    int fd_ = -1;
};

unsigned long this_pid() {
    return static_cast<unsigned long>(::getpid());
}

#endif // _WIN32

std::filesystem::path normalize_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(dir, ec);
    if (ec) return dir;
    abs = abs.lexically_normal();
    // lexically_normal keeps a trailing separator for "."-terminated inputs
    // ("x/." → "x/") while parent_path() of files under it yields "x" —
    // strip the empty final component so directory keys match exactly in
    // register/release/sweep.
    if (!abs.empty() && abs.filename().empty() && abs.parent_path() != abs) {
        abs = abs.parent_path();
    }
    return abs;
}

// Appends one record and fsyncs. The record reaches the disk BEFORE the
// caller creates the temp — durable-first ordering, the core of the
// orphan-hygiene guarantee.
bool append_record(LockedFile& f, const std::filesystem::path& temp_abs) {
    std::string path_bytes = u8_str(temp_abs);
    if (path_bytes.size() > kMaxRecordPathLen) return false;
    std::string rec = "T ";
    rec += std::to_string(path_bytes.size());
    rec += " ";
    rec += hex8(path_checksum(path_bytes));
    rec += "\n";
    rec += path_bytes;
    return f.append(rec) && f.sync();
}

} // namespace

// ── ExtractionSession ────────────────────────────────────────────────────────

ExtractionSession::ExtractionSession() = default;

bool ExtractionSession::matches_temp_shape(const std::string& name) {
    // <name>.<32 lowercase hex>.tmp  (name part non-empty)
    constexpr size_t kHexLen = 32;
    constexpr size_t kSuffixLen = 4; // ".tmp"
    if (name.size() < 1 + 1 + kHexLen + kSuffixLen) return false;
    if (name.compare(name.size() - kSuffixLen, kSuffixLen, ".tmp") != 0) return false;
    const size_t hex_pos = name.size() - kSuffixLen - kHexLen;
    if (name[hex_pos - 1] != '.') return false;
    for (size_t i = 0; i < kHexLen; ++i) {
        const char c = name[hex_pos + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

void ExtractionSession::journal_name(std::filesystem::path& out) {
    core::byte nonce[4];
    if (!crypto::secure_random_bytes(nonce, sizeof(nonce))) return; // out unchanged
    std::string name = ".openrar_journal_";
    name += std::to_string(this_pid());
    name += "_";
    name += std::to_string(static_cast<unsigned long>(time(nullptr)));
    name += "_";
    name += hex8((static_cast<uint32_t>(nonce[0]) << 24) | (static_cast<uint32_t>(nonce[1]) << 16) |
                 (static_cast<uint32_t>(nonce[2]) << 8) | static_cast<uint32_t>(nonce[3]));
    name += ".tmp";
    out = name;
}

bool ExtractionSession::register_temp(const std::filesystem::path& dir,
                                      const std::filesystem::path& temp_abs) {
    std::filesystem::path key = normalize_dir(dir);
    auto it = journals_.find(key);
    if (it == journals_.end()) {
        // Startup sweep for this directory — once, before this run's journal
        // exists here. Locked journals of live concurrent extractors are
        // skipped by the lock itself.
        sweep_directory(key);

        std::filesystem::path name;
        journal_name(name);
        if (name.empty()) return false; // no strong entropy: fail closed
        const std::filesystem::path jpath = key / name;

        LockedFile* lf = new LockedFile();
        if (!lf->open_create(jpath)) {
            delete lf;
            return false; // locked by someone else should not happen (fresh name)
        }
        if (!lf->append(kJournalHeader) || !lf->sync()) {
            lf->close();
            delete lf;
            std::error_code rm_ec;
            std::filesystem::remove(jpath, rm_ec);
            return false;
        }
        it = journals_.emplace(key, DirJournal{lf, 0}).first;
        journal_paths_[key] = jpath;
    }
    if (!append_record(*static_cast<LockedFile*>(it->second.file), temp_abs)) return false;
    ++it->second.in_flight;
    return true;
}

void ExtractionSession::release_temp(const std::filesystem::path& temp_abs) {
    std::filesystem::path key = normalize_dir(temp_abs.parent_path());
    auto it = journals_.find(key);
    if (it == journals_.end() || it->second.in_flight == 0) return;
    if (--it->second.in_flight == 0) {
        static_cast<LockedFile*>(it->second.file)->close();
        delete static_cast<LockedFile*>(it->second.file);
        const auto jp = journal_paths_.find(key);
        if (jp != journal_paths_.end()) {
            std::error_code ec;
            std::filesystem::remove(jp->second, ec);
            journal_paths_.erase(jp);
        }
        journals_.erase(it);
    }
}

std::string ExtractionSession::journal_content(const std::filesystem::path& dir) {
    auto it = journals_.find(normalize_dir(dir));
    if (it == journals_.end()) return "";
    std::string out;
    if (!static_cast<LockedFile*>(it->second.file)->read_all(out)) return "";
    return out;
}

bool ExtractionSession::sweep_directory(const std::filesystem::path& dir) {
    const std::filesystem::path base = normalize_dir(dir);

    // Collect journal candidates first; deleting files while iterating the
    // directory is avoided entirely.
    std::vector<std::filesystem::path> journals;
    std::error_code ec;
    std::filesystem::directory_iterator it(base, ec);
    if (ec) return false;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string name = u8_str(it->path().filename());
        if (name.size() > 17 && name.compare(0, 17, ".openrar_journal_") == 0 &&
            name.compare(name.size() - 4, 4, ".tmp") == 0) {
            journals.push_back(it->path());
        }
    }

    for (const auto& jpath : journals) {
        LockedFile lf;
        if (!lf.open_existing(jpath)) continue; // locked: a live extractor owns it
        std::string content;
        if (!lf.read_all(content)) continue;
        if (content.size() < kJournalHeaderLen ||
            content.compare(0, kJournalHeaderLen, kJournalHeader) != 0) {
            continue; // garbage or unknown journal version: leave it alone
        }

        size_t pos = kJournalHeaderLen;
        bool any = false;
        while (pos < content.size()) {
            // "T <len> <crc8hex>\n" + <len> path bytes
            if (content[pos] != 'T' || pos + 1 >= content.size() || content[pos + 1] != ' ') break;
            size_t p = pos + 2;
            size_t len = 0;
            bool have_len = false;
            while (p < content.size() && content[p] >= '0' && content[p] <= '9') {
                len = len * 10 + static_cast<size_t>(content[p] - '0');
                if (len > kMaxRecordPathLen) break;
                ++p;
                have_len = true;
            }
            if (!have_len || p >= content.size() || content[p] != ' ') break;
            ++p;
            if (p + 8 >= content.size() || content[p + 8] != '\n') break;
            uint32_t crc = 0;
            bool hex_ok = true;
            for (size_t i = 0; i < 8; ++i) {
                const char c = content[p + i];
                uint32_t v;
                if (c >= '0' && c <= '9')
                    v = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    v = static_cast<uint32_t>(c - 'a' + 10);
                else {
                    hex_ok = false;
                    break;
                }
                crc = (crc << 4) | v;
            }
            if (!hex_ok) break;
            p += 9; // past crc + '\n'
            if (len == 0 || p + len > content.size()) break;
            const std::string path_bytes = content.substr(p, len);
            if (path_checksum(path_bytes) != crc) break; // checksummed: torn tail stops replay
            pos = p + len;

            const std::filesystem::path ref = std::filesystem::u8path(path_bytes);
            // A journal only vouches for temps in its OWN directory, with our
            // exact temp shape. This is what neutralizes forged journals: an
            // attacker who can write a journal into `base` can never name a
            // victim outside `base`, and never a name this code would not
            // have created itself.
            if (normalize_dir(ref.parent_path()) != base) continue;
            if (!matches_temp_shape(u8_str(ref.filename()))) continue;
            if (ref == jpath) continue;
            std::error_code rm_ec;
            std::filesystem::remove(ref, rm_ec); // no-follow: removes a link, never its target
            any = true;
        }

        lf.close();
        // Unlink only journals we could fully parse as ours (header verified
        // above); unparseable ones are left for a human, never pattern-swept.
        if (!content.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(jpath, rm_ec);
        }
        (void)any;
    }
    return true;
}

ExtractionSession::~ExtractionSession() {
    // Close + unlink every journal this run still holds (no temps in flight
    // anywhere — AtomicWriter destructors ran first by declaration order).
    for (auto& kv : journals_) {
        static_cast<LockedFile*>(kv.second.file)->close();
        delete static_cast<LockedFile*>(kv.second.file);
        const auto jp = journal_paths_.find(kv.first);
        if (jp != journal_paths_.end()) {
            std::error_code ec;
            std::filesystem::remove(jp->second, ec);
        }
    }
}

// ── AtomicWriter ─────────────────────────────────────────────────────────────

AtomicWriter::AtomicWriter() = default;

AtomicWriter::~AtomicWriter() {
    if (!finished_) abandon();
}

bool AtomicWriter::open(ExtractionSession& session, const std::filesystem::path& dest) {
    std::filesystem::path dir = dest.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    std::filesystem::path dir_abs = std::filesystem::absolute(dir, ec);
    if (ec) return false;
    dir_abs = dir_abs.lexically_normal();

    // Commit target is pinned absolute so a CWD change mid-run cannot make
    // the temp and the commit land in different directories.
    dest_path_ = dir_abs / dest.filename();

    const std::filesystem::path stem = dir_abs / dest.filename();
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const std::string hex = temp_suffix_hex();
        if (hex.empty()) return false; // no strong entropy: fail closed
        std::filesystem::path temp = stem;
        temp += "." + hex + ".tmp";

        if (!session.register_temp(dir_abs, temp)) return false;
        journaled_ = true;

        if (stream_.open(temp, FileMode::CreateNew)) {
            temp_path_ = std::move(temp);
            session_ = &session;
            return true;
        }
        if (!stream_.is_collision_error()) {
            // Real IO failure (permissions, ENOSPC): the record names a temp
            // that will never exist — harmless for sweeps, but release the
            // journal slot anyway.
            session.release_temp(temp);
            journaled_ = false;
            return false;
        }
        // Astronomically unlikely (128-bit random) — deterministic-failure
        // guard for broken CSPRNG mocks and per-directory name caps.
        session.release_temp(temp);
        journaled_ = false;
    }
    return false;
}

bool AtomicWriter::commit(CommitMode mode) {
    if (!session_ || !stream_.is_open()) return false;
    if (!stream_.commit_rename(dest_path_, mode)) {
        // Failed commit is terminal: dispose of the temp (and the journal
        // slot) now so the caller cannot observe a half-open writer state.
        abandon();
        return false;
    }
    stream_.close();
    session_->release_temp(temp_path_);
    journaled_ = false;
    finished_ = true;
    return true;
}

void AtomicWriter::abandon() {
    if (stream_.is_open()) stream_.close();
    if (journaled_ && session_) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
        session_->release_temp(temp_path_);
        journaled_ = false;
    }
    finished_ = true;
}

} // namespace openrar::io
