// WinGUI.SFX — native-dialog SFX stub (v1.23.0, plan §11). Shares the
// directive parser, consent engine, and process contract with Default.SFX;
// only the prompt backend differs (Windows message boxes instead of stdin).
//
// v1.23 GUI scope: consent via MessageBox chains (the Don't-Run default is
// MB_DEFBUTTON2), extraction without a progress dialog (Silent semantics are
// inherent), and fatal errors via MessageBox. Batch "deny all" is a two-step
// follow-up question; "run all" likewise.

#include "../archive/archive_reader.hpp"
#include "../core/types.hpp"
#include "../io/path_util.hpp"
#include "../sfx/prompt_console.hpp"
#include "../sfx/process_exec.hpp"
#include "../sfx/sfx_config.hpp"
#include "../sfx/sfx_consent.hpp"
#include "../sfx/sfx_pipeline.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(linker, "/subsystem:windows")

namespace openrar::sfx {

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring to_wide_gui(const std::string& s) {
    if (s.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

// MessageBox consent backend (WinGUI.SFX). Default focus is always the
// Don't-Run answer (MB_DEFBUTTON2). Batch answers are two-step follow-ups.
class GuiPromptBackend final : public IPromptBackend {
public:
    bool interactive() const override { return true; }

    ConsentAnswer ask(const ConsentRequest& req) override {
        std::wstring text = to_wide_gui("OpenRAR SFX wants to " + req.verb + ".");
        text += L"\n\n";
        if (!req.command.empty()) text += L"Command: " + to_wide_gui(req.command) + L"\n";
        if (!req.resolved_path.empty())
            text += L"Program: " + to_wide_gui(req.resolved_path) + L"\n";
        if (!req.detail.empty()) text += to_wide_gui(req.detail) + L"\n";
        text += L"\nRun this?";

        const int rc = MessageBoxW(nullptr, text.c_str(), L"OpenRAR SFX - Security Consent",
                                   MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (rc == IDYES) {
            const int all =
                MessageBoxW(nullptr, L"Apply this answer to all directives of this type?",
                            L"OpenRAR SFX", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            return all == IDYES ? ConsentAnswer::RunAll : ConsentAnswer::Run;
        }
        if (rc == IDCANCEL) return ConsentAnswer::Abort;
        // No = deny; offer deny-all as a follow-up.
        const int all = MessageBoxW(nullptr,
                                    L"Deny all directives of this type without asking "
                                    L"again?",
                                    L"OpenRAR SFX", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        return all == IDYES ? ConsentAnswer::DenyAll : ConsentAnswer::Deny;
    }
};

} // namespace openrar::sfx

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; wargv && i < argc; ++i) args.push_back(openrar::sfx::wide_to_utf8(wargv[i]));
    if (wargv) LocalFree(wargv);

    std::string dest_dir;
    std::string password;
    bool no_exec = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-sfxnoexec") {
            no_exec = true;
        } else if (arg.rfind("-d", 0) == 0) {
            if (arg.size() > 2)
                dest_dir = arg.substr(2);
            else if (i + 1 < args.size())
                dest_dir = args[++i];
        } else if (arg.rfind("-p", 0) == 0 && arg.size() > 2) {
            password = arg.substr(2);
        } else if (arg[0] != '-' && dest_dir.empty()) {
            dest_dir = arg;
        }
    }

    // Self-probe modes (sandbox e2e uses the same executable shape).
    if (args.size() >= 3 && args[1] == "--sfx-probe-touch") {
        HANDLE h = CreateFileW(openrar::sfx::to_wide_gui(args[2]).c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, "touched", 7, &written, nullptr);
            CloseHandle(h);
        }
        return 0;
    }
    if (args.size() >= 3 && args[1] == "--sfx-probe-exit") {
        return std::atoi(args[2].c_str());
    }

    wchar_t self_w[MAX_PATH];
    GetModuleFileNameW(nullptr, self_w, MAX_PATH);
    const std::filesystem::path self_path(openrar::sfx::wide_to_utf8(self_w));

    openrar::archive::ArchiveReader reader;
    if (!reader.open(self_path, password)) {
        MessageBoxW(nullptr, L"Cannot open the embedded SFX archive.", L"OpenRAR SFX",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    std::vector<openrar::core::byte> comment;
    openrar::sfx::SfxConfig cfg;
    if (reader.read_archive_comment(comment)) {
        cfg = openrar::sfx::parse_sfx_config(comment);
    }

    openrar::sfx::PipelineOptions pipe_opts;
    pipe_opts.no_exec = no_exec || getenv("OPENRAR_SFX_NOEXEC") != nullptr;
    pipe_opts.argv_dest = dest_dir;

    openrar::sfx::GuiPromptBackend gui_backend;
    openrar::sfx::SfxConsentEngine consent(gui_backend);

    auto extract_loop = [&](const std::string& pipe_dest,
                            openrar::sfx::OverwriteDirective overwrite) -> int {
        std::filesystem::path out_root =
            pipe_dest.empty() ? std::filesystem::current_path() : std::filesystem::path(pipe_dest);
        std::error_code mk_ec;
        std::filesystem::create_directories(out_root, mk_ec);

        for (const auto& entry : reader.entries()) {
            if (entry.header.is_service) continue;
            const std::string safe_name =
                openrar::io::sanitize_archive_path(entry.header.file_name);
            if (safe_name.empty()) continue;
            std::filesystem::path target = out_root / std::filesystem::path(safe_name);
            if (!openrar::io::is_lexically_contained(target, out_root)) continue;
            // §3.2: directives may only de-escalate; the GUI has no per-file
            // overwrite prompt, so Ask and SkipExisting skip existing targets.
            if (overwrite != openrar::sfx::OverwriteDirective::OverwriteAll &&
                std::filesystem::exists(target)) {
                continue;
            }
            if (!reader.extract_entry(entry, target, password)) return 1;
        }
        return 0;
    };

    const openrar::sfx::PipelineResult result =
        openrar::sfx::run_sfx_pipeline(cfg, consent, pipe_opts, extract_loop);

    if (result.exit_code != 0) {
        std::string detail = "Extraction or directive processing failed (exit " +
                             std::to_string(result.exit_code) + ").";
        for (const auto& line : result.report) {
            if (line.status == "refused-containment" || line.status == "failed") {
                detail += "\n" + line.phase + ": " + line.detail;
            }
        }
        MessageBoxW(nullptr, openrar::sfx::to_wide_gui(detail).c_str(), L"OpenRAR SFX",
                    MB_OK | MB_ICONERROR);
    } else if (result.suppressed_directives > 0) {
        const std::wstring note =
            L"Directives suppressed: -sfxnoexec (" +
            openrar::sfx::to_wide_gui(std::to_string(result.suppressed_directives)) + L")";
        MessageBoxW(nullptr, note.c_str(), L"OpenRAR SFX", MB_OK | MB_ICONINFORMATION);
    }
    return result.exit_code;
}
