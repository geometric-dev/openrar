#include "process_exec.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include "../core/types.hpp"
#include <windows.h>
#include <amsi.h>
#pragma comment(lib, "amsi.lib")
// The mitigation-policy nibble constants are missing from some installed
// SDKs; the encoding is documented (4-bit fields, 1 = ALWAYS_ON, the
// dynamic-code field sits at bits 40..43). Guarded so newer SDKs win.
#ifndef PROCESS_CREATION_MITIGATION_POLICY_DYNAMIC_CODE_DISABLE_ALWAYS_ON
#define PROCESS_CREATION_MITIGATION_POLICY_DYNAMIC_CODE_DISABLE_ALWAYS_ON (0x00000001UI64 << 40)
#endif

#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <cstdlib>
#include <chrono>
#include <thread>
#endif

namespace openrar::sfx {

namespace {

// Quote-aware first-token extraction (CreateProcessW parsing rules for the
// executable token: a leading quoted section wins, otherwise up to the first
// whitespace run).
std::string first_token(const std::string& command) {
    if (command.empty()) return "";
    if (command[0] == '"') {
        const size_t end = command.find('"', 1);
        return end == std::string::npos ? command.substr(1) : command.substr(1, end - 1);
    }
    const size_t end = command.find_first_of(" \t");
    return end == std::string::npos ? command : command.substr(0, end);
}

// Quote-aware argv tokenization for POSIX execv (values are authored command
// lines; double quotes group, no escape sequences — documented grammar).
std::vector<std::string> tokenize_command(const std::string& command) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = command.size();
    while (i < n) {
        while (i < n && (command[i] == ' ' || command[i] == '\t')) ++i;
        if (i >= n) break;
        std::string tok;
        bool quoted = false;
        while (i < n) {
            if (command[i] == '"') {
                quoted = !quoted;
                ++i;
                continue;
            }
            if (!quoted && (command[i] == ' ' || command[i] == '\t')) break;
            tok += command[i];
            ++i;
        }
        out.push_back(tok);
    }
    return out;
}

bool file_present(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

} // namespace

std::string resolve_command_executable(const std::string& command, const std::string& dest_dir,
                                       bool allow_dest) {
    const std::string token = first_token(command);
    if (token.empty()) return "";
#ifdef _WIN32
    const bool absolute = token.size() >= 2 && (token[1] == ':' || token[1] == '\\');
#else
    const bool absolute = !token.empty() && token[0] == '/';
#endif
    if (absolute && file_present(token)) return token;

    if (allow_dest && !dest_dir.empty()) {
        std::string joined = dest_dir;
#ifdef _WIN32
        if (joined.back() != '\\' && joined.back() != '/') joined += '\\';
#else
        if (joined.back() != '/') joined += '/';
#endif
        joined += token;
        if (file_present(joined)) return joined;
    }

    // PATH search.
#ifdef _WIN32
    char path_buf[MAX_PATH];
    if (SearchPathA(nullptr, token.c_str(), ".exe", MAX_PATH, path_buf, nullptr) > 0)
        return path_buf;
#else
    const char* path_env = getenv("PATH");
    if (path_env) {
        std::string paths = path_env;
        size_t start = 0;
        while (start <= paths.size()) {
            size_t colon = paths.find(':', start);
            if (colon == std::string::npos) colon = paths.size();
            const std::string dir = paths.substr(start, colon - start);
            if (!dir.empty()) {
                std::string joined = dir;
                if (joined.back() != '/') joined += '/';
                joined += token;
                if (file_present(joined)) return joined;
            }
            if (colon == paths.size()) break;
            start = colon + 1;
        }
    }
#endif
    return "";
}

#ifdef _WIN32

namespace {
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}
} // namespace

bool amsi_scan_command(const std::string& command, bool& amsi_failed) {
    amsi_failed = false;
    HAMSICONTEXT ctx = nullptr;
    if (AmsiInitialize(L"OpenRAR.SFX", &ctx) != S_OK || ctx == nullptr) {
        amsi_failed = true;
        return true; // documented defense-in-depth fail-open (plan §2)
    }
    const std::wstring wcmd = to_wide(command);
    AMSI_RESULT result = AMSI_RESULT_CLEAN;
    const HRESULT hr =
        AmsiScanString(ctx, wcmd.c_str(), L"OpenRAR SFX directive", nullptr, &result);
    AmsiUninitialize(ctx);
    if (FAILED(hr)) {
        amsi_failed = true;
        return true;
    }
    return result == AMSI_RESULT_CLEAN;
}

ExecResult spawn_contained(const std::string& command, const std::string& resolved_exe,
                           const std::string& working_dir, const std::function<bool()>& cancelled) {
    ExecResult result;
    if (resolved_exe.empty() || !file_present(resolved_exe)) {
        result.error = "resolved executable not found";
        return result;
    }

    // Containment first: Job Object with kill-on-close, per-process memory
    // cap (2 GiB) and active-process cap (64). Any failure refuses execution.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        result.error = "containment unavailable: CreateJobObject failed";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.ProcessMemoryLimit = 2ULL * 1024 * 1024 * 1024;
    limits.BasicLimitInformation.ActiveProcessLimit = 64;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.error = "containment unavailable: SetInformationJobObject failed";
        CloseHandle(job);
        return result;
    }

    // Mitigation policy: ACG (dynamic code disabled, always-on). Child-process
    // restriction from the pre-analysis was evaluated and REJECTED: standard
    // installer chains (msiexec spawning custom actions) break under it; ACG
    // + CFG (compile-time) + CET + the Job Object carry the mitigation weight.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        result.error = "containment unavailable: attribute list init failed";
        CloseHandle(job);
        return result;
    }
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_DYNAMIC_CODE_DISABLE_ALWAYS_ON;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy,
                                   sizeof(policy), nullptr, nullptr)) {
        result.error = "containment unavailable: mitigation policy rejected";
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(job);
        return result;
    }

    std::wstring wcmd = to_wide(command);
    std::wstring wcwd = to_wide(working_dir);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                   wcwd.empty() ? nullptr : wcwd.c_str(),
                                   reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        result.error = "CreateProcess failed";
        CloseHandle(job);
        return result;
    }

    // Containment is now established: assign while suspended, then resume.
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        result.error = "containment unavailable: job assignment failed";
        return result;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    result.spawned = true;
    result.contained = true;

    // Wait in slices so cancellation can kill the whole job.
    for (;;) {
        const DWORD w = WaitForSingleObject(pi.hProcess, 100);
        if (w == WAIT_OBJECT_0) break;
        if (cancelled && cancelled()) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            result.cancelled = true;
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(job); // kill-on-close sweeps any children the payload left
    return result;
}

#else // POSIX

bool amsi_scan_command(const std::string& command, bool& amsi_failed) {
    amsi_failed = false;
    (void)command;
    return true; // AMSI is Windows-only: no-op elsewhere (plan §5)
}

ExecResult spawn_contained(const std::string& command, const std::string& resolved_exe,
                           const std::string& working_dir, const std::function<bool()>& cancelled) {
    ExecResult result;
    if (resolved_exe.empty() || access(resolved_exe.c_str(), X_OK) != 0) {
        result.error = "resolved executable not found or not executable";
        return result;
    }

    std::vector<std::string> toks = tokenize_command(command);
    std::vector<char*> argv;
    argv.reserve(toks.size() + 1);
    for (auto& t : toks) argv.push_back(t.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        // Child: own process group + rlimits + death signal, then exec.
        setpgid(0, 0);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif
        struct rlimit rl;
        rl.rlim_cur = 600;
        rl.rlim_max = 600;
        setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = 2ULL * 1024 * 1024 * 1024;
        rl.rlim_max = rl.rlim_cur;
        setrlimit(RLIMIT_AS, &rl);
        rl.rlim_cur = 64;
        rl.rlim_max = 64;
        setrlimit(RLIMIT_NPROC, &rl);
        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) _exit(126);
        }
        // Containment established pre-exec; a setrlimit failure here must not
        // yield an uncontained exec (fail-closed): refuse.
        if (getrlimit(RLIMIT_CPU, &rl) != 0 || rl.rlim_max != 600) _exit(126);
        execv(resolved_exe.c_str(), argv.data());
        _exit(127); // execv only returns on failure
    }

    // Parent: own the process group too (race-safe double setpgid).
    setpgid(pid, pid);
    result.spawned = true;
    result.contained = true;

    for (;;) {
        int status = 0;
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status))
                result.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exit_code = 128 + WTERMSIG(status);
            break;
        }
        if (cancelled && cancelled()) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return result;
}

#endif // _WIN32

} // namespace openrar::sfx
