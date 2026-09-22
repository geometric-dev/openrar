#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "../io/path_util.hpp"
#include "../format/headers.hpp"
#include "../archive/archive_reader.hpp"
#include "../archive/archive_mutator.hpp"
#include "../compress/filters50.hpp"
#include "../archive/rar_errors.hpp"
#include "../archive/volume.hpp"
#include "../recovery/recovery_record.hpp"
#include "../recovery/recovery_writer.hpp"
#include "../io/win32_meta.hpp"
#include "progress.hpp"
#include "thread_pool.hpp"
#include "../core/cpu.hpp"
#include "../crypto/crc32.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "openrar/version.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#endif

namespace openrar::cli {

bool g_plain_mode = false;
bool g_quiet_mode = false;
bool g_assume_yes = false;

// WinRAR/unrar exit-code taxonomy (unrar errhnd.hpp RAR_EXIT; v1.21.2).
// Previously every failure collapsed to 1 — which in WinRAR semantics means
// "warning, operation succeeded" — so scripts could not distinguish corrupt
// archives from password failures from open failures. Handlers map the
// diagnosable causes; unknown failures are fatal (2).
[[maybe_unused]] constexpr int EXIT_OK = 0;
[[maybe_unused]] constexpr int EXIT_WARNING = 1;   // warnings only; operation succeeded
[[maybe_unused]] constexpr int EXIT_FATAL = 2;     // generic failure
[[maybe_unused]] constexpr int EXIT_CRC = 3;       // checksum error
[[maybe_unused]] constexpr int EXIT_LOCKED = 4;    // locked archive
[[maybe_unused]] constexpr int EXIT_WRITE = 5;     // write error
[[maybe_unused]] constexpr int EXIT_OPEN = 6;      // archive open error
[[maybe_unused]] constexpr int EXIT_USAGE = 7;     // command-line error
[[maybe_unused]] constexpr int EXIT_MEMORY = 8;    // not enough memory
[[maybe_unused]] constexpr int EXIT_NO_FILES = 10; // no files matched
[[maybe_unused]] constexpr int EXIT_BAD_PASSWORD = 11;
[[maybe_unused]] constexpr int EXIT_BAD_ARCHIVE = 13; // unrar RARX_BADARC: unrecognized archive
[[maybe_unused]] constexpr int EXIT_USER_BREAK = 255;

enum class OverwriteMode { Prompt, Overwrite, SkipExisting };

// Answer to one extraction overwrite query (B8: the query itself was never
// implemented, so Prompt silently overwrote and -y was parsed into a flag
// nothing read).
enum class OverwriteAnswer { Yes, No, Always, Never, Quit };

OverwriteAnswer ask_overwrite(const std::string& display_name) {
    // -y answers every query Yes. Non-interactive stdin (pipe, closed fd,
    // ctest) also answers Yes: batch jobs keep the pre-query behavior of
    // overwriting in place, which scripted callers rely on.
#ifdef _WIN32
    if (g_assume_yes || !_isatty(_fileno(stdin))) return OverwriteAnswer::Yes;
#else
    if (g_assume_yes || !::isatty(STDIN_FILENO)) return OverwriteAnswer::Yes;
#endif
    for (;;) {
        std::cout << "\n"
                  << display_name << " already exists. Overwrite?\n"
                  << "[Y]es, [N]o, [A]lways, n[E]ver, [Q]uit: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return OverwriteAnswer::Yes; // EOF
        if (line.empty()) return OverwriteAnswer::Yes;
        switch (std::tolower(static_cast<unsigned char>(line[0]))) {
        case 'y':
            return OverwriteAnswer::Yes;
        case 'n':
            return OverwriteAnswer::No;
        case 'a':
            return OverwriteAnswer::Always;
        case 'e':
            return OverwriteAnswer::Never;
        case 'q':
            return OverwriteAnswer::Quit;
        default:
            continue;
        }
    }
}

inline bool sw_eq(std::string_view sw, std::string_view target) {
    if (sw.size() != target.size()) return false;
    for (size_t i = 0; i < sw.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(sw[i])) !=
            std::tolower(static_cast<unsigned char>(target[i])))
            return false;
    }
    return true;
}

inline bool sw_starts(std::string_view sw, std::string_view prefix) {
    if (sw.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(sw[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

static bool read_listfile(const std::filesystem::path& list_path,
                          std::vector<std::string>& out_items, std::string& err_msg) {
    io::FileStream f;
    if (!f.open(list_path, io::FileMode::ReadOnly)) {
        err_msg = "Cannot open list file: " + list_path.string();
        return false;
    }
    size_t sz = static_cast<size_t>(f.size());
    std::string content(sz, '\0');
    if (sz > 0 && f.read(&content[0], sz) != sz) {
        err_msg = "Cannot read list file: " + list_path.string();
        return false;
    }
    f.close();

    // Strip UTF-8 BOM if present (\xEF\xBB\xBF)
    size_t offset = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        offset = 3;
    }

    std::istringstream stream(content.substr(offset));
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r and spaces/tabs
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        // Strip leading whitespace
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            start++;
        }
        if (start > 0) line = line.substr(start);

        if (line.empty()) continue;
        // Comments: lines starting with ;, #, or //
        if (line[0] == ';' || line[0] == '#' ||
            (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
            continue;
        }
        out_items.push_back(line);
    }
    return true;
}

static bool is_path_excluded(const std::string& path_str,
                             const std::vector<std::string>& exclude_patterns) {
    if (exclude_patterns.empty()) return false;
    std::string norm_path = path_str;
    for (char& c : norm_path) {
        if (c == '\\') c = '/';
    }
    if (norm_path.rfind("./", 0) == 0) {
        norm_path.erase(0, 2);
    }
    std::string base_name = norm_path;
    size_t last_slash = norm_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        base_name = norm_path.substr(last_slash + 1);
    }

    for (const auto& pat : exclude_patterns) {
        std::string norm_pat = pat;
        for (char& c : norm_pat) {
            if (c == '\\') c = '/';
        }
        if (norm_pat.rfind("./", 0) == 0) {
            norm_pat.erase(0, 2);
        }
        if (norm_pat.find('/') != std::string::npos) {
            if (io::wildcard_match(norm_pat, norm_path)) return true;
            if (!norm_pat.empty() && norm_pat.back() == '/') {
                if (norm_path.rfind(norm_pat, 0) == 0) return true;
            }
        } else {
            if (io::wildcard_match(norm_pat, base_name)) return true;
            if (io::wildcard_match(norm_pat, norm_path)) return true;
        }
    }
    return false;
}

int print_archive_to_stdout(const std::string& arc_path, const std::vector<std::string>& file_masks,
                            const std::vector<std::string>& exclude_patterns,
                            const std::string& password = "") {
#ifdef _WIN32
    // Restore the previous stdout translation mode on every exit path so any
    // later in-process output is not emitted through binary mode (v1.21.1).
    int prev_stdout_mode = _setmode(_fileno(stdout), _O_BINARY);
    struct StdoutModeRestore {
        int prev;
        ~StdoutModeRestore() {
            if (prev != -1) _setmode(_fileno(stdout), prev);
        }
    } stdout_mode_restore{prev_stdout_mode};
#endif

    archive::ArchiveReader reader;
    int open_status = archive::RAR_OK;
    std::string open_detail;
    if (!reader.open_ex(arc_path, password, open_status, open_detail)) {
        // Oracle-mapped taxonomy (unrar errhnd.hpp): missing archive =
        // 10 (NO_FILES), unrecognized = 13 (BADARC), password = 11.
        if (!std::filesystem::exists(arc_path)) {
            std::cerr << "Cannot open " << arc_path << "\n";
            return EXIT_NO_FILES;
        }
        if (open_status == archive::RAR_ERR_ENCRYPTED ||
            open_status == archive::RAR_ERR_BAD_PASSWORD) {
            std::cerr << "Cannot decrypt: BADPSW (bad password)\n";
            return EXIT_BAD_PASSWORD;
        }
        if (open_status == archive::RAR_ERR_NOT_RAR) {
            std::cerr << "Not a RAR archive: " << arc_path << "\n";
            return EXIT_BAD_ARCHIVE;
        }
        std::cerr << "Cannot open " << arc_path << "\n";
        return EXIT_OPEN;
    }

    size_t printed_entries = 0;
    for (size_t i = 0; i < reader.entries().size(); ++i) {
        const auto& entry = reader.entries()[i];
        if (entry.header.is_service) continue;
        if (entry.header.file_flags & format::FHFL_DIRECTORY) continue;

        const std::string& name = entry.header.file_name;
        if (is_path_excluded(name, exclude_patterns)) continue;

        if (!file_masks.empty()) {
            bool matched = false;
            for (const auto& mask : file_masks) {
                if (io::wildcard_match(mask, name) ||
                    io::wildcard_match(mask, std::filesystem::path(name).filename().string())) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
        }

        int rc = reader.extract_entry_sink(i, [](const core::byte* p, size_t n) -> bool {
            return std::fwrite(p, 1, n, stdout) == n;
        });
        ++printed_entries;

        if (rc != archive::RAR_OK) {
            std::fflush(stdout);
            if (reader.has_bad_password() || rc == archive::RAR_ERR_BAD_PASSWORD) {
                std::cerr << "Cannot decrypt: BADPSW (bad password) for " << name << "\n";
                return EXIT_BAD_PASSWORD;
            }
            if (rc == archive::RAR_ERR_CRC_MISMATCH) {
                std::cerr << "Checksum error in " << name << "\n";
                return EXIT_CRC;
            }
            std::cerr << "Extraction failed for " << name << "\n";
            return EXIT_FATAL;
        }
    }

    std::fflush(stdout);
    if (printed_entries == 0 && !file_masks.empty()) return EXIT_NO_FILES;
    return 0;
}

void print_banner() {
    if (g_quiet_mode) {
        return;
    }

    // OPENRAR_CLI_VERSION comes from the CMake project version; the fallback
    // only serves bare manual compiles that bypass the build system.
#ifndef OPENRAR_CLI_VERSION
#define OPENRAR_CLI_VERSION OPENRAR_VERSION_STRING
#endif
    std::cout << "\nOpenRAR " << OPENRAR_CLI_VERSION << " Open Source Archiver\n"
              << "Copyright (c) 2026 OpenRAR Project\n";
    // Acceleration line: shows which hardware kernels the dispatchers
    // actually selected on this machine, so speed differences between
    // machines have a visible explanation. Amber warning (not red — missing
    // acceleration is slow, not an error) when every path runs scalar.
    const core::AccelerationReport accel = core::describe_acceleration();
    if (!accel.tags.empty()) {
        if (is_vt_supported())
            std::cout << "\x1b[38;2;123;193;127m⚡\x1b[0m ";
        else
            std::cout << "* ";
        for (size_t i = 0; i < accel.tags.size(); ++i) {
            if (i) std::cout << " \xC2\xB7 ";
            std::cout << accel.tags[i];
        }
        std::cout << "\n";
    } else if (is_vt_supported()) {
        std::cout << "\x1b[38;2;224;164;88m⚠\x1b[0m no hardware acceleration (scalar paths)\n";
    } else {
        std::cout << "! no hardware acceleration (scalar paths)\n";
    }
    std::cout << "\n";
}

void print_help() {
    print_banner();
    std::cout
        << "Usage: openrar <command> -<switch 1> -<switch N> <archive> <files...>\n\n"
        << "<Commands>\n"
        << "  a             Add files to archive\n"
        << "  d             Delete files from archive\n"
        << "  e             Extract files without archived paths\n"
        << "  f             Freshen existing files in archive\n"
        << "  k             Lock archive against changes\n"
        << "  l[t[a],b]     List contents of archive [technical, bare]\n"
        << "  m             Move files to archive (delete after archiving)\n"
        << "  p             Print file to stdout\n"
        << "  r             Repair damaged archive\n"
        << "  rr[N]         Add data recovery record\n"
        << "  s             Convert archive to SFX\n"
        << "  t             Test archive integrity\n"
        << "  u             Update files in archive\n"
        << "  x             Extract files with full paths\n\n"
        << "<Switches>\n"
        << "  @<list>       Read file names from list file (UTF-8, ignores ;#// comments)\n"
        << "  -ed           Do not store directory records\n"
        << "  -ep           Exclude paths from names\n"
        << "  -hp<p>        Encrypt both file data and headers\n"
        << "  -m<0..5>      Set compression level (0-store...3-default...5-maximal)\n"
        << "  -mc<par>      Set compression pre-processing filters (e.g. -mc-, -mcE+, -mcD+)\n"
        << "  -md<size>     Accepted and validated (128k..1T); dictionary size is\n"
        << "                auto-selected per entry\n"
        << "  -mt<n>        Worker threads for batch add (default: all cores; -mt0 = auto)\n"
        << "  -oh           Save hard links as the link instead of the file\n"
        << "  -ol           Save symbolic links as the link instead of the file\n"
        << "  -o+ / -o-     Overwrite all existing files / never overwrite (default: ask)\n"
        << "  -os, -ow      Save NTFS streams / File security data (Windows ACLs, POSIX "
           "owner/group/mode)\n"
        << "  -p<p>         Set password\n"
        << "  -plain, --plain\n"
        << "                Plain line-by-line output (disable ANSI animations)\n"
        << "  -q, -quiet, --quiet\n"
        << "                Quiet mode (suppress informational output)\n"
        << "  -r            Recurse subdirectories\n"
        << "  -rr[N]        Add data recovery record (percentage)\n"
        << "  -s            Create solid archive\n"
        << "  -sfx          Create SFX archive\n"
        << "  -ts<m|c|a>    Time fields to store: m=modified, c=created, a=accessed\n"
        << "                (letters combine; default -tsm)\n"
        << "  -v<size>      Create multi-volume archive\n"
        << "  -ver[n]       File version control\n"
        << "  -x<pattern>   Exclude specified file or wildcard\n"
        << "  -x@<list>     Exclude files in specified list file\n"
        << "  -y            Assume Yes on all queries\n"
        << "  -z<file>      Read archive comment from file\n"
        << "  --version     Print version information and exit\n";
}

int list_archive(const std::string& arc_path, bool bare, bool technical,
                 const std::string& password = "",
                 const std::vector<std::string>& exclude_patterns = {},
                 const std::vector<std::string>& file_masks = {}) {
    // INFO8: quiet mode only suppresses OUTPUT. The archive must still be
    // opened and validated here so a quiet list of a missing archive exits
    // nonzero instead of reporting success without ever touching the archive.
    archive::ArchiveReader reader;
    int open_status = archive::RAR_OK;
    std::string open_detail;
    if (!reader.open_ex(arc_path, password, open_status, open_detail)) {
        // Oracle-mapped taxonomy (unrar errhnd.hpp): missing archive =
        // 10 (NO_FILES), unrecognized = 13 (BADARC), password = 11.
        if (!std::filesystem::exists(arc_path)) {
            std::cerr << "Cannot open " << arc_path << "\n";
            return EXIT_NO_FILES;
        }
        if (open_status == archive::RAR_ERR_ENCRYPTED ||
            open_status == archive::RAR_ERR_BAD_PASSWORD) {
            std::cerr << "Cannot decrypt: BADPSW (bad password)\n";
            return EXIT_BAD_PASSWORD;
        }
        if (open_status == archive::RAR_ERR_NOT_RAR) {
            std::cerr << "Not a RAR archive: " << arc_path << "\n";
            return EXIT_BAD_ARCHIVE;
        }
        std::cerr << "Cannot open " << arc_path << "\n";
        return EXIT_OPEN;
    }

    if (!g_quiet_mode) {
        if (!bare) {
            std::cout << "Archive: " << arc_path << "\n"
                      << "Details: RAR 5.0" << (reader.is_solid() ? ", solid" : "")
                      << (reader.is_locked() ? ", locked" : "")
                      << (reader.is_volume() ? ", volume" : "")
                      << (reader.has_recovery_record() ? ", recovery record" : "") << "\n\n";
            std::string cmt_text;
            for (const auto& entry : reader.entries()) {
                if (entry.header.is_service && entry.header.service_type == "CMT") {
                    if (!entry.header.sub_data.empty()) {
                        cmt_text.assign(reinterpret_cast<const char*>(entry.header.sub_data.data()),
                                        entry.header.sub_data.size());
                    } else if (entry.data_size > 0 && entry.data_size <= 1024 * 1024) {
                        std::vector<char> cbuf(static_cast<size_t>(entry.data_size));
                        auto& s = reader.stream();
                        if (s.seek(static_cast<core::int64>(entry.data_offset),
                                   io::SeekOrigin::Begin) &&
                            s.read(reinterpret_cast<core::byte*>(cbuf.data()), cbuf.size()) ==
                                cbuf.size()) {
                            cmt_text.assign(cbuf.data(), cbuf.size());
                        }
                    }
                    break;
                }
            }
            if (!cmt_text.empty()) {
                std::cout << "Comment:\n" << cmt_text << "\n\n";
            }
            if (!technical) {
                std::cout << " Attributes      Size     Date     Time   Name\n"
                          << "-----------  --------  ---------- -----  ----\n";
            }
        }

        for (const auto& entry : reader.entries()) {
            if (entry.header.is_service) continue;
            if (is_path_excluded(entry.header.file_name, exclude_patterns)) continue;
            // File-mask support (v1.21.2): `l`/`lb`/`lt` previously ignored
            // mask arguments entirely.
            if (!file_masks.empty()) {
                bool matched = false;
                for (const auto& mask : file_masks) {
                    if (io::wildcard_match(mask, entry.header.file_name) ||
                        io::wildcard_match(
                            mask,
                            std::filesystem::path(entry.header.file_name).filename().string())) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
            }
            std::string disp_name = entry.header.file_name;
            if (entry.header.has_file_version) {
                disp_name += ";" + std::to_string(entry.header.file_version);
            }
            if (bare) {
                std::cout << sanitize_for_display(disp_name) << "\n";
            } else if (technical) {
                std::cout << "  File:        " << sanitize_for_display(disp_name) << "\n"
                          << "  Size:        " << entry.header.unp_size << "\n"
                          << "  Packed:      " << entry.header.pack_size << "\n"
                          << "  Method:      " << entry.header.method << "\n"
                          << "  CRC32:       " << std::hex << entry.header.data_crc32 << std::dec
                          << "\n";
                if (entry.header.win_size > 0) {
                    if (entry.header.win_size >= 1024 * 1024 &&
                        (entry.header.win_size % (1024 * 1024) == 0)) {
                        std::cout << "  Dictionary:  " << (entry.header.win_size / (1024 * 1024))
                                  << " MB\n";
                    } else if (entry.header.win_size >= 1024 &&
                               (entry.header.win_size % 1024 == 0)) {
                        std::cout << "  Dictionary:  " << (entry.header.win_size / 1024) << " KB\n";
                    } else {
                        std::cout << "  Dictionary:  " << entry.header.win_size << " bytes\n";
                    }
                }
                if (entry.header.has_file_version) {
                    std::cout << "  File version: " << entry.header.file_version << "\n";
                }
                if (entry.header.has_owner) {
                    if (!entry.header.owner_user.empty() || !entry.header.owner_group.empty()) {
                        std::cout << "  User/Group:  " << entry.header.owner_user << " / "
                                  << entry.header.owner_group << "\n";
                    }
                    if (entry.header.has_owner_uid || entry.header.has_owner_gid) {
                        std::cout << "  UID/GID:     " << entry.header.owner_uid << " / "
                                  << entry.header.owner_gid << "\n";
                    }
                }
                std::cout << "\n";
            } else {
                std::cout << "    ..A....  " << entry.header.unp_size << "  "
                          << sanitize_for_display(disp_name) << "\n";
            }
        }
    }

    return 0;
}

static CLIProgress Prog;

// — parallel entry processing for x/e/t —
//
// Entries of a non-solid archive decode independently, so extraction and
// testing can run on the worker pool. Two pieces make that safe:
//
//   ReaderSlots — one independently opened ArchiveReader per concurrent slot,
//   borrowed by jobs. stream_ / solid-chain state never cross threads, and
//   each archive pays its header scan once per slot rather than once per
//   entry.
//
//   EntryFlags — per-entry completion flags. Workers decode out of order;
//   the main thread waits in archive order for the per-file console lines so
//   output looks exactly like the sequential run.

struct EntryFlags {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<char> done, ok;

    void resize(size_t n) {
        done.assign(n, 0);
        ok.assign(n, 0);
    }
    void finish(size_t i, bool okv) {
        {
            std::lock_guard<std::mutex> lk(mu);
            done[i] = 1;
            ok[i] = okv ? 1 : 0;
        }
        cv.notify_all();
    }
    bool wait(size_t i) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return done[i] != 0; });
        return ok[i] != 0;
    }
};

// Extraction target identity key. Windows filesystems are case-insensitive,
// so "ReadMe.txt" and "readme.txt" denote the same physical file: the
// duplicate-target guard must treat case-variant targets as identical, or
// two parallel extraction jobs race on the same path (v1.21.1 fix).
// ASCII-only folding covers the collisions that matter in practice.
static std::string target_key(const std::filesystem::path& p) {
    std::string s = p.string();
#ifdef _WIN32
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

struct ReaderSlots {
    std::vector<std::unique_ptr<archive::ArchiveReader>> readers;
    std::vector<char> in_use;
    std::mutex mu;
    std::condition_variable cv;

    bool init(const std::string& arc_path, const std::string& password, size_t n) {
        readers.resize(n);
        in_use.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            readers[i] = std::make_unique<archive::ArchiveReader>();
            if (!readers[i]->open(arc_path, password)) return false;
        }
        return true;
    }
    size_t acquire() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return std::find(in_use.begin(), in_use.end(), 0) != in_use.end(); });
        size_t i = static_cast<size_t>(std::find(in_use.begin(), in_use.end(), 0) - in_use.begin());
        in_use[i] = 1;
        return i;
    }
    void release(size_t i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            in_use[i] = 0;
        }
        cv.notify_one();
    }
};

// Parallel decode is only safe when every entry decodes independently: no
// solid archive or solid-flagged entry (they share one LZ window in order),
// and no multi-volume set (chain state across volumes). Redirections/symlinks
// are handled cleanly by the Three-Phase extraction pipeline.
bool entries_independently_decodable(const archive::ArchiveReader& reader) {
    if (reader.is_solid() || reader.is_volume()) return false;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service) continue;
        if (e.header.is_solid) return false;
    }
    return true;
}

int test_archive(const std::string& arc_path, const std::string& password = "",
                 unsigned threads = 1, const std::vector<std::string>& exclude_patterns = {},
                 const std::vector<std::string>& file_masks = {}) {
    archive::ArchiveReader reader;
    int open_status = archive::RAR_OK;
    std::string open_detail;
    if (!reader.open_ex(arc_path, password, open_status, open_detail)) {
        // Oracle-mapped taxonomy (unrar errhnd.hpp): missing archive =
        // 10 (NO_FILES), unrecognized = 13 (BADARC), password = 11.
        if (!std::filesystem::exists(arc_path)) {
            std::cerr << "Cannot open " << arc_path << "\n";
            return EXIT_NO_FILES;
        }
        if (open_status == archive::RAR_ERR_ENCRYPTED ||
            open_status == archive::RAR_ERR_BAD_PASSWORD) {
            std::cerr << "Cannot decrypt: BADPSW (bad password)\n";
            return EXIT_BAD_PASSWORD;
        }
        if (open_status == archive::RAR_ERR_NOT_RAR) {
            std::cerr << "Not a RAR archive: " << arc_path << "\n";
            return EXIT_BAD_ARCHIVE;
        }
        std::cerr << "Cannot open " << arc_path << "\n";
        return EXIT_OPEN;
    }

    if (!g_quiet_mode && !is_vt_supported()) {
        std::cout << "Testing archive: " << arc_path << "\n\n";
    }

    size_t total_entries = 0;
    core::uint64 total_bytes = 0;
    for (const auto& entry : reader.entries()) {
        if (!entry.header.is_service &&
            !is_path_excluded(entry.header.file_name, exclude_patterns)) {
            total_entries++;
            total_bytes += entry.header.unp_size;
        }
    }

    Prog.init("TESTING", "\x1b[38;2;95;184;176m", "\x1b[48;2;19;37;35m");
    Prog.set_totals(total_entries, total_bytes);

    size_t error_count = 0;
    size_t idx = 0;

    // Jobs = non-service entries, decoded independently. The vector index is
    // kept alongside the pointer: encrypted entries verify through
    // test_entry_stream, which is index-addressed.
    struct TestJob {
        const archive::ArchiveEntry* entry;
        size_t index;
    };
    std::vector<TestJob> jobs;
    {
        size_t ei = 0;
        for (const auto& entry : reader.entries()) {
            if (!entry.header.is_service &&
                !is_path_excluded(entry.header.file_name, exclude_patterns)) {
                if (!file_masks.empty()) {
                    bool matched = false;
                    for (const auto& mask : file_masks) {
                        if (io::wildcard_match(mask, entry.header.file_name) ||
                            io::wildcard_match(mask, std::filesystem::path(entry.header.file_name)
                                                         .filename()
                                                         .string())) {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        ei++;
                        continue;
                    }
                }
                jobs.push_back({&entry, ei});
            }
            ei++;
        }
    }

    const bool want_parallel =
        threads > 1 && jobs.size() > 1 && entries_independently_decodable(reader);
    ReaderSlots slots;
    if (want_parallel && !slots.init(arc_path, password, std::min<size_t>(threads, jobs.size()))) {
        // A slot failed to open what the main reader already opened
        // successfully (transient IO). Stay sequential rather than fail.
        slots.readers.clear();
    }

    // test_entry cannot verify encrypted payloads (it takes no password and
    // must not guess one), so routing encrypted entries through it reported
    // OK without ever decoding them (fail-open). Both paths below verify
    // encrypted entries through test_entry_stream instead, which decodes with
    // the reader's password and applies the same hash policy as extraction
    // (including the tweaked-checksum exception, 0x0002). Without a password
    // there is nothing to verify against — the skip stays, but says so.
    // Three-state verdict for one entry. Fail-closed (v1.21.1): an encrypted
    // entry with no password can NOT be verified — it must be reported as a
    // skip and counted as an error, never as OK (test_entry returns true for
    // encrypted entries without decoding anything).
    enum class TestResult { Ok, Failed, SkippedNoPassword };
    auto test_one = [&](archive::ArchiveReader& r, const TestJob& job) -> TestResult {
        if (job.entry->header.is_encrypted && password.empty()) {
            return TestResult::SkippedNoPassword;
        }
        if (!job.entry->header.is_encrypted) {
            return r.test_entry(*job.entry) ? TestResult::Ok : TestResult::Failed;
        }
        archive::ReaderHooks hooks{};
        return r.test_entry_stream(job.index, hooks) == archive::RAR_OK ? TestResult::Ok
                                                                        : TestResult::Failed;
    };
    int skipped_count = 0;
    auto test_verdict_str = [](TestResult r) -> const char* {
        switch (r) {
        case TestResult::Ok:
            return "OK";
        case TestResult::Failed:
            return "FAILED";
        default:
            return "SKIPPED (encrypted - no password)";
        }
    };

    if (jobs.empty()) {
        // Oracle semantics: a command that matched no files exits 10.
        return EXIT_NO_FILES;
    }

    if (slots.readers.empty()) {
        for (const auto& job : jobs) {
            idx++;
            Prog.start_file(job.entry->header.file_name, idx);

            if (!g_quiet_mode && !is_vt_supported()) {
                std::cout << "Testing     " << sanitize_for_display(job.entry->header.file_name)
                          << "... ";
            }

            TestResult res = test_one(reader, job);
            if (res == TestResult::SkippedNoPassword) skipped_count++;
            if (!g_quiet_mode && !is_vt_supported()) std::cout << test_verdict_str(res) << "\n";
            if (res != TestResult::Ok) error_count++;
            Prog.update_bytes(job.entry->header.unp_size);
        }
    } else {
        EntryFlags flags;
        flags.resize(jobs.size());
        std::vector<TestResult> verdicts(jobs.size(), TestResult::Failed);
        ThreadPool pool(static_cast<unsigned>(slots.readers.size()));
        for (size_t i = 0; i < jobs.size(); ++i) {
            pool.submit([&slots, &flags, &verdicts, &skipped_count, &jobs, &test_one, i] {
                // test_entry/test_entry_stream decode untrusted archive data
                // and can throw (bad_alloc, filesystem errors); an escaping
                // exception would skip flags.finish and hang the reporting
                // loop, and pre-guard it also kept the process alive (H2).
                TestResult res = TestResult::Failed;
                size_t s = 0;
                bool acquired = false;
                try {
                    s = slots.acquire();
                    acquired = true;
                    res = test_one(*slots.readers[s], jobs[i]);
                } catch (...) {
                    res = TestResult::Failed;
                }
                if (acquired) slots.release(s);
                verdicts[i] = res;
                if (res == TestResult::SkippedNoPassword) skipped_count++;
                flags.finish(i, res == TestResult::Ok);
                try {
                    Prog.note_file_done(jobs[i].entry->header.file_name,
                                        jobs[i].entry->header.unp_size);
                } catch (...) {
                }
            });
        }
        // Report in archive order so console output matches the sequential run.
        for (size_t i = 0; i < jobs.size(); ++i) {
            const bool okv = flags.wait(i);
            if (!g_quiet_mode && !is_vt_supported()) {
                std::cout << "Testing     " << sanitize_for_display(jobs[i].entry->header.file_name)
                          << "... " << test_verdict_str(verdicts[i]) << "\n";
            }
            if (!okv) error_count++;
        }
    }

    Prog.done(total_entries, "tested", "", total_bytes, 0,
              error_count == 0 ? "all OK" : (std::to_string(error_count) + " errors"));
    if (error_count == 0) return EXIT_OK;
    // Taxonomy: unverified encrypted entries / BADPSW are password
    // failures (11); other verification failures are checksum- or
    // structure-level (3, matching WinRAR t on damaged archives).
    return skipped_count > 0 ? EXIT_BAD_PASSWORD : EXIT_CRC;
}

int delete_from_archive(const std::string& arc_path, const std::vector<std::string>& files) {
    if (files.empty()) {
        std::cerr << "No files specified for deletion\n";
        return EXIT_FATAL;
    }

    if (!archive::ArchiveMutator::delete_entries(arc_path, files)) {
        std::cerr << "Cannot delete files from archive (archive may be locked or volume)\n";
        return EXIT_FATAL;
    }

    if (!g_quiet_mode) {
        std::cout << "Deleted specified entries from " << arc_path << "\n";
    }
    return 0;
}

// One queued source file for batch add: the directory scan (command 'a') or
// the explicit file list (command 'm') produce this, the pipeline consumes it
// in order.
struct PendingFile {
    std::filesystem::path src_path;
    std::string entry_name;
    core::uint64 file_size{0};
    bool is_dir{false};
    bool is_symlink{false};
    bool is_dir_target{false};
    std::string symlink_target;
    bool is_hardlink{false};
    std::string hardlink_target;

    PendingFile() = default;
    PendingFile(std::filesystem::path src, std::string entry, core::uint64 sz, bool dir = false,
                bool symlink = false, bool dir_target = false, std::string target = {},
                bool hardlink = false, std::string htarget = {})
        : src_path(std::move(src)), entry_name(std::move(entry)), file_size(sz), is_dir(dir),
          is_symlink(symlink), is_dir_target(dir_target), symlink_target(std::move(target)),
          is_hardlink(hardlink), hardlink_target(std::move(htarget)) {}
};

static core::uint64 get_total_physical_memory() {
#if defined(_WIN32)
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        return static_cast<core::uint64>(mem_status.ullTotalPhys);
    }
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<core::uint64>(pages) * static_cast<core::uint64>(page_size);
    }
#endif
    return 4ULL * 1024ULL * 1024ULL * 1024ULL; // Safe fallback: 4 GiB
}

// Parallel batch add: prepare every file on the pool (read + CRC + compress +
// optional encrypt), then write the archive in queue order on the calling
// thread. write_batch_add's on_write hook blocks until that entry's prepare
// has finished, so output bytes match the serial path exactly while
// compression overlaps disk writes. Memory stays bounded by prepare_budget:
// a job may only begin once its bytes are available, and the charge is
// released when the entry's payload reaches the archive — preparing ahead of
// the writer can never accumulate the whole batch in RAM. Any prepare or
// write failure aborts before the archive is replaced (all-or-nothing).
// err_name (optional) receives the failing entry's name.
static int run_batch_add(const std::string& arc_path, const std::vector<PendingFile>& queue,
                         int method, const std::filesystem::path& sfx_stub,
                         const std::string& password, bool encrypt_headers, bool delete_source,
                         bool announce, unsigned threads, std::string* err_name = nullptr,
                         core::uint32 times_mask = archive::time_flags::MTIME, bool solid = false,
                         const std::vector<core::byte>* comment = nullptr, bool want_stm = false,
                         bool want_acl = false, bool want_qo = true, bool want_ams = false,
                         core::uint64 dict_size = 0, const compress::FilterConfig& filter_cfg = {},
                         int max_versions = -1, const std::string& default_group = "",
                         const std::string& default_user = "") {
    const core::uint64 total_ram = get_total_physical_memory();
    const core::uint64 prepare_budget = std::clamp<core::uint64>(
        total_ram / 4, 1ULL << 30, 32ULL * 1024ULL * 1024ULL * 1024ULL); // 25% of RAM, 1-32 GiB
    if (solid) {
        threads = 1;
    }

    // Dynamic concurrency throttling for large dictionaries:
    // If dictionary size W > 32 MiB, Compressor50's ~5W footprint requires clamping
    // max worker threads to prevent exceeding prepare_budget and exhausting RAM.
    core::uint64 eff_dict = 0;
    if (dict_size >= 1 && dict_size <= 15) {
        eff_dict = 0x20000ULL << (dict_size - 1);
    } else if (dict_size == 0) {
        eff_dict = compress::default_dict_size_for_method(static_cast<uint32_t>(method));
    } else {
        eff_dict = dict_size;
    }
    if (eff_dict > 32ULL * 1024ULL * 1024ULL) {
        core::uint64 per_thread_mem = (eff_dict * 5) + (6ULL * 1024ULL * 1024ULL);
        unsigned max_safe_threads =
            static_cast<unsigned>(std::max<core::uint64>(1, prepare_budget / per_thread_mem));
        threads = std::min(threads, max_safe_threads);
    }

    std::vector<archive::ArchiveMutator::PreparedAdd> prepared(queue.size());
    // entry_name/src_path are caller-owned identity fields: fill them before
    // any prepare job runs. prepare_add_* never writes them, so the writer
    // thread can read them while a prepare is still in flight without a data
    // race (sweep finding M4).
    for (size_t i = 0; i < queue.size(); ++i) {
        prepared[i].entry_name = queue[i].entry_name;
        prepared[i].src_path = queue[i].src_path;
    }

    // Single file: nothing to overlap, so skip the pool and pipeline entirely
    // (no thread spawn, no handoff) and prepare inline. Semantics match the
    // pipelined path: prepare failure reports the entry and writes nothing;
    // a write failure exits non-zero without a per-file line.
    if (queue.size() == 1) {
        bool okv = false;
        try {
            if (queue[0].is_hardlink)
                okv = archive::ArchiveMutator::prepare_add_hardlink(
                    queue[0].src_path, queue[0].entry_name, queue[0].hardlink_target, prepared[0],
                    times_mask, want_acl, default_group, default_user);
            else if (queue[0].is_symlink)
                okv = archive::ArchiveMutator::prepare_add_symlink(
                    queue[0].src_path, queue[0].entry_name, queue[0].symlink_target,
                    queue[0].is_dir_target, prepared[0], times_mask, want_acl, default_group,
                    default_user);
            else if (queue[0].is_dir)
                okv = archive::ArchiveMutator::prepare_add_dir(
                    queue[0].src_path, queue[0].entry_name, prepared[0], times_mask, want_acl,
                    default_group, default_user);
            else
                okv = archive::ArchiveMutator::prepare_add_file(
                    queue[0].src_path, queue[0].entry_name, method, password, prepared[0],
                    times_mask, dict_size, want_stm, want_acl, solid, /*direct_stream=*/true,
                    filter_cfg, default_group, default_user, /*threads=*/threads);
        } catch (...) {
            okv = false;
        }
        if (!okv) {
            if (err_name) *err_name = queue[0].entry_name;
            if (announce && !g_quiet_mode && !is_vt_supported()) {
                std::cout << "Adding    " << queue[0].entry_name << " ... FAILED\n";
            }
            return EXIT_FATAL;
        }
        prepared[0].delete_source = delete_source;
        Prog.note_file_done(queue[0].entry_name, queue[0].file_size);
        if (!archive::ArchiveMutator::write_batch_add(
                arc_path, prepared, sfx_stub, password, encrypt_headers, {}, solid,
                comment ? *comment : std::vector<core::byte>(), want_qo, want_ams, filter_cfg,
                max_versions)) {
            return EXIT_FATAL;
        }
        if (announce && !g_quiet_mode && !is_vt_supported()) {
            std::cout << "Adding    " << queue[0].entry_name << " ... OK\n";
        }
        return 0;
    }

    // Shared pipeline state. One mutex/cv pair drives both the FIFO admission
    // gate and completion tracking; every wait has an aborting escape so a
    // failed batch always drains without deadlock.
    struct Pipeline {
        std::mutex mu;
        std::condition_variable cv;
        size_t admit_head = 0; // FIFO: only this index may charge bytes
        core::uint64 budget = 0;
        std::vector<core::uint64> holds; // bytes charged per entry (0 = released)
        std::vector<char> done, ok;
        bool aborting = false;
    } pl;
    pl.budget = prepare_budget;
    pl.holds.assign(queue.size(), 0);
    pl.done.assign(queue.size(), 0);
    pl.ok.assign(queue.size(), 0);

    // Build compression and execution plan to determine per-entry workspace estimates.
    std::vector<compress::EntryPlan> plan_reqs;
    plan_reqs.reserve(queue.size());
    for (const auto& q : queue) {
        compress::EntryPlan ep;
        ep.is_dir = q.is_dir || q.is_symlink || q.is_hardlink;
        ep.method = ep.is_dir ? 0 : static_cast<uint32_t>(method);
        ep.raw_size = q.file_size;
        plan_reqs.push_back(ep);
    }
    auto comp_plan = compress::CompressPlan::plan_entries(plan_reqs, solid, method, eff_dict);
    auto exec_plan = compress::ExecutionPlan::from_compress_plan(comp_plan);

    ThreadPool pool(threads);

    for (size_t i = 0; i < queue.size(); ++i) {
        const core::uint64 est_ws = (i < exec_plan.entries.size())
                                        ? exec_plan.entries[i].estimated_workspace_bytes
                                        : queue[i].file_size;
        pool.submit([&pl, &queue, &prepared, i, method, &password, delete_source, times_mask,
                     want_stm, want_acl, est_ws, budget_bytes = prepare_budget, dict_size, solid,
                     &filter_cfg, &default_group, &default_user] {
            std::unique_lock<std::mutex> lk(pl.mu);
            pl.cv.wait(lk, [&] { return pl.aborting || pl.admit_head == i; });
            if (pl.aborting) {
                pl.done[i] = 1;
            } else {
                // Record the workspace charge before acquiring so an aborting writer can
                // always find (and release) it; a 1-byte minimum keeps empty
                // files inside the budget without a special case.
                const core::uint64 hold =
                    std::max<core::uint64>(1, std::min<core::uint64>(est_ws, budget_bytes));
                pl.holds[i] = hold;
                pl.cv.wait(lk, [&] { return pl.aborting || pl.budget >= hold; });
                if (pl.aborting) {
                    pl.holds[i] = 0;
                    pl.done[i] = 1;
                } else {
                    pl.budget -= hold;
                    pl.admit_head = i + 1;
                    lk.unlock();
                    pl.cv.notify_all();

                    bool okv = false;
                    try {
                        if (queue[i].is_hardlink)
                            okv = archive::ArchiveMutator::prepare_add_hardlink(
                                queue[i].src_path, queue[i].entry_name, queue[i].hardlink_target,
                                prepared[i], times_mask, want_acl, default_group, default_user);
                        else if (queue[i].is_symlink)
                            okv = archive::ArchiveMutator::prepare_add_symlink(
                                queue[i].src_path, queue[i].entry_name, queue[i].symlink_target,
                                queue[i].is_dir_target, prepared[i], times_mask, want_acl,
                                default_group, default_user);
                        else if (queue[i].is_dir)
                            okv = archive::ArchiveMutator::prepare_add_dir(
                                queue[i].src_path, queue[i].entry_name, prepared[i], times_mask,
                                want_acl, default_group, default_user);
                        else
                            okv = archive::ArchiveMutator::prepare_add_file(
                                queue[i].src_path, queue[i].entry_name, method, password,
                                prepared[i], times_mask, dict_size, want_stm, want_acl, solid,
                                /*direct_stream=*/false, filter_cfg, default_group, default_user,
                                /*threads=*/1);
                    } catch (...) {
                        // std::filesystem throws on sources that vanish or
                        // become unreadable after the scan; same handling as
                        // a plain false return.
                        okv = false;
                    }
                    if (okv) {
                        prepared[i].delete_source = delete_source;
                        Prog.note_file_done(queue[i].entry_name, queue[i].file_size);
                    }

                    lk.lock();
                    pl.ok[i] = okv ? 1 : 0;
                    pl.done[i] = 1;
                }
            }
            lk.unlock();
            pl.cv.notify_all();
        });
    }

    // Writer-side abort signal: thrown out of on_write, through
    // write_batch_add (which removes its tmp file), caught below.
    struct PrepareFailed {
        size_t index;
    };
    size_t failed_index = queue.size();
    auto on_write = [&](size_t i, const std::string&) {
        std::unique_lock<std::mutex> lk(pl.mu);
        pl.cv.wait(lk, [&] { return pl.done[i] || pl.aborting; });
        if (pl.aborting) {
            failed_index = std::min(failed_index, i);
            throw PrepareFailed{i};
        }
        if (!pl.ok[i]) {
            pl.aborting = true;
            failed_index = i;
            throw PrepareFailed{i};
        }
        // The entry leaves the in-flight set as its payload hits the disk.
        pl.budget += pl.holds[i];
        pl.holds[i] = 0;
        lk.unlock();
        pl.cv.notify_all();
    };

    // Drain: release every remaining charge so blocked admissions finish, and
    // wait until all jobs have reached done[] before the pool joins. Runs on
    // EVERY exit path — a non-PrepareFailed exception from write_batch_add
    // must also drain, or workers parked on the admission/budget waits never
    // finish and ~ThreadPool hangs in join() forever (sweep finding M3).
    auto drain_pipeline = [&pl] {
        {
            std::lock_guard<std::mutex> lk(pl.mu);
            pl.aborting = true;
            for (auto& h : pl.holds) {
                pl.budget += h;
                h = 0;
            }
        }
        pl.cv.notify_all();
        std::unique_lock<std::mutex> lk(pl.mu);
        pl.cv.wait(lk, [&] {
            for (char d : pl.done)
                if (!d) return false;
            return true;
        });
    };

    bool ok = false;
    try {
        ok = archive::ArchiveMutator::write_batch_add(
            arc_path, prepared, sfx_stub, password, encrypt_headers, on_write, solid,
            comment ? *comment : std::vector<core::byte>(), want_qo, want_ams, filter_cfg,
            max_versions);
    } catch (const PrepareFailed&) {
        ok = false;
    } catch (...) {
        drain_pipeline();
        throw;
    }
    drain_pipeline();

    if (!ok) {
        if (failed_index < queue.size() && err_name) *err_name = queue[failed_index].entry_name;
        if (announce && !g_quiet_mode && !is_vt_supported() && failed_index < queue.size()) {
            std::cout << "Adding    " << queue[failed_index].entry_name << " ... FAILED\n";
        }
        return EXIT_FATAL;
    }
    if (announce && !g_quiet_mode && !is_vt_supported()) {
        for (const auto& item : queue) {
            std::cout << "Adding    " << item.entry_name << " ... OK\n";
        }
    }
    return 0;
}

int add_to_archive(const std::string& arc_path, const std::vector<std::string>& files,
                   int method = 3, const std::filesystem::path& sfx_stub = {},
                   ::openrar::core::uint64 vol_size = 0, const std::string& password = "",
                   bool encrypt_headers = false, unsigned threads = 1, bool solid = false,
                   const std::vector<core::byte>& comment = {},
                   core::uint32 times_mask = archive::time_flags::MTIME,
                   bool no_dir_records = false,
                   io::ExcludePathMode ep_mode = io::ExcludePathMode::None,
                   bool recurse_subdirs = true, bool want_symlinks = false, bool freshen = false,
                   bool want_stm = false, bool want_acl = false, bool want_hardlinks = false,
                   bool want_qo = true, bool want_ams = false,
                   const std::vector<std::string>& exclude_patterns = {},
                   core::uint64 dict_size = 0, const compress::FilterConfig& filter_cfg = {},
                   int max_versions = -1, const std::string& default_group = "",
                   const std::string& default_user = "", bool want_lock = false) {
    if (files.empty()) {
        std::cerr << "No files specified for addition\n";
        return EXIT_FATAL;
    }

    std::unordered_map<std::string, core::uint64> existing_files;
    if (freshen) {
        if (!std::filesystem::exists(arc_path)) return 0;
        archive::ArchiveReader r;
        if (!r.open(arc_path)) return EXIT_FATAL;
        for (const auto& entry : r.entries()) {
            if (!entry.header.is_service) {
                existing_files[entry.header.file_name] = entry.header.utime_unix;
            }
        }
    }

    auto should_include = [&](const std::string& rel_name,
                              const std::filesystem::path& disk_path) -> bool {
        if (is_path_excluded(rel_name, exclude_patterns)) return false;
        if (!freshen) return true;
        auto it = existing_files.find(rel_name);
        if (it == existing_files.end()) return false;
        core::uint64 disk_mtime = 0;
        if (archive::ArchiveMutator::get_file_mtime(disk_path, disk_mtime)) {
            if (disk_mtime <= it->second) return false;
        }
        return true;
    };

    std::vector<PendingFile> queue;
    core::uint64 total_unp = 0;
    // Multi-volume writes have no directory-record support yet (file-centric
    // slicing); queue files only and say so.
    const bool volume_add = vol_size != 0;
    size_t skipped_dirs = 0;

    Prog.init(method == 0 ? "STORING" : "COMPRESSING",
              method == 0 ? "\x1b[38;2;139;147;216m" : "\x1b[38;2;224;164;88m",
              method == 0 ? "\x1b[48;2;28;30;46m" : "\x1b[48;2;45;33;18m");

    for (const auto& f : files) {
        std::filesystem::path p(f);
        std::string filename_str = p.filename().string();
        if (filename_str.find('*') != std::string::npos ||
            filename_str.find('?') != std::string::npos) {
            std::filesystem::path parent_dir =
                p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
            std::string parent_str = parent_dir.string();
            if (parent_str.find('*') != std::string::npos ||
                parent_str.find('?') != std::string::npos) {
                std::cerr << "Error: Wildcard in directory component '" << f
                          << "' is unsupported without recursion.\n";
                continue;
            }
            std::error_code it_ec;
            if (recurse_subdirs) {
                for (const auto& dir_entry :
                     std::filesystem::recursive_directory_iterator(parent_dir, it_ec)) {
                    if (it_ec) break;
                    std::error_code stat_ec;
                    if (dir_entry.is_symlink(stat_ec)) {
                        if (want_symlinks) {
                            std::error_code link_ec;
                            std::filesystem::path target =
                                std::filesystem::read_symlink(dir_entry.path(), link_ec);
                            if (!link_ec) {
                                std::string target_str = target.generic_string();
                                bool is_dir_target =
                                    std::filesystem::is_directory(dir_entry.path(), link_ec);
                                std::string rel = (ep_mode != io::ExcludePathMode::None)
                                                      ? io::format_archive_path(
                                                            dir_entry.path().generic_string(),
                                                            parent_dir.generic_string(), ep_mode)
                                                      : dir_entry.path()
                                                            .lexically_relative(p.parent_path())
                                                            .generic_string();
                                if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                                if (!rel.empty() && should_include(rel, dir_entry.path())) {
                                    PendingFile pf;
                                    pf.src_path = dir_entry.path();
                                    pf.entry_name = rel;
                                    pf.file_size = 0;
                                    pf.is_symlink = true;
                                    pf.is_dir_target = is_dir_target;
                                    pf.symlink_target = target_str;
                                    queue.push_back(std::move(pf));
                                    Prog.spin("Scanning files…", queue.size());
                                }
                            }
                        }
                        continue;
                    }
                    if (!dir_entry.is_regular_file(stat_ec)) continue;
                    if (openrar::io::wildcard_match(filename_str,
                                                    dir_entry.path().filename().string(), false)) {
                        core::uint64 sz = dir_entry.file_size(stat_ec);
                        if (stat_ec) continue;
                        std::string rel =
                            (ep_mode != io::ExcludePathMode::None)
                                ? io::format_archive_path(dir_entry.path().generic_string(),
                                                          parent_dir.generic_string(), ep_mode)
                                : dir_entry.path()
                                      .lexically_relative(p.parent_path())
                                      .generic_string();
                        if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                        if (rel.empty()) continue;
                        if (!should_include(rel, dir_entry.path())) continue;
                        queue.push_back({dir_entry.path(), rel, sz});
                        total_unp += sz;
                        Prog.spin("Scanning files…", queue.size());
                    }
                }
            } else {
                for (const auto& dir_entry :
                     std::filesystem::directory_iterator(parent_dir, it_ec)) {
                    if (it_ec) break;
                    std::error_code stat_ec;
                    if (dir_entry.is_symlink(stat_ec)) {
                        if (want_symlinks) {
                            std::error_code link_ec;
                            std::filesystem::path target =
                                std::filesystem::read_symlink(dir_entry.path(), link_ec);
                            if (!link_ec) {
                                std::string target_str = target.generic_string();
                                bool is_dir_target =
                                    std::filesystem::is_directory(dir_entry.path(), link_ec);
                                std::string rel =
                                    (ep_mode != io::ExcludePathMode::None)
                                        ? io::format_archive_path(dir_entry.path().generic_string(),
                                                                  parent_dir.generic_string(),
                                                                  ep_mode)
                                        : dir_entry.path().filename().generic_string();
                                if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                                if (!rel.empty() && should_include(rel, dir_entry.path())) {
                                    PendingFile pf;
                                    pf.src_path = dir_entry.path();
                                    pf.entry_name = rel;
                                    pf.file_size = 0;
                                    pf.is_symlink = true;
                                    pf.is_dir_target = is_dir_target;
                                    pf.symlink_target = target_str;
                                    queue.push_back(std::move(pf));
                                    Prog.spin("Scanning files…", queue.size());
                                }
                            }
                        }
                        continue;
                    }
                    if (!dir_entry.is_regular_file(stat_ec)) continue;
                    if (openrar::io::wildcard_match(filename_str,
                                                    dir_entry.path().filename().string(), false)) {
                        core::uint64 sz = dir_entry.file_size(stat_ec);
                        if (stat_ec) continue;
                        std::string rel =
                            (ep_mode != io::ExcludePathMode::None)
                                ? io::format_archive_path(dir_entry.path().generic_string(),
                                                          parent_dir.generic_string(), ep_mode)
                                : dir_entry.path().filename().generic_string();
                        if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                        if (rel.empty()) continue;
                        if (!should_include(rel, dir_entry.path())) continue;
                        queue.push_back({dir_entry.path(), rel, sz});
                        total_unp += sz;
                        Prog.spin("Scanning files…", queue.size());
                    }
                }
            }
            continue;
        }
        if (std::filesystem::is_directory(p)) {
            std::error_code it_ec;
            auto scan_dir_entry = [&](const std::filesystem::directory_entry& dir_entry) {
                std::error_code stat_ec;
                if (dir_entry.is_symlink(stat_ec)) {
                    if (want_symlinks) {
                        std::error_code link_ec;
                        std::filesystem::path target =
                            std::filesystem::read_symlink(dir_entry.path(), link_ec);
                        if (!link_ec) {
                            std::string target_str = target.generic_string();
                            bool is_dir_target =
                                std::filesystem::is_directory(dir_entry.path(), link_ec);
                            std::string rel = (ep_mode != io::ExcludePathMode::None)
                                                  ? io::format_archive_path(
                                                        dir_entry.path().generic_string(),
                                                        p.parent_path().generic_string(), ep_mode)
                                                  : dir_entry.path()
                                                        .lexically_relative(p.parent_path())
                                                        .generic_string();
                            if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                            if (!rel.empty() && should_include(rel, dir_entry.path())) {
                                PendingFile pf;
                                pf.src_path = dir_entry.path();
                                pf.entry_name = rel;
                                pf.file_size = 0;
                                pf.is_symlink = true;
                                pf.is_dir_target = is_dir_target;
                                pf.symlink_target = target_str;
                                queue.push_back(std::move(pf));
                                Prog.spin("Scanning files…", queue.size());
                            }
                        }
                    }
                    return;
                }
                bool is_dir = dir_entry.is_directory(stat_ec);
                if (stat_ec) return;
                std::string rel =
                    (ep_mode != io::ExcludePathMode::None)
                        ? io::format_archive_path(dir_entry.path().generic_string(),
                                                  p.parent_path().generic_string(), ep_mode)
                        : dir_entry.path().lexically_relative(p.parent_path()).generic_string();
                if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
                if (rel.empty()) return;
                if (!should_include(rel, dir_entry.path())) return;
                if (is_dir) {
                    if (volume_add || no_dir_records) {
                        skipped_dirs++;
                        return;
                    }
                    queue.push_back({dir_entry.path(), rel, 0, true});
                } else {
                    if (!dir_entry.is_regular_file(stat_ec)) return;
                    core::uint64 sz = dir_entry.file_size(stat_ec);
                    if (stat_ec) return;
                    queue.push_back({dir_entry.path(), rel, sz});
                    total_unp += sz;
                }
                Prog.spin("Scanning files…", queue.size());
            };
            if (recurse_subdirs) {
                for (const auto& dir_entry :
                     std::filesystem::recursive_directory_iterator(p, it_ec)) {
                    if (it_ec) break;
                    scan_dir_entry(dir_entry);
                }
            } else {
                for (const auto& dir_entry : std::filesystem::directory_iterator(p, it_ec)) {
                    if (it_ec) break;
                    scan_dir_entry(dir_entry);
                }
            }
        } else if (std::filesystem::exists(p)) {
            // L10: same vanish-between-exists-and-stat race as above.
            std::error_code stat_ec;
            if (std::filesystem::is_symlink(p, stat_ec)) {
                if (want_symlinks) {
                    std::filesystem::path target = std::filesystem::read_symlink(p, stat_ec);
                    if (!stat_ec) {
                        std::string target_str = target.generic_string();
                        bool is_dir_target = std::filesystem::is_directory(p, stat_ec);
                        std::string entry =
                            (ep_mode != io::ExcludePathMode::None)
                                ? io::format_archive_path(p.generic_string(),
                                                          p.parent_path().generic_string(), ep_mode)
                                : p.filename().generic_string();
                        if (should_include(entry, p)) {
                            PendingFile pf;
                            pf.src_path = p;
                            pf.entry_name = entry;
                            pf.file_size = 0;
                            pf.is_symlink = true;
                            pf.is_dir_target = is_dir_target;
                            pf.symlink_target = target_str;
                            queue.push_back(std::move(pf));
                            Prog.spin("Scanning files…", queue.size());
                        }
                    }
                }
                continue;
            }
            core::uint64 sz = std::filesystem::file_size(p, stat_ec);
            if (stat_ec) {
                std::cerr << "W: cannot stat " << p.string() << ", skipping\n";
                continue;
            }
            std::string entry =
                (ep_mode != io::ExcludePathMode::None)
                    ? io::format_archive_path(p.generic_string(), p.parent_path().generic_string(),
                                              ep_mode)
                    : p.filename().generic_string();
            if (should_include(entry, p)) {
                queue.push_back({p, entry, sz});
                total_unp += sz;
                Prog.spin("Scanning files…", queue.size());
            }
        }
    }
    if (skipped_dirs > 0 && !g_quiet_mode) {
        std::cout << "W: " << skipped_dirs
                  << " director"
                     "y records not stored (multi-volume mode does not store directory records "
                     "yet)\n";
    }
    if (!exclude_patterns.empty()) {
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [&](const PendingFile& pf) {
                                       return is_path_excluded(pf.entry_name, exclude_patterns);
                                   }),
                    queue.end());
    }

    if (queue.empty()) {
        if (freshen) return 0;
        std::cerr << "No files found to add\n";
        return EXIT_FATAL;
    }

    std::stable_sort(queue.begin(), queue.end(), [](const PendingFile& a, const PendingFile& b) {
        return a.entry_name < b.entry_name;
    });

    if (want_hardlinks) {
#ifdef _WIN32
        struct WinFileId {
            FILE_ID_128 id128{};
            DWORD vol_serial{0};
            bool is_128{false};
            DWORD file_idx_high{0};
            DWORD file_idx_low{0};

            bool operator==(const WinFileId& o) const {
                if (vol_serial != o.vol_serial) return false;
                if (is_128 && o.is_128) {
                    return std::memcmp(&id128, &o.id128, sizeof(id128)) == 0;
                }
                return file_idx_high == o.file_idx_high && file_idx_low == o.file_idx_low;
            }
        };
        struct WinFileIdHash {
            size_t operator()(const WinFileId& k) const {
                if (k.is_128) {
                    uint64_t low64 = 0, high64 = 0;
                    std::memcpy(&low64, k.id128.Identifier, 8);
                    std::memcpy(&high64, k.id128.Identifier + 8, 8);
                    return std::hash<uint64_t>()(low64) ^ (std::hash<uint64_t>()(high64) << 1) ^
                           (std::hash<uint32_t>()(k.vol_serial) << 2);
                }
                return (static_cast<size_t>(k.file_idx_high) << 32) ^ k.file_idx_low ^ k.vol_serial;
            }
        };
        std::unordered_map<WinFileId, std::string, WinFileIdHash> seen_files;
        for (auto& item : queue) {
            if (item.is_dir || item.is_symlink) continue;
            HANDLE h = CreateFileW(item.src_path.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FILE_ID_INFO id_info{};
                BY_HANDLE_FILE_INFORMATION bhfi{};
                bool got_id = false;
                WinFileId fid{};
                DWORD nlinks = 1;

                if (GetFileInformationByHandle(h, &bhfi)) {
                    nlinks = bhfi.nNumberOfLinks;
                    fid.vol_serial = bhfi.dwVolumeSerialNumber;
                    fid.file_idx_high = bhfi.nFileIndexHigh;
                    fid.file_idx_low = bhfi.nFileIndexLow;
                    fid.is_128 = false;
                    got_id = true;
                }
                if (GetFileInformationByHandleEx(h, FileIdInfo, &id_info, sizeof(id_info))) {
                    fid.vol_serial = static_cast<DWORD>(id_info.VolumeSerialNumber);
                    fid.id128 = id_info.FileId;
                    fid.is_128 = true;
                    got_id = true;
                }
                CloseHandle(h);

                if (got_id && nlinks > 1) {
                    auto it = seen_files.find(fid);
                    if (it == seen_files.end()) {
                        seen_files[fid] = item.entry_name;
                    } else {
                        item.is_hardlink = true;
                        item.hardlink_target = it->second;
                        if (total_unp >= item.file_size) {
                            total_unp -= item.file_size;
                        }
                        item.file_size = 0;
                    }
                }
            }
        }
#else
        struct DevIno {
            dev_t dev;
            ino_t ino;
            bool operator==(const DevIno& o) const { return dev == o.dev && ino == o.ino; }
        };
        struct DevInoHash {
            size_t operator()(const DevIno& k) const {
                return std::hash<uint64_t>()(static_cast<uint64_t>(k.dev)) ^
                       (std::hash<uint64_t>()(static_cast<uint64_t>(k.ino)) << 1);
            }
        };
        std::unordered_map<DevIno, std::string, DevInoHash> seen_files;
        for (auto& item : queue) {
            if (item.is_dir || item.is_symlink) continue;
            struct stat st;
            if (::lstat(item.src_path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_nlink > 1) {
                DevIno key{st.st_dev, st.st_ino};
                auto it = seen_files.find(key);
                if (it == seen_files.end()) {
                    seen_files[key] = item.entry_name;
                } else {
                    item.is_hardlink = true;
                    item.hardlink_target = it->second;
                    if (total_unp >= item.file_size) {
                        total_unp -= item.file_size;
                    }
                    item.file_size = 0;
                }
            }
        }
#endif
    }

    Prog.set_totals(queue.size(), total_unp);

    if (!g_quiet_mode && !is_vt_supported()) {
        std::cout << "Creating archive " << arc_path << "\n";
    }

    const bool volume_mode = (vol_size != 0 && vol_size != archive::volume::VOLSIZE_AUTO);
    if (volume_mode) {
        // Multi-volume: every add rewrites the whole chain (sidecar rotation,
        // extent rewriting), so preparation cannot be decoupled from writing;
        // stay sequential.
        for (size_t i = 0; i < queue.size(); ++i) {
            const auto& item = queue[i];
            Prog.start_file(item.entry_name, i + 1);

            if (!g_quiet_mode && !is_vt_supported()) {
                std::cout << "Adding    " << item.entry_name << " ... ";
            }
            bool ok = false;
            bool is_last = (i == queue.size() - 1);
            if (!sfx_stub.empty())
                ok = archive::ArchiveMutator::add_file_to_archive(
                    arc_path, item.src_path, item.entry_name, method, sfx_stub, vol_size, password,
                    encrypt_headers, solid, dict_size, filter_cfg);
            else
                ok = archive::ArchiveMutator::add_file_to_archive_vol(
                    arc_path, item.src_path, item.entry_name, method, vol_size, password, solid,
                    dict_size, filter_cfg, encrypt_headers, comment.empty() ? nullptr : &comment,
                    is_last ? want_lock : false);
            if (!ok) {
                if (!g_quiet_mode && !is_vt_supported()) std::cout << "FAILED\n";
                return EXIT_FATAL;
            }
            if (!g_quiet_mode && !is_vt_supported()) std::cout << "OK\n";
            Prog.update_bytes(item.file_size);
        }
    } else {
        int rc = run_batch_add(arc_path, queue, method, sfx_stub, password, encrypt_headers,
                               /*delete_source=*/false, /*announce=*/true, threads, nullptr,
                               times_mask, solid, comment.empty() ? nullptr : &comment, want_stm,
                               want_acl, want_qo, want_ams, dict_size, filter_cfg, max_versions,
                               default_group, default_user);
        if (rc != 0) return rc;
    }

    Prog.spin("Writing archive index…");

    core::uint64 final_arc_size = 0;
    if (vol_size != 0 && vol_size != archive::volume::VOLSIZE_AUTO) {
        auto firstVol = archive::volume::first_volume_name(arc_path);
        if (std::filesystem::exists(firstVol))
            final_arc_size = std::filesystem::file_size(firstVol);
        else if (std::filesystem::exists(arc_path))
            final_arc_size = std::filesystem::file_size(arc_path);
    } else {
        if (std::filesystem::exists(arc_path))
            final_arc_size = std::filesystem::file_size(arc_path);
    }

    Prog.done(queue.size(), "packed", arc_path, total_unp, final_arc_size);
    return 0;
}

int extract_archive(const std::string& arc_path, const std::string& dest_dir, bool full_paths,
                    const std::string& password = "", unsigned threads = 1,
                    bool keep_broken = false, OverwriteMode overwrite_mode = OverwriteMode::Prompt,
                    bool extract_symlinks = true,
                    const std::vector<std::string>& exclude_patterns = {}, int extract_version = -1,
                    const std::vector<std::string>& file_patterns = {},
                    [[maybe_unused]] bool restore_owner = false) {
    archive::ArchiveReader reader;
    reader.set_keep_broken(keep_broken);
    reader.set_extract_symlinks(extract_symlinks);
    int open_status = archive::RAR_OK;
    std::string open_detail;
    if (!reader.open_ex(arc_path, password, open_status, open_detail)) {
        // Oracle-mapped taxonomy (unrar errhnd.hpp): missing archive =
        // 10 (NO_FILES), unrecognized = 13 (BADARC), password = 11.
        if (!std::filesystem::exists(arc_path)) {
            std::cerr << "Cannot open " << arc_path << "\n";
            return EXIT_NO_FILES;
        }
        if (open_status == archive::RAR_ERR_ENCRYPTED ||
            open_status == archive::RAR_ERR_BAD_PASSWORD) {
            std::cerr << "Cannot decrypt: BADPSW (bad password)\n";
            return EXIT_BAD_PASSWORD;
        }
        if (open_status == archive::RAR_ERR_NOT_RAR) {
            std::cerr << "Not a RAR archive: " << arc_path << "\n";
            return EXIT_BAD_ARCHIVE;
        }
        std::cerr << "Cannot open " << arc_path << "\n";
        return EXIT_OPEN;
    }

    if (!g_quiet_mode && !is_vt_supported()) {
        std::cout << "Extracting from " << arc_path << "\n\n";
    }

    size_t total_entries = 0;
    core::uint64 total_bytes = 0;
    for (const auto& entry : reader.entries()) {
        if (entry.header.is_service || is_path_excluded(entry.header.file_name, exclude_patterns))
            continue;
        if (entry.header.has_file_version) {
            if (extract_version < 0) {
                bool explicit_version_match = false;
                for (const auto& pat : file_patterns) {
                    if (pat.find(';') != std::string::npos) {
                        std::string ver_name = entry.header.file_name + ";" +
                                               std::to_string(entry.header.file_version);
                        if (openrar::io::wildcard_match(pat, ver_name, false) ||
                            openrar::io::wildcard_match(
                                pat, std::filesystem::path(ver_name).filename().string(), false)) {
                            explicit_version_match = true;
                            break;
                        }
                    }
                }
                if (!explicit_version_match) continue;
            } else if (extract_version > 0) {
                if (entry.header.file_version != static_cast<core::uint64>(extract_version))
                    continue;
            }
        } else {
            if (extract_version > 0) continue;
        }

        if (!file_patterns.empty()) {
            bool matched = false;
            std::string ver_name = entry.header.file_name;
            if (entry.header.has_file_version) {
                ver_name += ";" + std::to_string(entry.header.file_version);
            }
            for (const auto& pat : file_patterns) {
                if (openrar::io::wildcard_match(pat, entry.header.file_name, false) ||
                    openrar::io::wildcard_match(pat, ver_name, false) ||
                    openrar::io::wildcard_match(
                        pat, std::filesystem::path(entry.header.file_name).filename().string(),
                        false) ||
                    openrar::io::wildcard_match(
                        pat, std::filesystem::path(ver_name).filename().string(), false)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
        }
        total_entries++;
        total_bytes += entry.header.unp_size;
    }

    Prog.init("DECOMPRESSING", "\x1b[38;2;95;184;176m", "\x1b[48;2;19;37;35m");
    Prog.set_totals(total_entries, total_bytes);

    std::filesystem::path out_root =
        dest_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(dest_dir);

    // Precompute every sanitized target up front: the parallel path must not
    // build paths per job, and duplicate targets (two entries landing on the
    // same file) would race their writers — those fall back to sequential.
    struct ExtractJob {
        const archive::ArchiveEntry* entry;
        std::filesystem::path target;
        std::string display_name;
        std::vector<const archive::ArchiveEntry*> children;
    };
    std::vector<ExtractJob> extract_jobs;
    bool duplicate_targets = false;
    {
        std::set<std::string> seen_targets;
        const auto& all_entries = reader.entries();
        for (size_t i = 0; i < all_entries.size(); ++i) {
            const auto& entry = all_entries[i];
            if (entry.header.is_service) continue;
            if (is_path_excluded(entry.header.file_name, exclude_patterns)) continue;

            // Version filtering
            if (entry.header.has_file_version) {
                if (extract_version < 0) {
                    bool explicit_version_match = false;
                    for (const auto& pat : file_patterns) {
                        if (pat.find(';') != std::string::npos) {
                            std::string ver_name = entry.header.file_name + ";" +
                                                   std::to_string(entry.header.file_version);
                            if (openrar::io::wildcard_match(pat, ver_name, false) ||
                                openrar::io::wildcard_match(
                                    pat, std::filesystem::path(ver_name).filename().string(),
                                    false)) {
                                explicit_version_match = true;
                                break;
                            }
                        }
                    }
                    if (!explicit_version_match) continue;
                } else if (extract_version > 0) {
                    if (entry.header.file_version != static_cast<core::uint64>(extract_version))
                        continue;
                }
            } else {
                if (extract_version > 0) continue;
            }

            if (!file_patterns.empty()) {
                bool matched = false;
                std::string ver_name = entry.header.file_name;
                if (entry.header.has_file_version) {
                    ver_name += ";" + std::to_string(entry.header.file_version);
                }
                for (const auto& pat : file_patterns) {
                    if (openrar::io::wildcard_match(pat, entry.header.file_name, false) ||
                        openrar::io::wildcard_match(pat, ver_name, false) ||
                        openrar::io::wildcard_match(
                            pat, std::filesystem::path(entry.header.file_name).filename().string(),
                            false) ||
                        openrar::io::wildcard_match(
                            pat, std::filesystem::path(ver_name).filename().string(), false)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
            }

            std::string safe_name = io::sanitize_archive_path(entry.header.file_name);
            if (safe_name.empty()) {
                std::cerr << "Skipping entry with unsafe empty path: "
                          << sanitize_for_display(entry.header.file_name) << "\n";
                continue;
            }
            std::string disk_name = safe_name;
            if (entry.header.has_file_version &&
                extract_version != static_cast<int>(entry.header.file_version)) {
                disk_name += ";" + std::to_string(entry.header.file_version);
            }
            std::filesystem::path target =
                full_paths ? (out_root / std::filesystem::path(disk_name))
                           : (out_root / std::filesystem::path(disk_name).filename());
            if (!io::is_lexically_contained(target, out_root)) {
                std::cerr << "Skipping entry escaping extraction directory: "
                          << sanitize_for_display(entry.header.file_name) << "\n";
                continue;
            }
            if (!seen_targets.insert(target_key(target)).second) duplicate_targets = true;
            std::vector<const archive::ArchiveEntry*> children;
            for (size_t j = i + 1; j < all_entries.size() && all_entries[j].header.is_service;
                 ++j) {
                if (all_entries[j].header.service_type == "STM" ||
                    all_entries[j].header.service_type == "ACL") {
                    children.push_back(&all_entries[j]);
                }
            }
            extract_jobs.push_back(
                {&entry, target, sanitize_for_display(target.string()), std::move(children)});
        }
        // Component-prefix overlap (sweep finding L11): a file entry "a"
        // alongside a directory entry "a/b" makes create_directories race the
        // file write when threaded. Walk every target's ancestor chain and
        // fall back to sequential if any ancestor is itself a target (covers
        // both orders of the pair).
        if (!duplicate_targets) {
            std::set<std::string> all_targets;
            for (const auto& j : extract_jobs) all_targets.insert(target_key(j.target));
            for (const auto& j : extract_jobs) {
                const std::filesystem::path& t = j.target;
                std::filesystem::path anc;
                for (auto it = t.begin(); it != t.end() && std::next(it) != t.end(); ++it) {
                    anc /= *it;
                    if (all_targets.count(target_key(anc))) {
                        duplicate_targets = true;
                        break;
                    }
                }
                if (duplicate_targets) break;
            }
        }
    }

    // -o- pre-filter: drop existing targets up front so BOTH the sequential
    // and the parallel path skip them. The parallel path has no per-file
    // decision point; without the filter `-o- -mt4` would still overwrite.
    if (overwrite_mode == OverwriteMode::SkipExisting) {
        std::vector<ExtractJob> kept;
        kept.reserve(extract_jobs.size());
        for (auto& job : extract_jobs) {
            std::error_code exists_ec;
            if (std::filesystem::exists(job.target, exists_ec)) {
                if (!g_quiet_mode && !is_vt_supported())
                    std::cout << "Extracting  " << job.display_name
                              << " ... SKIPPED (already exists)\n";
                continue;
            }
            kept.push_back(std::move(job));
        }
        extract_jobs.swap(kept);
    }

    // Prompt mode cannot ask inside the thread pool: if any target exists and
    // queries are not pre-answered (-y), run sequentially so each file can be
    // asked in archive order.
    bool needs_sequential_prompt = false;
    if (overwrite_mode == OverwriteMode::Prompt && !g_assume_yes) {
        std::error_code exists_ec;
        for (const auto& job : extract_jobs) {
            if (std::filesystem::exists(job.target, exists_ec)) {
                needs_sequential_prompt = true;
                break;
            }
        }
    }
    const bool want_parallel = threads > 1 && extract_jobs.size() > 1 && !duplicate_targets &&
                               !needs_sequential_prompt && entries_independently_decodable(reader);
    ReaderSlots slots;
    if (want_parallel &&
        !slots.init(arc_path, password, std::min<size_t>(threads, extract_jobs.size()))) {
        // A slot failed to open what the main reader already opened
        // successfully (transient IO). Stay sequential rather than fail.
        slots.readers.clear();
    }

    if (extract_jobs.empty()) {
        // Oracle semantics: a command that matched no files exits 10.@N        return EXIT_NO_FILES;
    }

    bool any_failed = false;
    std::atomic<int> badpw_flag{0};

    auto restore_children = [restore_owner](archive::ArchiveReader& r, const ExtractJob& j) {
        for (const auto* child : j.children) {
            if (child->header.service_type == "STM") {
                std::vector<core::byte> payload;
                if (r.read_packed_data(*child, payload)) {
                    if (child->header.has_crc32) {
                        crypto::Crc32 c;
                        c.update(payload.data(), payload.size());
                        if (c.get() != child->header.data_crc32) {
                            // Say so: a silently dropped ADS hides corruption
                            // from the user (v1.21.2).
                            std::cerr
                                << "W: checksum mismatch, skipping stream "
                                << sanitize_for_display(std::string(child->header.sub_data.begin(),
                                                                    child->header.sub_data.end()))
                                << " for " << j.display_name << "\n";
                            continue;
                        }
                    }
                    std::string sname(child->header.sub_data.begin(), child->header.sub_data.end());
                    if (!sname.empty() && sname[0] == ':') {
                        io::write_alternate_stream(j.target, sname, payload.data(), payload.size());
                    }
                }
            } else if (child->header.service_type == "ACL") {
                std::vector<core::byte> payload;
                if (r.read_packed_data(*child, payload)) {
                    if (child->header.has_crc32) {
                        crypto::Crc32 c;
                        c.update(payload.data(), payload.size());
                        if (c.get() != child->header.data_crc32) {
                            std::cerr << "W: checksum mismatch, skipping security descriptor for "
                                      << j.display_name << "\n";
                            continue;
                        }
                    }
                    io::write_security_descriptor(j.target, payload.data(), payload.size());
                }
            }
        }
#ifndef _WIN32
        if ((restore_owner || ::geteuid() == 0) && j.entry && j.entry->header.has_owner) {
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            if (j.entry->header.has_owner_uid) {
                uid = static_cast<uid_t>(j.entry->header.owner_uid);
            } else if (!j.entry->header.owner_user.empty()) {
                struct passwd* pw = ::getpwnam(j.entry->header.owner_user.c_str());
                if (pw) uid = pw->pw_uid;
            }
            if (j.entry->header.has_owner_gid) {
                gid = static_cast<gid_t>(j.entry->header.owner_gid);
            } else if (!j.entry->header.owner_group.empty()) {
                struct group* gr = ::getgrnam(j.entry->header.owner_group.c_str());
                if (gr) gid = gr->gr_gid;
            }
            if (uid != static_cast<uid_t>(-1) || gid != static_cast<uid_t>(-1)) {
                // lchown does not follow symlinks; non-fatal on EPERM/ENOSYS
                if (::lchown(j.target.c_str(), uid, gid) != 0) {
                    // Ignored: unprivileged user or unsupported filesystem
                }
            }
        }
        if (j.entry && j.entry->header.host_os == 1 && j.entry->header.redir_type == 0) {
            mode_t mode = static_cast<mode_t>(j.entry->header.attributes & 07777);
            if (mode != 0) {
                if (::chmod(j.target.c_str(), mode) != 0) {
                    // Ignored: non-fatal permission update
                }
            }
        }
#endif
    };

    if (slots.readers.empty()) {
        size_t idx = 0;
        for (const auto& job : extract_jobs) {
            idx++;
            Prog.start_file(job.entry->header.file_name, idx);

            if (!g_quiet_mode && !is_vt_supported()) {
                std::cout << "Extracting  " << job.display_name << " ... ";
            }

            // -o- targets were pre-filtered above; this check still fires for
            // the rest of the run after the user answers n[E]ver to a query.
            std::error_code ow_ec;
            if (overwrite_mode == OverwriteMode::SkipExisting &&
                std::filesystem::exists(job.target, ow_ec)) {
                if (!g_quiet_mode && !is_vt_supported()) std::cout << "SKIPPED (already exists)\n";
                continue;
            }

            if (overwrite_mode == OverwriteMode::Prompt &&
                std::filesystem::exists(job.target, ow_ec)) {
                switch (ask_overwrite(job.display_name)) {
                case OverwriteAnswer::Yes:
                    break;
                case OverwriteAnswer::Always:
                    overwrite_mode = OverwriteMode::Overwrite;
                    break;
                case OverwriteAnswer::Never:
                    overwrite_mode = OverwriteMode::SkipExisting;
                    [[fallthrough]];
                case OverwriteAnswer::No:
                    if (!g_quiet_mode && !is_vt_supported())
                        std::cout << "SKIPPED (already exists)\n";
                    continue;
                case OverwriteAnswer::Quit:
                    std::cerr << "User break\n";
                    return EXIT_USER_BREAK;
                }
            }

            if (reader.extract_entry(*job.entry, job.target, password)) {
                if (!g_quiet_mode && !is_vt_supported()) std::cout << "OK\n";
                restore_children(reader, job);
            } else {
                if (!g_quiet_mode && !is_vt_supported()) {
                    if (reader.has_bad_password()) {
                        std::cout << "FAILED (incorrect password / BADPSW)\n";
                    } else {
                        std::cout << "FAILED\n";
                    }
                }
                return EXIT_FATAL;
            }
            Prog.update_bytes(job.entry->header.unp_size);
        }
    } else {
        EntryFlags flags;
        flags.resize(extract_jobs.size());
        ThreadPool pool(static_cast<unsigned>(slots.readers.size()));
        for (auto& r : slots.readers) {
            r->set_keep_broken(keep_broken);
            r->set_extract_symlinks(extract_symlinks);
        }

        // Categorize jobs into three phases:
        // Phase 1: Regular files (parallelized across the thread pool)
        // Phase 2: Links/redirections (symlinks, junctions, hardlinks, filecopy) - sequential
        // Phase 3: Directories - bottom-up reverse topological order
        std::vector<size_t> phase1_indices;
        std::vector<size_t> phase2_indices;
        std::vector<size_t> phase3_indices;

        for (size_t i = 0; i < extract_jobs.size(); ++i) {
            const auto* ent = extract_jobs[i].entry;
            if (ent->header.redir_type != 0) {
                phase2_indices.push_back(i);
            } else if ((ent->header.file_flags & format::FHFL_DIRECTORY) != 0) {
                phase3_indices.push_back(i);
            } else {
                phase1_indices.push_back(i);
            }
        }

        // Sort Phase 3 directory indices descending by target path length (deepest directories first)
        std::sort(phase3_indices.begin(), phase3_indices.end(), [&](size_t a, size_t b) {
            return extract_jobs[a].target.string().size() > extract_jobs[b].target.string().size();
        });

        // Phase 1: Parallel file extraction
        std::mutex phase1_mu;
        std::condition_variable phase1_cv;
        size_t phase1_remaining = phase1_indices.size();

        for (size_t idx : phase1_indices) {
            pool.submit([&slots, &flags, &extract_jobs, &badpw_flag, &restore_children,
                         &phase1_remaining, &phase1_cv, &phase1_mu, idx] {
                bool okv = false;
                size_t s = 0;
                bool acquired = false;
                try {
                    s = slots.acquire();
                    acquired = true;
                    okv = slots.readers[s]->extract_entry(*extract_jobs[idx].entry,
                                                          extract_jobs[idx].target,
                                                          slots.readers[s]->password());
                    if (okv) {
                        restore_children(*slots.readers[s], extract_jobs[idx]);
                    }
                    if (!okv && slots.readers[s]->has_bad_password()) badpw_flag.store(1);
                } catch (...) {
                    okv = false;
                }
                if (acquired) slots.release(s);
                flags.finish(idx, okv);
                try {
                    Prog.note_file_done(extract_jobs[idx].entry->header.file_name,
                                        extract_jobs[idx].entry->header.unp_size);
                } catch (...) {
                }
                {
                    std::lock_guard<std::mutex> lk(phase1_mu);
                    if (--phase1_remaining == 0) {
                        phase1_cv.notify_one();
                    }
                }
            });
        }

        if (!phase1_indices.empty()) {
            std::unique_lock<std::mutex> lk(phase1_mu);
            phase1_cv.wait(lk, [&] { return phase1_remaining == 0; });
        }

        // Phase 2: Sequential links/redirections (target files guaranteed to exist on disk)
        for (size_t idx : phase2_indices) {
            bool okv = false;
            try {
                okv = slots.readers[0]->extract_entry(*extract_jobs[idx].entry,
                                                      extract_jobs[idx].target,
                                                      slots.readers[0]->password());
                if (okv) {
                    restore_children(*slots.readers[0], extract_jobs[idx]);
                }
                if (!okv && slots.readers[0]->has_bad_password()) badpw_flag.store(1);
            } catch (...) {
                okv = false;
            }
            flags.finish(idx, okv);
            try {
                Prog.note_file_done(extract_jobs[idx].entry->header.file_name,
                                    extract_jobs[idx].entry->header.unp_size);
            } catch (...) {
            }
        }

        // Phase 3: Directories in reverse topological order (bottom-up)
        for (size_t idx : phase3_indices) {
            bool okv = false;
            try {
                okv = slots.readers[0]->extract_entry(*extract_jobs[idx].entry,
                                                      extract_jobs[idx].target,
                                                      slots.readers[0]->password());
                if (okv) {
                    restore_children(*slots.readers[0], extract_jobs[idx]);
                }
                if (!okv && slots.readers[0]->has_bad_password()) badpw_flag.store(1);
            } catch (...) {
                okv = false;
            }
            flags.finish(idx, okv);
            try {
                Prog.note_file_done(extract_jobs[idx].entry->header.file_name,
                                    extract_jobs[idx].entry->header.unp_size);
            } catch (...) {
            }
        }

        // Report in archive order so console output matches the sequential run.
        // Unlike the old first-failure abort, every entry is attempted —
        // whatever is decodable gets extracted before the non-zero exit.
        for (size_t i = 0; i < extract_jobs.size(); ++i) {
            const bool okv = flags.wait(i);
            if (!g_quiet_mode && !is_vt_supported()) {
                std::cout << "Extracting  " << extract_jobs[i].display_name << " ... "
                          << (okv ? "OK"
                                  : (badpw_flag.load() ? "FAILED (incorrect password / BADPSW)"
                                                       : "FAILED"))
                          << "\n";
            }
            if (!okv) any_failed = true;
        }
    }

    if (any_failed) {
        Prog.done(extract_jobs.size(), "unpacked", arc_path, total_bytes, 0);
        return badpw_flag.load() ? EXIT_BAD_PASSWORD : EXIT_FATAL;
    }
    Prog.done(total_entries, "unpacked", arc_path, total_bytes, 0);
    return 0;
}

int repair_archive(const std::string& arc_path) {
    // Uses the inline "RR" service block's Reed-Solomon parity (0x1100B GF(2^16)
    // Cauchy) for single-volume archives, or external .rev parity shards for
    // multi-volume sets. Recoverable damage: tail-truncated or corrupt data
    // shards where RR/REV parity allows mathematical reconstruction.
    // Unrecoverable: damage exceeding parity redundancy, or damaged .rev headers.
    if (openrar::recovery::RecoveryWriter::repair(arc_path)) {
        if (!g_quiet_mode)
            std::cout << "Archive " << arc_path << ": OK (reconstruction / structure verified)\n";
        return 0;
    }
    std::cerr << "Cannot repair " << arc_path << "\n";
    std::cerr << "       (no usable recovery data, damage exceeds parity, .rev set "
                 "mismatched or incomplete, or unsupported recovery format)\n";
    return EXIT_FATAL;
}

int lock_archive(const std::string& arc_path, const std::string& password = "") {
    if (!archive::ArchiveMutator::lock_archive(arc_path, password)) {
        std::cerr << "Cannot lock archive\n";
        return EXIT_FATAL;
    }

    if (!g_quiet_mode) {
        std::cout << "Archive " << arc_path << " locked successfully\n";
    }
    return 0;
}

int convert_to_sfx(const std::string& arc_path, const std::string& sfx_name_raw,
                   const char* argv0) {
    std::filesystem::path sfx_stub_path =
        archive::ArchiveMutator::resolve_sfx_stub(sfx_name_raw, argv0);
    std::string err;
    if (!archive::ArchiveMutator::convert_to_sfx(arc_path, sfx_stub_path, err)) {
        std::cerr << "Error: " << err << "\n";
        return EXIT_FATAL;
    }
    if (!g_quiet_mode) {
        std::cout << "Done\n";
    }
    return 0;
}

int move_to_archive(const std::string& arc_path, const std::vector<std::string>& files,
                    int method = 3, const std::filesystem::path& sfx_stub = {},
                    ::openrar::core::uint64 vol_size = 0, const std::string& password = "",
                    bool encrypt_headers = false, unsigned threads = 1, bool want_qo = true,
                    const std::vector<std::string>& exclude_patterns = {},
                    core::uint64 dict_size = 0, const compress::FilterConfig& filter_cfg = {}) {
    if (files.empty()) {
        std::cerr << "No files specified for move\n";
        return EXIT_FATAL;
    }
    std::vector<PendingFile> queue;
    for (const auto& f : files) {
        std::string entry_name = std::filesystem::path(f).filename().string();
        if (is_path_excluded(entry_name, exclude_patterns) ||
            is_path_excluded(f, exclude_patterns)) {
            continue;
        }
        std::error_code stat_ec;
        core::uint64 sz = std::filesystem::file_size(f, stat_ec);
        if (stat_ec) sz = 0; // let prepare_add_file report the unreadable source
        queue.push_back({std::filesystem::path(f), entry_name, sz});
    }
    if (queue.empty()) {
        std::cerr << "No files found to move\n";
        return EXIT_FATAL;
    }

    if (vol_size != 0 && vol_size != archive::volume::VOLSIZE_AUTO) {
        // Volume chain rewrite is inherently sequential (see add_to_archive).
        for (const auto& item : queue) {
            bool ok = archive::ArchiveMutator::move_file_to_archive_vol(
                arc_path, item.src_path, item.entry_name, method, vol_size, password,
                /*solid=*/false, dict_size, filter_cfg, encrypt_headers);
            if (!ok) {
                std::cerr << "Failed moving " << item.src_path.string() << " to " << arc_path
                          << "\n";
                return EXIT_FATAL;
            }
        }
        return 0;
    }

    std::string failed_name;
    int rc = run_batch_add(arc_path, queue, method, sfx_stub, password, encrypt_headers,
                           /*delete_source=*/true, /*announce=*/false, threads, &failed_name,
                           archive::time_flags::MTIME, /*solid=*/false, nullptr, /*want_stm=*/false,
                           /*want_acl=*/false, want_qo, /*want_ams=*/false, dict_size, filter_cfg);
    if (rc != 0) {
        std::cerr << "Failed moving "
                  << (failed_name.empty() ? queue.front().src_path.string() : failed_name) << " to "
                  << arc_path << "\n";
        return EXIT_FATAL;
    }
    return 0;
}

} // namespace openrar::cli

// L10: everything from switch parsing down can throw (std::filesystem on
// unreadable dirs, vanishing files, current_path(), bad_alloc from hostile
// archives). This body must not be reached by an escaping exception directly;
// main() below wraps it. The argc<2/argv copies here cannot throw.
static bool parse_mc_switch(const std::string& s, openrar::compress::FilterConfig& cfg) {
    if (s == "-mc-") {
        cfg.mode = openrar::compress::FilterMode::DisableAll;
        return true;
    }
    std::string tail = s.substr(3);
    if (tail.empty()) {
        cfg.mode = openrar::compress::FilterMode::Auto;
        cfg.e8_override = 0;
        cfg.arm_override = 0;
        cfg.delta_override = 0;
        cfg.delta_channels = 0;
        return true;
    }
    if (tail == "-") {
        cfg.mode = openrar::compress::FilterMode::DisableAll;
        return true;
    }

    size_t i = 0;
    while (i < tail.size()) {
        std::string params;
        while (i < tail.size() &&
               (std::isdigit(static_cast<unsigned char>(tail[i])) || tail[i] == ':')) {
            params.push_back(tail[i]);
            ++i;
        }
        if (i >= tail.size()) {
            if (params == "-") {
                cfg.mode = openrar::compress::FilterMode::DisableAll;
            }
            break;
        }
        char type_char = tail[i++];
        char sign = 0;
        if (i < tail.size() && (tail[i] == '+' || tail[i] == '-')) {
            sign = tail[i++];
        }

        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(type_char)));
        int override_val = (sign == '+') ? 1 : ((sign == '-') ? -1 : 0);

        if (upper == 'E') {
            cfg.e8_override = override_val;
            if (override_val > 0) cfg.mode = openrar::compress::FilterMode::Auto;
        } else if (upper == 'A') {
            cfg.arm_override = override_val;
            if (override_val > 0) cfg.mode = openrar::compress::FilterMode::Auto;
        } else if (upper == 'D') {
            cfg.delta_override = override_val;
            if (override_val > 0) cfg.mode = openrar::compress::FilterMode::Auto;
            if (!params.empty()) {
                auto colon = params.rfind(':');
                std::string chan_str =
                    (colon != std::string::npos) ? params.substr(colon + 1) : params;
                try {
                    int ch = std::stoi(chan_str);
                    if (ch >= 1 && ch <= 32) {
                        cfg.delta_channels = static_cast<openrar::core::uint8>(ch);
                    }
                } catch (...) {
                }
            }
        } else if (upper == 'L' || upper == 'X' || upper == 'T') {
            // Long range / exhaustive / text accepted for compatibility
        }
    }
    return true;
}

static int cli_main(int argc, char* argv[]) {
    using namespace openrar::cli;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "OpenRAR " << OPENRAR_VERSION_STRING << "\n";
            return 0;
        }
    }

    if (argc < 2) {
        openrar::cli::print_help();
        return 0;
    }

    std::string cmd = argv[1];
    std::string arc_path;
    std::vector<std::string> files;
    std::vector<std::string> switches;
    std::vector<std::string> exclude_patterns;

    bool stop_switches = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (!stop_switches && arg == "--") {
            stop_switches = true;
            continue;
        }
        if (!stop_switches && !arg.empty() && arg[0] == '-') {
            switches.push_back(arg);
            continue;
        }
        if (arc_path.empty()) {
            arc_path = arg;
        } else {
            if (!stop_switches && arg.size() > 1 && arg[0] == '@') {
                std::string err;
                if (!openrar::cli::read_listfile(arg.substr(1), files, err)) {
                    std::cerr << "Error: " << err << "\n";
                    return 7;
                }
            } else {
                files.push_back(arg);
            }
        }
    }

    if (arc_path.empty()) {
        std::cerr << "Error: No archive name specified.\n";
        openrar::cli::print_help();
        return EXIT_FATAL;
    }

    std::string password;
    bool want_header_encryption = false; // -hp
    int method = 3;
    unsigned mt_flag = 0; // 0 = auto-detect
    openrar::core::uint64 vol_size = 0;
    bool vol_pause = false;
    // Feature switch flags
    bool want_sfx = false, want_rr = false;
    bool want_acl = false, want_stm = false;
    bool want_hardlinks = false;                                            // -oh
    bool want_solid = false;                                                // -s
    bool no_dir_records = false;                                            // -ed
    bool want_lock = false;                                                 // -k
    std::string comment_path;                                               // -z<file>
    openrar::core::uint32 times_mask = openrar::archive::time_flags::MTIME; // -ts<...>
    std::string sfx_name_raw;
    openrar::core::uint32 rr_percent = 3;
    openrar::io::ExcludePathMode ep_mode = openrar::io::ExcludePathMode::None;
    bool recurse_subdirs = true;
    bool want_symlinks = false;   // -ol
    bool extract_symlinks = true; // -ol-
    bool keep_broken = false;     // -kb
    openrar::cli::OverwriteMode overwrite_mode = openrar::cli::OverwriteMode::Prompt;
    bool want_qo = true;   // -qo, -qo+, -qo- (default: enabled)
    bool want_ams = false; // -ams, -am
    bool want_rv = false;  // -rv
    openrar::core::uint32 rv_count_or_percent = 1;
    bool rv_is_percent = false;
    openrar::core::uint64 opt_dict_size = 0; // -md<size>
    openrar::compress::FilterConfig opt_filter_cfg;
    int max_versions = -1;    // -1 = disabled, 0 = unlimited, >0 = limit
    int extract_version = -1; // -1 = default, 0 = all versions (-ver), >0 = specific (-verN)
    bool want_og = false;     // -og
    std::string opt_group;
    std::string opt_user;

    for (const auto& s : switches) {
        if (sw_eq(s, "-plain") || sw_eq(s, "--plain") || sw_eq(s, "-idp") ||
            sw_eq(s, "--no-color")) {
            openrar::cli::g_plain_mode = true;
        } else if (sw_eq(s, "-q") || sw_eq(s, "-quiet") || sw_eq(s, "--quiet") ||
                   sw_eq(s, "-inul") || sw_eq(s, "-idq")) {
            openrar::cli::g_quiet_mode = true;
        } else if (sw_eq(s, "-y")) {
            openrar::cli::g_assume_yes = true;
        } else if (sw_eq(s, "-r")) {
            recurse_subdirs = true;
        } else if (sw_eq(s, "-r-")) {
            recurse_subdirs = false;
        } else if (sw_eq(s, "-oh")) {
            want_hardlinks = true;
        } else if (sw_eq(s, "-oh-")) {
            want_hardlinks = false;
        } else if (sw_eq(s, "-ol")) {
            want_symlinks = true;
            extract_symlinks = true;
        } else if (sw_eq(s, "-ol-")) {
            want_symlinks = false;
            extract_symlinks = false;
        } else if (sw_eq(s, "-ep")) {
            ep_mode = openrar::io::ExcludePathMode::SkipWholePath;
        } else if (sw_eq(s, "-ep1")) {
            ep_mode = openrar::io::ExcludePathMode::BasePath;
        } else if (sw_eq(s, "-ep2")) {
            ep_mode = openrar::io::ExcludePathMode::SaveFullPathNoDrive;
        } else if (sw_eq(s, "-ep3")) {
            ep_mode = openrar::io::ExcludePathMode::AbsPath;
        } else if (sw_eq(s, "-kb")) {
            keep_broken = true;
        } else if (sw_eq(s, "-o+")) {
            overwrite_mode = openrar::cli::OverwriteMode::Overwrite;
        } else if (sw_eq(s, "-o-")) {
            overwrite_mode = openrar::cli::OverwriteMode::SkipExisting;
        } else if (sw_starts(s, "-hp") && s.size() > 3) {
            // Header encryption. Match this BEFORE the -p prefix check.
            password = s.substr(3);
            want_header_encryption = true;
        } else if (sw_eq(s, "-hp")) {
            // A bare -hp must fail loudly: falling through silently produced
            // an UNENCRYPTED archive while the user believed headers were
            // protected (sweep finding M7).
            std::cerr << "Error: -hp requires a password (-hp<password>).\n";
            return 7;
        } else if (sw_starts(s, "-p") && s.size() > 2) {
            password = s.substr(2);
        } else if (sw_eq(s, "-p")) {
            std::cerr << "Error: -p requires a password (-p<password>).\n";
            return 7;
        } else if (sw_starts(s, "-m") && s.size() == 3 && s[2] >= '0' && s[2] <= '5') {
            method = s[2] - '0';
        } else if (sw_starts(s, "-mt")) {
            // -mt<N> worker threads for batch add. -mt0 / bare
            // -mt / unparseable = auto-detect. Clamped to a sane ceiling;
            // file-granular work beyond this only costs thread stacks.
            std::string mt_tail = s.substr(3);
            long v = 0;
            try {
                v = mt_tail.empty() ? 0 : std::stol(mt_tail);
            } catch (...) {
                v = 0;
            }
            if (v < 0) v = 0;
            if (v > 64) v = 64;
            mt_flag = static_cast<unsigned>(v);
        } else if (sw_starts(s, "-md")) {
            std::string tail = s.substr(3);
            bool is_mdx = false;
            if (!tail.empty() && (tail[0] == 'x' || tail[0] == 'X')) {
                is_mdx = true;
                tail = tail.substr(1);
            }
            if (tail.empty()) {
                std::cerr << "Error: -md requires a size specification\n";
                return 7;
            }
            char unit = tail.empty() ? 0 : tail.back();
            uint64_t mult = 1;
            bool has_unit = false;
            if (unit == 'k' || unit == 'K') {
                mult = 1024ULL;
                tail.pop_back();
                has_unit = true;
            } else if (unit == 'm' || unit == 'M') {
                mult = 1024ULL * 1024ULL;
                tail.pop_back();
                has_unit = true;
            } else if (unit == 'g' || unit == 'G') {
                mult = 1024ULL * 1024ULL * 1024ULL;
                tail.pop_back();
                has_unit = true;
            } else if (unit == 't' || unit == 'T') {
                mult = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
                tail.pop_back();
                has_unit = true;
            }
            if (!has_unit) {
                // If no modifier is present, megabytes are assumed for -md
                // and gigabytes for -mdx switch (matching WinRAR spec).
                mult = is_mdx ? (1024ULL * 1024ULL * 1024ULL) : (1024ULL * 1024ULL);
            }
            uint64_t val = 0;
            try {
                size_t consumed = 0;
                double dval = std::stod(tail, &consumed);
                // Strict parse: the whole numeric tail must be consumed
                // ("-md16mxyz" is an error, not 16 MB) and non-finite values
                // are rejected before the double→uint64 cast, which is UB for
                // out-of-range floats (v1.21.1 fix).
                if (consumed != tail.size() || !std::isfinite(dval) || dval < 0) {
                    std::cerr << "Error: invalid dictionary size '" << s << "'\n";
                    return 7;
                }
                const double kMaxDict = 1024.0 * 1024.0 * 1024.0 * 1024.0; // 1 TB
                if (dval * static_cast<double>(mult) > kMaxDict) {
                    std::cerr << "Error: dictionary size > 1 TB not allowed\n";
                    return 7;
                }
                val = static_cast<uint64_t>(dval * static_cast<double>(mult));
            } catch (...) {
                std::cerr << "Error: invalid dictionary size '" << s << "'\n";
                return 7;
            }
            if (val < 128ULL * 1024ULL) {
                std::cerr << "Error: dictionary size < 128 KB not allowed\n";
                return 7;
            }
            if (val > 1024ULL * 1024ULL * 1024ULL * 1024ULL) { // 1 TB
                std::cerr << "Error: dictionary size > 1 TB not allowed\n";
                return 7;
            }
            // In RAR 7.0, non-power-of-two dictionary sizes are permitted.
            // Adjust to the nearest discrete step: base 128K<<N + fraction*(base/32)
            uint64_t requested = val;
            uint64_t pow2 = 0x20000;
            while (2 * pow2 <= val && pow2 < (1ULL << 39)) {
                pow2 *= 2;
            }
            if (val > pow2) {
                uint64_t step = pow2 / 32;
                if (step > 0) {
                    uint64_t fraction = (val - pow2) / step;
                    if (fraction > 31) fraction = 31;
                    val = pow2 + fraction * step;
                }
            }
            if (val != requested) {
                // Accepted-but-snapped values must not shrink silently
                // (v1.21.2): -md1t snaps to 1008 GiB (grid cap), off-grid
                // values floor to the nearest step.
                std::cerr << "Warning: dictionary size snapped to " << (val >> 20)
                          << " MiB (FCI grid limit)\n";
            }
            opt_dict_size = val;
        } else if (sw_starts(s, "-ver") &&
                   (s.size() == 4 || std::all_of(s.begin() + 4, s.end(), [](unsigned char c) {
                        return std::isdigit(c);
                    }))) {
            // Exact dispatch (v1.21.2): only "-ver" or "-ver<digits>"; other
            // "-ver*" spellings (e.g. "-verbose") fall through to the
            // unknown-switch error instead of silently enabling versioning.
            std::string tail = s.substr(4);
            if (tail.empty()) {
                max_versions = 0;
                extract_version = 0;
            } else {
                try {
                    int n = std::stoi(tail);
                    max_versions = (n >= 0) ? n : 0;
                    extract_version = n;
                } catch (...) {
                    max_versions = 0;
                    extract_version = 0;
                }
            }
        } else if (sw_starts(s, "-v")) {
            std::string vs = s.substr(2);
            if (vs == "p" || vs == "-") {
                if (vs == "p") vol_pause = true;
                if (vs == "-") vol_size = 0;
            } else if (sw_starts(vs, "p")) {
                vol_pause = true;
                std::string rest = vs.substr(1);
                if (!rest.empty()) {
                    bool ok = false;
                    openrar::core::uint64 v =
                        openrar::archive::volume::parse_vol_size_str(rest, ok);
                    if (ok) vol_size = v;
                }
            } else {
                bool ok = false;
                openrar::core::uint64 v = openrar::archive::volume::parse_vol_size_str(vs, ok);
                if (ok)
                    vol_size = v;
                else
                    vol_size = openrar::archive::volume::VOLSIZE_AUTO;
            }
            if (sw_eq(s, "-vp")) vol_pause = true;
        } else if (sw_starts(s, "-rr")) {
            want_rr = true;
            // Parse -rr[N][%] where N is percent (e.g. -rr, -rr3, -rr10%, -rr100)
            std::string tail = s.substr(3);
            // strip trailing % or p
            while (!tail.empty() &&
                   (tail.back() == '%' || tail.back() == 'p' || tail.back() == 'P'))
                tail.pop_back();
            if (!tail.empty()) {
                try {
                    rr_percent = static_cast<openrar::core::uint32>(std::stoul(tail));
                } catch (...) {
                    rr_percent = 3;
                }
                if (rr_percent == 0) rr_percent = 3;
                if (rr_percent > 1000) rr_percent = 1000;
            } else {
                rr_percent = 3;
            }
        } else if (sw_starts(s, "-rv")) {
            want_rv = true;
            std::string tail = s.substr(3);
            if (!tail.empty() && (tail.back() == '%' || tail.back() == 'p' || tail.back() == 'P')) {
                rv_is_percent = true;
                tail.pop_back();
            } else {
                rv_is_percent = false;
            }
            if (!tail.empty()) {
                try {
                    rv_count_or_percent = static_cast<openrar::core::uint32>(std::stoul(tail));
                } catch (...) {
                    rv_count_or_percent = rv_is_percent ? 3 : 1;
                }
                if (rv_count_or_percent == 0) rv_count_or_percent = rv_is_percent ? 3 : 1;
                if (rv_count_or_percent > 1000) rv_count_or_percent = 1000;
            } else {
                rv_count_or_percent = 1;
                rv_is_percent = false;
            }
        } else if (sw_eq(s, "-s")) {
            want_solid = true;
        } else if (sw_eq(s, "-ed")) {
            no_dir_records = true;
        } else if (sw_eq(s, "-k")) {
            want_lock = true;
        } else if (sw_starts(s, "-z")) {
            comment_path = s.substr(2);
        } else if (sw_starts(s, "-ts")) {
            for (char c : s.substr(3)) {
                if (c == 'm' || c == 'M')
                    times_mask |= openrar::archive::time_flags::MTIME;
                else if (c == 'c' || c == 'C')
                    times_mask |= openrar::archive::time_flags::CTIME;
                else if (c == 'a' || c == 'A')
                    times_mask |= openrar::archive::time_flags::ATIME;
                else if (c == 'p' || c == 'P' || c == '+' || c == '-' || (c >= '0' && c <= '4')) {
                    // precision / sign accepted, second granularity
                } else {
                    break;
                }
            }
        } else if (sw_starts(s, "-sfx")) {
            want_sfx = true;
            sfx_name_raw = s.substr(4); // may be empty -> default.sfx; strip '=' later in resolver
            if (!sfx_name_raw.empty() && sfx_name_raw[0] == '=')
                sfx_name_raw = sfx_name_raw.substr(1);
        } else if (sw_eq(s, "-ow") || sw_starts(s, "-ow")) {
            want_acl = true;
        } else if (sw_starts(s, "-og")) {
            want_og = true;
            opt_group = s.substr(3);
        } else if (sw_starts(s, "--group=")) {
            want_og = true;
            opt_group = s.substr(8);
        } else if (sw_starts(s, "--owner=")) {
            opt_user = s.substr(8);
        } else if (sw_eq(s, "-os") || sw_starts(s, "-os")) {
            want_stm = true;
        } else if (sw_eq(s, "-qo") || sw_eq(s, "-qo+")) {
            want_qo = true;
        } else if (sw_eq(s, "-qo-")) {
            want_qo = false;
        } else if (sw_eq(s, "-am") || sw_eq(s, "-ams")) {
            want_ams = true;
        } else if (sw_starts(s, "-mc")) {
            parse_mc_switch(s, opt_filter_cfg);
        } else if (sw_starts(s, "-x@") && s.size() > 3) {
            std::string err;
            if (!openrar::cli::read_listfile(s.substr(3), exclude_patterns, err)) {
                std::cerr << "Error: " << err << "\n";
                return 7;
            }
        } else if (sw_eq(s, "-x@")) {
            // Bare "-x@" used to fall through and become the exclusion
            // pattern "@" (v1.21.2).
            std::cerr << "Error: -x@ requires a listfile path (-x@<listfile>).\n";
            return 7;
        } else if (sw_starts(s, "-x") && s.size() > 2) {
            exclude_patterns.push_back(s.substr(2));
        } else if (sw_eq(s, "-x")) {
            std::cerr << "Error: -x requires a pattern (-x<pattern> or -x@<listfile>).\n";
            return 7;
        } else {
            // Unknown switches must not vanish silently: a mistyped
            // security-relevant switch would otherwise degrade to insecure
            // defaults with no trace (sweep finding M7).
            std::cerr << "Warning: unknown switch '" << s << "' ignored\n";
        }
    }
    (void)vol_pause;
    // Defer W: messages to command handlers; keep flags
    (void)want_acl;
    (void)want_stm;

    // Worker count for batch add/move and RR parity (and later extract/test):
    // explicit -mt wins, otherwise one worker per hardware thread.
    unsigned threads = mt_flag ? mt_flag : openrar::core::hardware_thread_hint();

    // Header encryption (-hp): implies per-file data encryption
    if (want_header_encryption && (cmd == "a" || cmd == "u" || cmd == "f" || cmd == "m")) {
        if (password.empty()) {
            std::cerr << "Error: -hp requires a password (-hp<password>).\n";
            return 7;
        }
    }
    // RecoveryWriter now handles -rr (0x1100B Cauchy). Keep rejection only
    // for vintage 0x11D if ever requested
    // For now all -rr goes via RecoveryWriter; vintage detection would be via archive version flag
    if ((cmd == "a" || cmd == "u" || cmd == "f" || cmd == "m") && want_rr) {
        // defer to handler below; no early rejection
    }
    // Read the -z comment payload up front so a bad file fails before any
    // archive work starts.
    std::vector<openrar::core::byte> comment;
    if (!comment_path.empty() && (cmd == "a" || cmd == "u" || cmd == "f" || cmd == "m")) {
        openrar::io::FileStream cf;
        if (!cf.open(comment_path, openrar::io::FileMode::ReadOnly)) {
            std::cerr << "Error: cannot open comment file " << comment_path << "\n";
            return 7;
        }
        openrar::core::uint64 csz = cf.size();
        if (csz > 0x100000) { // sanity bound; comments are small by nature
            std::cerr << "Error: comment file " << comment_path << " larger than 1 MiB\n";
            return 7;
        }
        comment.resize(static_cast<size_t>(csz));
        if (csz > 0 && cf.read(comment.data(), comment.size()) != comment.size()) {
            std::cerr << "Error: cannot read comment file " << comment_path << "\n";
            return 7;
        }
    }
    // SFX creation is now supported via flag-driven mutator; keep multivolume
    // edge rejection (stub only first volume)
    // Previous generic SFX rejection removed. Multivalue case already returned above via want_vol.
    // ACL/STM are read-skipped with warning; writer path warns once. The
    // warning also fires for x/e: without it the switches are silently
    // ignored there (B8 class — advertised behavior must never fail silent).
#ifndef _WIN32
    if ((cmd == "a" || cmd == "u" || cmd == "f" || cmd == "m" || cmd == "x" || cmd == "e") &&
        want_stm) {
        std::cerr << "W: alternate streams (-os) preservation not supported on this platform\n";
    }
#endif

    // SFX handling: resolve stub, validate MAX_SFX_SIZE, apply SetSFXExt
    // (first-volume-only with -v handled via want_vol reject above)
    std::filesystem::path sfx_stub_path;
    std::string sfx_arc_path_str = arc_path;
    if (want_sfx && (cmd == "a" || cmd == "u" || cmd == "f" || cmd == "m")) {
        sfx_stub_path = openrar::archive::ArchiveMutator::resolve_sfx_stub(sfx_name_raw, argv[0]);
        if (!std::filesystem::exists(sfx_stub_path)) {
            std::cerr << "Cannot open " << sfx_stub_path.string() << "\n";
            return 6;
        }
        std::error_code ec;
        auto sz = std::filesystem::file_size(sfx_stub_path, ec);
        if (!ec && sz > openrar::archive::ArchiveMutator::MAX_SFX_SIZE) {
            std::cerr << "SFX module too large (" << sz << " > "
                      << openrar::archive::ArchiveMutator::MAX_SFX_SIZE << ")\n";
            return 7;
        }
        // SetSFXExt .exe/.sfx per 10-sfx.md:70 (final name known before open)
        sfx_arc_path_str = openrar::archive::ArchiveMutator::apply_sfx_extension(arc_path).string();
    }

    if (cmd == "a" || cmd == "u" || cmd == "f") {
        std::string target_arc = want_sfx ? sfx_arc_path_str : arc_path;
        int rc = 0;
        if (files.empty()) {
            if (std::filesystem::exists(target_arc) && (want_rr || want_rv || want_lock)) {
                rc = 0;
            } else {
                std::cerr << "No files specified for addition\n";
                return EXIT_FATAL;
            }
        } else {
            rc = openrar::cli::add_to_archive(
                target_arc, files, method, sfx_stub_path, vol_size, password,
                want_header_encryption, threads, want_solid, comment, times_mask, no_dir_records,
                ep_mode, recurse_subdirs, want_symlinks, (cmd == "f"), want_stm, want_acl,
                want_hardlinks, want_qo, want_ams, exclude_patterns, opt_dict_size, opt_filter_cfg,
                max_versions, opt_group, opt_user, want_lock);
        }
        if (rc == 0 && want_rr) {
            bool rr_ok;
            bool is_vol_set = vol_size != 0 && vol_size != openrar::archive::volume::VOLSIZE_AUTO;
            if (is_vol_set) {
                // Multi-volume sets get external .rev parity volumes (spec §4.7).
                rr_ok = openrar::recovery::RecoveryWriter::write_rev_volumes(target_arc, rr_percent,
                                                                             threads);
            } else {
                rr_ok = openrar::recovery::RecoveryWriter::add_recovery_record(target_arc,
                                                                               rr_percent, threads);
            }
            if (!rr_ok) {
                std::cerr << "W: recovery record creation failed (-- vintage 0x11D not yet "
                             "implemented)\n";
                return EXIT_FATAL;
            }
            if (!openrar::cli::g_quiet_mode) {
                if (is_vol_set)
                    std::cout << "Added .rev recovery volumes (" << rr_percent << "%)\n";
                else
                    std::cout << "Added RR " << rr_percent << "% (0x1100B)\n";
            }
        }
        if (rc == 0 && want_rv) {
            bool is_vol_set = vol_size != 0 && vol_size != openrar::archive::volume::VOLSIZE_AUTO;
            if (!is_vol_set) {
                std::cerr << "Cannot create recovery volumes for a non-volume archive\n";
                return EXIT_FATAL;
            }
            bool rv_ok = openrar::recovery::RecoveryWriter::write_rev_volumes(
                target_arc, rv_count_or_percent, rv_is_percent, threads);
            if (!rv_ok) {
                std::cerr << "W: recovery volume creation failed\n";
                return EXIT_FATAL;
            }
            if (!openrar::cli::g_quiet_mode) {
                if (rv_is_percent)
                    std::cout << "Added .rev recovery volumes (" << rv_count_or_percent << "%)\n";
                else
                    std::cout << "Added .rev recovery volumes (" << rv_count_or_percent << ")\n";
            }
        }
        // -k locks after the archive (and any RR) is fully written, matching
        // the `k` command's rewrite; locking earlier would block RR splicing.
        if (rc == 0 && want_lock) {
            if (!openrar::archive::ArchiveMutator::lock_archive(target_arc, password)) {
                std::cerr << "Error: failed to lock " << target_arc << "\n";
                return EXIT_FATAL;
            }
        }
        return rc;
    } else if (cmd == "p") {
        return openrar::cli::print_archive_to_stdout(arc_path, files, exclude_patterns, password);
    } else if (cmd == "x" || cmd == "e") {
        std::string dest = "";
        std::vector<std::string> file_patterns;
        if (!files.empty()) {
            if (files.size() == 1) {
                if (files[0].find(';') != std::string::npos ||
                    files[0].find('*') != std::string::npos ||
                    files[0].find('?') != std::string::npos) {
                    file_patterns.push_back(files[0]);
                } else {
                    dest = files[0];
                }
            } else {
                const std::string& last = files.back();
                // Guard the empty-string argument ("x arc \"\""): back() on an
                // empty string is UB (v1.21.1 fix).
                if (!last.empty() && (last.back() == '/' || last.back() == '\\' ||
                                      std::filesystem::is_directory(last))) {
                    dest = last;
                    for (size_t fi = 0; fi + 1 < files.size(); ++fi) {
                        file_patterns.push_back(files[fi]);
                    }
                } else {
                    for (size_t fi = 0; fi < files.size(); ++fi) {
                        file_patterns.push_back(files[fi]);
                    }
                }
            }
        }
        return openrar::cli::extract_archive(arc_path, dest, cmd == "x", password, threads,
                                             keep_broken, overwrite_mode, extract_symlinks,
                                             exclude_patterns, extract_version, file_patterns,
                                             (want_acl || want_og));
    } else if (cmd == "r") {
        return openrar::cli::repair_archive(arc_path);
    } else if (cmd == "rr" || (cmd.rfind("rr", 0) == 0 && cmd.size() > 2 &&
                               std::all_of(cmd.begin() + 2, cmd.end(), [](unsigned char c) {
                                   return std::isdigit(c) || c == '%' || c == 'p' || c == 'P';
                               }))) {
        if (!std::filesystem::exists(arc_path)) {
            std::cerr << "Cannot open " << arc_path << "\n";
            return EXIT_OPEN;
        }
        if (cmd.size() > 2) {
            std::string pct_str = cmd.substr(2);
            while (!pct_str.empty() &&
                   (pct_str.back() == '%' || pct_str.back() == 'p' || pct_str.back() == 'P'))
                pct_str.pop_back();
            if (!pct_str.empty()) {
                try {
                    rr_percent = static_cast<openrar::core::uint32>(std::stoul(pct_str));
                } catch (...) {
                    rr_percent = 3;
                }
                if (rr_percent == 0) rr_percent = 3;
                if (rr_percent > 1000) rr_percent = 1000;
            }
        }
        bool rr_ok =
            openrar::recovery::RecoveryWriter::add_recovery_record(arc_path, rr_percent, threads);
        if (!rr_ok) {
            std::cerr << "W: recovery record creation failed\n";
            return EXIT_FATAL;
        }
        if (!openrar::cli::g_quiet_mode) {
            std::cout << "Added RR " << rr_percent << "% (0x1100B)\n";
        }
        return 0;
    } else if (cmd == "rv" || cmd.rfind("rv", 0) == 0) {
        if (!std::filesystem::exists(arc_path)) {
            std::filesystem::path first =
                openrar::archive::volume::vol_name_to_first_name(arc_path, false);
            if (std::filesystem::exists(first)) {
                arc_path = first.string();
            } else {
                first = openrar::archive::volume::vol_name_to_first_name(arc_path, true);
                if (std::filesystem::exists(first)) {
                    arc_path = first.string();
                } else {
                    std::cerr << "Cannot open " << arc_path << "\n";
                    return EXIT_FATAL;
                }
            }
        }
        openrar::core::uint32 count_or_pct = 1;
        bool is_pct = false;
        if (cmd.size() > 2) {
            std::string val = cmd.substr(2);
            if (!val.empty() && (val.back() == '%' || val.back() == 'p' || val.back() == 'P')) {
                is_pct = true;
                val.pop_back();
            }
            if (!val.empty()) {
                try {
                    count_or_pct = static_cast<openrar::core::uint32>(std::stoul(val));
                } catch (...) {
                    count_or_pct = is_pct ? 3 : 1;
                }
                if (count_or_pct == 0) count_or_pct = is_pct ? 3 : 1;
                if (count_or_pct > 1000) count_or_pct = 1000;
            }
        } else if (want_rv) {
            count_or_pct = rv_count_or_percent;
            is_pct = rv_is_percent;
        }
        bool ok = openrar::recovery::RecoveryWriter::write_rev_volumes(arc_path, count_or_pct,
                                                                       is_pct, threads);
        if (!ok) {
            // Honest diagnostics (v1.21.1): a false return covers IO/parity
            // failures too, not just a non-volume archive.
            std::cerr << "Cannot create recovery volumes for " << arc_path << "\n";
            std::cerr << "       (archive may not be a multi-volume set, or an IO/parity "
                         "error occurred)\n";
            return EXIT_FATAL;
        }
        if (!openrar::cli::g_quiet_mode) {
            if (is_pct)
                std::cout << "Created recovery volumes (" << count_or_pct << "%) for " << arc_path
                          << "\n";
            else
                std::cout << "Created recovery volumes (" << count_or_pct << ") for " << arc_path
                          << "\n";
        }
        return 0;
    } else if (cmd == "l" || cmd == "v") {
        return openrar::cli::list_archive(arc_path, false, false, password, exclude_patterns,
                                          files);
    } else if (cmd == "lb") {
        return openrar::cli::list_archive(arc_path, true, false, password, exclude_patterns, files);
    } else if (cmd == "lt" || cmd == "lta") {
        return openrar::cli::list_archive(arc_path, false, true, password, exclude_patterns, files);
    } else if (cmd == "t") {
        return openrar::cli::test_archive(arc_path, password, threads, exclude_patterns, files);
    } else if (cmd == "d") {
        return openrar::cli::delete_from_archive(arc_path, files);
    } else if (cmd == "k") {
        return openrar::cli::lock_archive(arc_path, password);
    } else if (cmd == "m") {
        // B8-class honesty: a/u/f honor -z/-s/-ts, but m's batch path does
        // not carry them. Warn instead of silently dropping advertised
        // behavior.
        if (!g_quiet_mode) {
            if (!comment_path.empty())
                std::cerr << "W: -z is not applied by m (archive comment dropped)\n";
            if (want_solid) std::cerr << "W: -s is not applied by m (solid mode dropped)\n";
            if (times_mask != openrar::archive::time_flags::MTIME)
                std::cerr << "W: -ts is not applied by m (times stored with the default mask)\n";
        }
        std::string move_arc = sfx_stub_path.empty() ? arc_path : sfx_arc_path_str;
        int rc = openrar::cli::move_to_archive(move_arc, files, method, sfx_stub_path, vol_size,
                                               password, want_header_encryption, threads, want_qo,
                                               exclude_patterns, opt_dict_size, opt_filter_cfg);
        if (rc == 0 && want_rr) {
            bool rr_ok;
            bool is_vol_set = vol_size != 0 && vol_size != openrar::archive::volume::VOLSIZE_AUTO;
            if (is_vol_set) {
                // Multi-volume sets get external .rev parity volumes (spec §4.7),
                // same as a/u/f. Writing an inline RR into the first volume
                // would desynchronise the set instead.
                rr_ok = openrar::recovery::RecoveryWriter::write_rev_volumes(move_arc, rr_percent,
                                                                             threads);
            } else {
                rr_ok = openrar::recovery::RecoveryWriter::add_recovery_record(move_arc, rr_percent,
                                                                               threads);
            }
            if (!rr_ok) {
                std::cerr << "W: recovery record creation failed (-- vintage 0x11D not yet "
                             "implemented)\n";
                return EXIT_FATAL;
            }
            if (!openrar::cli::g_quiet_mode) {
                if (is_vol_set)
                    std::cout << "Added .rev recovery volumes (" << rr_percent << "%)\n";
                else
                    std::cout << "Added RR " << rr_percent << "% (0x1100B)\n";
            }
        }
        // -k locks after the archive (and any RR) is fully written (see a/u/f).
        if (rc == 0 && want_lock) {
            if (!openrar::archive::ArchiveMutator::lock_archive(move_arc, password)) {
                std::cerr << "Error: failed to lock " << move_arc << "\n";
                return EXIT_FATAL;
            }
        }
        return rc;
    } else if (cmd == "s" || cmd == "sfx" || cmd == "S" || cmd == "SFX") {
        // Exact dispatch (v1.21.2): the old `s*` prefix match silently
        // accepted typos ("stats") as SFX conversion with the suffix as a
        // module name that could never resolve. Modules are selected with
        // the -sfx switch.
        std::string sfx_sub = sfx_name_raw;
        return openrar::cli::convert_to_sfx(arc_path, sfx_sub, argv[0]);
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        openrar::cli::print_help();
        return EXIT_USAGE;
    }

    return 0;
}

#ifdef _WIN32
static std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) return {};
    std::string str(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size_needed, nullptr, nullptr);
    return str;
}
#endif

int main(int argc, char* argv[]) {
    // L10: an uncaught std::filesystem/library exception used to reach the
    // top of main and call std::terminate - no diagnostic, no partial-output
    // cleanup, exit code meaningless. Report and exit non-zero instead.
    try {
#ifdef _WIN32
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            std::vector<std::string> utf8_args;
            std::vector<char*> utf8_argv;
            utf8_args.reserve(wargc);
            utf8_argv.reserve(wargc + 1);
            for (int i = 0; i < wargc; ++i) {
                utf8_args.push_back(wide_to_utf8(wargv[i]));
            }
            LocalFree(wargv);
            for (auto& s : utf8_args) {
                utf8_argv.push_back(&s[0]);
            }
            utf8_argv.push_back(nullptr);
            return cli_main(wargc, utf8_argv.data());
        }
#endif
        return cli_main(argc, argv);
    } catch (const std::bad_alloc&) {
        std::cerr << "openrar: error: out of memory\n";
        return openrar::cli::EXIT_MEMORY;
    } catch (const std::exception& e) {
        std::cerr << "openrar: error: " << e.what() << "\n";
        return openrar::cli::EXIT_FATAL;
    } catch (...) {
        std::cerr << "openrar: error: unknown non-standard exception\n";
        return openrar::cli::EXIT_FATAL;
    }
}
