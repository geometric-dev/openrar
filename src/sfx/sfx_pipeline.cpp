#include "sfx_pipeline.hpp"

#include "process_exec.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
namespace fs = std::filesystem;

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openrar::sfx {

namespace {

constexpr size_t kTempHexChars = 32; // 128-bit random hex

std::string user_profile_dir() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &raw))) {
        std::string profile;
        const int len = WideCharToMultiByte(CP_UTF8, 0, raw, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::wstring w(raw);
            profile.assign(w.begin(), w.end());
        }
        CoTaskMemFree(raw);
        return profile;
    }
    return "";
#else
    const char* home = getenv("HOME");
    return home ? home : "";
#endif
}

// True when `candidate` (already absolute, lexically normalized) is inside
// `base` (also absolute/normalized). Case-insensitive on Windows.
bool path_inside(const std::string& base, const std::string& candidate) {
    if (base.empty() || candidate.empty()) return false;
    std::string b = base;
    std::string c = candidate;
#ifdef _WIN32
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
#endif
    std::string nb = fs::path(b).lexically_normal().string();
    std::string nc = fs::path(c).lexically_normal().string();
    if (nb.back() != '/'
#ifdef _WIN32
        && nb.back() != '\\'
#endif
    ) {
        nb += fs::path::preferred_separator;
    }
    return nc.compare(0, nb.size(), nb) == 0;
}

std::string random_hex(size_t chars) {
    std::string out;
    out.reserve(chars);
    while (out.size() < chars) {
        unsigned char buf[16];
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            // NTSTATUS nonzero = failure. Fall back to rand: temp NAMES are
            // not the security boundary here (the owner-only ACL is).
            for (unsigned char& b : buf) b = static_cast<unsigned char>(std::rand());
        }
#else
        FILE* f = fopen("/dev/urandom", "rb");
        if (!f || fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
            for (unsigned char& b : buf) b = static_cast<unsigned char>(std::rand());
        }
        if (f) fclose(f);
#endif
        static const char* hexd = "0123456789abcdef";
        for (unsigned char b : buf)
            if (out.size() < chars) out += hexd[b >> 4];
    }
    return out;
}

std::string temp_root() {
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmp) > 0) {
        std::string t = tmp;
        if (!t.empty() && t.back() == '\\') t.pop_back();
        return t;
    }
    return "";
#else
    const char* t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
#endif
}

} // namespace

bool match_wildcard(const std::string& pattern, const std::string& name) {
    // Iterative '*' matcher with backtracking; '?' matches one character.
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
#ifdef _WIN32
    auto eq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    };
#else
    auto eq = [](char a, char b) {
        return a == b;
    };
#endif
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string make_tempmode_dir() {
    const std::string root = temp_root();
    if (root.empty()) return "";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string dir = root + "/OpenRAR-" + random_hex(kTempHexChars);
        std::error_code ec;
        if (fs::create_directory(dir, ec) && !ec) {
#ifndef _WIN32
            chmod(dir.c_str(), 0700);
#endif
            return dir;
        }
        if (ec) continue; // collision or failure: retry with a new name
    }
    return "";
}

std::string resolve_sfx_destination(const SfxConfig& cfg, const PipelineOptions& opts,
                                    std::vector<DirectiveReportLine>& report) {
    // Explicit user destination: a real user choice, overrides everything.
    if (!opts.argv_dest.empty()) return opts.argv_dest;
    // TempMode overrides Path= (plan §7); the caller creates the temp dir.
    if (cfg.tempmode) return "";

    if (!cfg.path.empty()) {
        // Containment (plan §7): the archive may only propose a directory
        // inside the user profile. This is a refusal check, not a prompt.
        const std::string profile = user_profile_dir();
        std::error_code ec;
        const std::string abs = fs::absolute(fs::u8path(cfg.path), ec).string();
        if (profile.empty() || ec || !fs::path(abs).is_absolute() || !path_inside(profile, abs)) {
            report.push_back({"path", "refused-containment-path",
                              "Path= outside the user profile refused: " + cfg.path});
            return "";
        }
        std::error_code mk_ec;
        fs::create_directories(abs, mk_ec);
        if (mk_ec) {
            report.push_back({"path", "failed", "Path= destination not creatable: " + cfg.path});
            return "";
        }
        return abs;
    }
    return "";
}

PipelineResult run_sfx_pipeline(
    const SfxConfig& cfg, SfxConsentEngine& consent, const PipelineOptions& opts,
    const std::function<int(const std::string& dest, OverwriteDirective overwrite)>& extract_fn) {
    PipelineResult result;
    auto report = [&result](const char* phase, const char* status, const std::string& detail) {
        result.report.push_back({phase, status, detail});
    };

    const size_t side_effecting =
        cfg.setup.size() + cfg.presetup.size() + cfg.delete_patterns.size() + cfg.shortcuts.size();

    // ── Destination resolution ────────────────────────────────────────────
    std::string dest = resolve_sfx_destination(cfg, opts, result.report);
    if (cfg.tempmode) {
        dest = make_tempmode_dir();
        if (dest.empty()) {
            report("tempmode", "failed", "could not create a contained temp directory");
            result.exit_code = 2;
            return result;
        }
        report("tempmode", "executed", "temp directory created");
    }
    if (dest.empty()) {
        // No argv destination, no (or refused) Path=, no TempMode: extraction
        // cannot proceed — the caller's default destination would be a guess.
        // Use the current directory (the stub's historical default) and let
        // the extraction layer's own guards apply.
        dest = fs::current_path().string();
    }
    // Canonicalize: Setup's CWD, per-match Delete containment, and the
    // extraction layer all compare absolute paths (weakly_canonical also
    // resolves a relative argv destination against the CWD).
    std::error_code mkdest_ec;
    fs::create_directories(dest, mkdest_ec);
    result.effective_dest = fs::weakly_canonical(dest, mkdest_ec).string();
    if (mkdest_ec || result.effective_dest.empty()) result.effective_dest = dest;

    // ── -sfxnoexec kill switch (observable, plan §9) ──────────────────────
    if (opts.no_exec) {
        result.suppressed_directives = side_effecting;
        report("pipeline", "skipped-noexec",
               "directives suppressed: -sfxnoexec (" + std::to_string(side_effecting) + ")");
        // Overwrite=1 is an escalation needing consent; with prompts suppressed
        // it de-escalates to Ask (never a silent overwrite-all).
        const OverwriteDirective eff = cfg.overwrite == OverwriteDirective::OverwriteAll
                                           ? OverwriteDirective::Ask
                                           : cfg.overwrite;
        result.exit_code = extract_fn(result.effective_dest, eff);
        return result;
    }

    // ── Presetup phase ────────────────────────────────────────────────────
    for (const std::string& cmd : cfg.presetup) {
        if (consent.aborted()) break;
        const std::string resolved =
            resolve_command_executable(cmd, /*dest_dir=*/"", /*allow_dest=*/false);
        if (resolved.empty()) {
            report("presetup", "skipped-not-found", cmd);
            continue;
        }
        bool amsi_failed = false;
        const bool amsi_clean = amsi_scan_command(cmd, amsi_failed);
        const ConsentDecision d =
            consent.ask({DirectiveType::Presetup,
                         amsi_clean || amsi_failed
                             ? "run a program BEFORE extraction"
                             : "run a program BEFORE extraction (WARNING: Windows AMSI flagged "
                               "this command as suspicious)",
                         cmd, resolved, ""});
        if (d != ConsentDecision::Run) {
            report("presetup", consent.aborted() ? "aborted" : "skipped-consent", cmd);
            continue;
        }
        ExecResult r = spawn_contained(cmd, resolved, /*working_dir=*/"",
                                       [&consent]() { return consent.aborted(); });
        if (!r.spawned) {
            report("presetup", "refused-containment", cmd + ": " + r.error);
            result.exit_code = 2;
            continue;
        }
        report("presetup", r.cancelled ? "cancelled" : "executed",
               cmd + " exit=" + std::to_string(r.exit_code));
        if (r.cancelled) {
            result.aborted = true;
            result.exit_code = 255; // user-break (§3.1)
            return result;
        }
        if (r.exit_code != 0 && result.exit_code == 0) result.exit_code = r.exit_code;
    }
    if (consent.aborted()) {
        result.aborted = true;
        if (result.exit_code == 0) result.exit_code = 2;
        return result;
    }

    // ── Extraction phase ──────────────────────────────────────────────────
    // Overwrite escalation needs its own consent (plan §7): Overwrite=1
    // denied falls back to Ask; the §3.2 engine stays the decision function.
    OverwriteDirective effective = cfg.overwrite;
    if (cfg.overwrite == OverwriteDirective::OverwriteAll) {
        const ConsentDecision d =
            consent.ask({DirectiveType::OverwriteEscalation, "overwrite existing files", "", "",
                         "the archive requests overwrite-all; existing files may be replaced"});
        if (d != ConsentDecision::Run) effective = OverwriteDirective::Ask;
    }
    const int extract_exit = extract_fn(result.effective_dest, effective);
    if (extract_exit != 0) {
        result.exit_code = extract_exit;
        return result; // incomplete extraction never reaches Setup/Delete
    }

    // ── Setup phase ───────────────────────────────────────────────────────
    for (const std::string& cmd : cfg.setup) {
        if (consent.aborted()) break;
        const std::string resolved = resolve_command_executable(cmd, result.effective_dest,
                                                                /*allow_dest=*/true);
        if (resolved.empty()) {
            report("setup", "skipped-not-found", cmd);
            continue;
        }
        bool amsi_failed = false;
        const bool amsi_clean = amsi_scan_command(cmd, amsi_failed);
        const ConsentDecision d =
            consent.ask({DirectiveType::Setup,
                         amsi_clean || amsi_failed
                             ? "run a program AFTER extraction"
                             : "run a program AFTER extraction (WARNING: Windows AMSI flagged this "
                               "command as suspicious)",
                         cmd, resolved, "working directory: " + result.effective_dest});
        if (d != ConsentDecision::Run) {
            report("setup", consent.aborted() ? "aborted" : "skipped-consent", cmd);
            continue;
        }
        ExecResult r = spawn_contained(cmd, resolved, result.effective_dest,
                                       [&consent]() { return consent.aborted(); });
        if (!r.spawned) {
            report("setup", "refused-containment", cmd + ": " + r.error);
            result.exit_code = 2;
            continue;
        }
        report("setup", r.cancelled ? "cancelled" : "executed",
               cmd + " exit=" + std::to_string(r.exit_code));
        if (r.cancelled) {
            result.aborted = true;
            result.exit_code = 255;
            return result;
        }
        if (r.exit_code != 0 && result.exit_code == 0) result.exit_code = r.exit_code;
    }
    if (consent.aborted()) {
        result.aborted = true;
        if (result.exit_code == 0) result.exit_code = 2;
        return result;
    }

    // ── Delete phase: one coalesced consent for the whole set ─────────────
    if (!cfg.delete_patterns.empty()) {
        std::string detail =
            std::to_string(cfg.delete_patterns.size()) + " pattern(s) in: " + result.effective_dest;
        const ConsentDecision d =
            consent.ask({DirectiveType::Delete, "delete files after extraction", "", "", detail});
        if (d != ConsentDecision::Run) {
            report("delete", consent.aborted() ? "aborted" : "skipped-consent", detail);
        } else {
            std::error_code walk_ec;
            for (fs::recursive_directory_iterator it(
                     result.effective_dest, fs::directory_options::skip_permission_denied, walk_ec);
                 it != fs::recursive_directory_iterator(); it.increment(walk_ec)) {
                if (walk_ec) break;
                if (!it->is_regular_file()) continue;
                const std::string full = it->path().string();
                // Containment per match: must resolve inside the destination
                // (symlink-hostile — recursive_directory_iterator does not
                // follow directory symlinks by default).
                std::error_code canon_ec;
                const std::string canon = fs::weakly_canonical(it->path(), canon_ec).string();
                if (canon_ec || !path_inside(result.effective_dest, canon)) {
                    report("delete", "refused-containment-path", full);
                    continue;
                }
                for (const std::string& pattern : cfg.delete_patterns) {
                    const std::string rel =
                        fs::path(full).lexically_relative(fs::path(result.effective_dest)).string();
                    if (match_wildcard(pattern, rel) ||
                        match_wildcard(pattern, it->path().filename().string())) {
                        std::error_code rm_ec;
                        if (fs::remove(it->path(), rm_ec) && !rm_ec)
                            report("delete", "executed", full);
                        else
                            report("delete", "failed", full);
                        break;
                    }
                }
            }
        }
    }

    // ── TempMode cleanup ──────────────────────────────────────────────────
    if (cfg.tempmode) {
        std::error_code rm_ec;
        const size_t removed = fs::remove_all(result.effective_dest, rm_ec);
        report("tempmode", rm_ec ? "failed" : "executed",
               "temp dir cleanup (" + std::to_string(removed) + " entries)");
    }

    return result;
}

} // namespace openrar::sfx
