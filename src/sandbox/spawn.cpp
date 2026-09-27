#include "spawn.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
// ─────────────────────────────────────────────────────────────────────────────
//  Windows — AppContainer (the v1.29 directive's mechanism #1).
// ─────────────────────────────────────────────────────────────────────────────
#include <sddl.h>
#include <userenv.h>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace openrar::sandbox {

bool platform_sandbox_available() {
    // The AppContainer profile APIs live in userenv.dll (Vista+); resolve
    // lazily so the probe stays cheap and side-effect-free.
    HMODULE ue = GetModuleHandleW(L"userenv.dll");
    if (ue == nullptr) ue = LoadLibraryW(L"userenv.dll");
    return ue != nullptr && GetProcAddress(ue, "CreateAppContainerProfile") != nullptr;
}

namespace {

// Per-run profile name: alnum only, ≤ 64 chars (CreateAppContainerProfile rule).
std::wstring make_profile_name() {
    wchar_t name[64];
    // CreateAppContainerProfile rule: alphanumeric only (no dots/dashes).
    _snwprintf_s(name, _TRUNCATE, L"openrarworker%u%u",
                 static_cast<unsigned>(GetCurrentProcessId()),
                 static_cast<unsigned>(GetTickCount64() & 0xFFFFFFFFu));
    // Delete any stale profile from a crashed run with the same name so
    // creation cannot collide.
    DeleteAppContainerProfile(name);
    return name;
}

void quote_into(std::string& out, const std::string& token) {
    out += " \"";
    out += token;
    out += "\"";
}

} // namespace

bool spawn_worker_process(const std::string& exe, const std::vector<std::string>& argv_tail,
                          SpawnProfile profile, void** proc_out, std::string& err) {
    std::string cmdline = "\"" + exe + "\"";
    for (const std::string& a : argv_tail) quote_into(cmdline, a);
    std::string cmdline_mut = cmdline;
    STARTUPINFOEXA siex{};
    siex.StartupInfo.cb = sizeof(siex);
    LPVOID attr_buf = nullptr;
    SIZE_T attr_size = 0;

    std::wstring profile_name;
    PSID app_container_sid = nullptr;
    bool profile_created = false;
    //RAII-ish cleanup via a small lambda: every failure path below frees the
    // same four resources.
    auto cleanup = [&]() {
        if (attr_buf != nullptr) {
            DeleteProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf));
            HeapFree(GetProcessHeap(), 0, attr_buf);
        }
        if (app_container_sid != nullptr) FreeSid(app_container_sid);
        if (profile_created) DeleteAppContainerProfile(profile_name.c_str());
    };

    if (profile == SpawnProfile::PlatformSandboxed) {
        // 1. Per-run AppContainer profile with an EMPTY capability list: the
        //    token carries no capability SIDs — no filesystem, no network.
        profile_name = make_profile_name();
        const HRESULT hr = CreateAppContainerProfile(profile_name.c_str(), L"openrar worker",
                                                     L"per-run parse/decode sandbox", nullptr, 0,
                                                     &app_container_sid);
        if (FAILED(hr) || app_container_sid == nullptr) {
            char b[16];
            std::snprintf(b, sizeof(b), "%08lx", static_cast<unsigned long>(hr));
            err = "CreateAppContainerProfile failed (hr=0x";
            err += b;
            err += ")";
            cleanup();
            return false;
        }
        profile_created = true;

        // 2. Attribute list: SECURITY_CAPABILITIES + mitigation policy (the
        //    v1.23 SFX hardening posture).
        if (!InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            err = "InitializeProcThreadAttributeList (size) failed";
            cleanup();
            return false;
        }
        attr_buf = HeapAlloc(GetProcessHeap(), 0, attr_size);
        if (attr_buf == nullptr) {
            err = "out of memory for the attribute list";
            cleanup();
            return false;
        }
        if (!InitializeProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf), 2, 0, &attr_size)) {
            err = "InitializeProcThreadAttributeList failed";
            cleanup();
            return false;
        }
        SECURITY_CAPABILITIES sc{};
        sc.AppContainerSid = app_container_sid;
        sc.Capabilities = nullptr;
        sc.CapabilityCount = 0;
        if (!UpdateProcThreadAttribute(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf), 0,
                                       PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sc, sizeof(sc),
                                       nullptr, nullptr)) {
            err = "cannot set SECURITY_CAPABILITIES attribute (err=" +
                  std::to_string(GetLastError()) + ")";
            cleanup();
            return false;
        }
        DWORD64 policy[2] = {PROCESS_CREATION_MITIGATION_POLICY_CONTROL_FLOW_GUARD_ALWAYS_ON, 0};
        if (!UpdateProcThreadAttribute(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf), 0,
                                       PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy,
                                       sizeof(policy), nullptr, nullptr)) {
            err = "cannot set MITIGATION_POLICY attribute (err=" + std::to_string(GetLastError()) +
                  ")";
            cleanup();
            return false;
        }
        siex.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf);
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(
        nullptr, cmdline_mut.data(), nullptr, nullptr, TRUE,
        profile == SpawnProfile::PlatformSandboxed ? EXTENDED_STARTUPINFO_PRESENT : 0, nullptr,
        nullptr, &siex.StartupInfo, &pi);
    cleanup();
    if (!ok) {
        err = "CreateProcess failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    *proc_out = pi.hProcess;
    return true;
}

std::filesystem::path worker_exe_path() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path() / L"openrar_worker.exe";
}

} // namespace openrar::sandbox

#elif defined(__linux__)
// ─────────────────────────────────────────────────────────────────────────────
//  Linux — fork/exec + in-child seccomp-BPF allowlist (mechanism #2). The
//  filter is installed INSIDE the worker by install_linux_seccomp, called on
//  --install-seccomp after CRT init and before any protocol I/O.
// ─────────────────────────────────────────────────────────────────────────────
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace openrar::sandbox {

bool platform_sandbox_available() {
    // PR_GET_SECCOMP: >=0 when seccomp exists; EINVAL/ENOSYS when not.
    const long r = ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    return !(r == -1 && (errno == EINVAL || errno == ENOSYS));
}

namespace {

struct SockFilter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};
struct SockFilterProg {
    uint16_t len;
    SockFilter* filter;
};

// Allowlist entry: "nr == K → ALLOW", otherwise fall through to the next
// instruction (the chain's tail RET deny).
void add_allow(std::vector<SockFilter>& f, uint32_t nr) {
    f.push_back({BPF_JMP + BPF_JEQ + BPF_K, 0, 1, nr});
    f.push_back({BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ALLOW});
}

// Allowlist entry with an argument check: "nr == K → (args[i] & mask) must be
// 0, else EPERM; else ALLOW". Used for mmap/mprotect PROT_EXEC (Gate 0
// directive 3: the argument-filter stance, made real).
void add_allow_arg_nomask(std::vector<SockFilter>& f, uint32_t nr, uint32_t arg_index,
                          uint32_t mask) {
    // i+0: nr != K → skip 4 (LD/JSET/RET-deny/RET-allow) to the nr re-load.
    // i+1: load args[arg_index].
    // i+2: any masked bit set → i+3 deny; else → i+4 allow.
    f.push_back({BPF_JMP + BPF_JEQ + BPF_K, 0, 4, nr});
    f.push_back({BPF_LD + BPF_W + BPF_ABS, 0, 0,
                 static_cast<uint32_t>(offsetof(struct seccomp_data, args[0]) + 8 * arg_index)});
    f.push_back({BPF_JMP + BPF_JSET + BPF_K, 0, 1, mask});
    f.push_back({BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ERRNO | EPERM});
    f.push_back({BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ALLOW});
    f.push_back(
        {BPF_LD + BPF_W + BPF_ABS, 0, 0, static_cast<uint32_t>(offsetof(struct seccomp_data, nr))});
}

} // namespace

void install_linux_seccomp() {
#if defined(__x86_64__)
    const uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
    // Unsupported architecture for the sandbox: fail-closed (spawn.hpp
    // contract) — never continue unsandboxed when a sandbox was requested.
    _exit(4);
#endif

    std::vector<SockFilter> f;
    // Architecture check: mismatch → deny (the worker binary is single-arch).
    f.push_back({BPF_LD + BPF_W + BPF_ABS, 0, 0, offsetof(struct seccomp_data, arch)});
    f.push_back({BPF_JMP + BPF_JEQ + BPF_K, 1, 0, kArch});
    f.push_back({BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ERRNO | EPERM});
    f.push_back({BPF_LD + BPF_W + BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr)});

    // I/O on the inherited fds + channels (the ONLY external access).
    add_allow(f, SYS_read);
    add_allow(f, SYS_write);
    add_allow(f, SYS_pread64);
    add_allow(f, SYS_lseek);
    add_allow(f, SYS_fstat);
    add_allow(f, SYS_dup); // private read handle per extent (L16 discipline)
    // Memory management — MANDATORY (a sandbox that kills malloc kills the
    // worker; Gate 0 directive 3) — with PROT_EXEC argument-filtered out.
    add_allow_arg_nomask(f, SYS_mmap, 2, PROT_EXEC);
    add_allow_arg_nomask(f, SYS_mprotect, 2, PROT_EXEC);
    add_allow(f, SYS_munmap);
    add_allow(f, SYS_madvise);
    add_allow(f, SYS_brk);
    // Single-threaded worker runtime primitives.
    add_allow(f, SYS_futex);
    add_allow(f, SYS_rt_sigprocmask);
    add_allow(f, SYS_rt_sigaction);
    add_allow(f, SYS_rt_sigreturn);
    add_allow(f, SYS_clock_gettime);
    add_allow(f, SYS_clock_nanosleep);
    add_allow(f, SYS_nanosleep);
    add_allow(f, SYS_sched_getaffinity);
    add_allow(f, SYS_getrandom);
    add_allow(f, SYS_getpid);
    add_allow(f, SYS_exit);
    add_allow(f, SYS_exit_group);
    // Tail deny: everything else (open/openat/execve/socket/clone/...) → EPERM.
    f.push_back({BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ERRNO | EPERM});

    SockFilterProg prog{static_cast<uint16_t>(f.size()), f.data()};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        // Fail-closed: a sandbox that cannot be installed never falls back to
        // an unsandboxed parse silently.
        _exit(4);
    }
}

bool spawn_worker_process(const std::string& exe, const std::vector<std::string>& argv_tail,
                          SpawnProfile profile, void** proc_out, std::string& err) {
    (void)profile; // the sandbox is installed in-child via --install-seccomp
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : argv_tail) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        err = "fork failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        ::execv(exe.c_str(), argv.data());
        _exit(127); // exec never returns on success
    }
    *proc_out = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    return true;
}

std::filesystem::path worker_exe_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path() / "openrar_worker";
}

} // namespace openrar::sandbox

#else
// ─────────────────────────────────────────────────────────────────────────────
//  Other platforms (macOS until Seatbelt lands, wasm never): plain spawn only.
// ─────────────────────────────────────────────────────────────────────────────
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace openrar::sandbox {

bool platform_sandbox_available() {
    return false;
}

std::filesystem::path worker_exe_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        // macOS: no /proc — fall back to argv[0] via /proc-less proc_pidpath
        // is private API; the empty path is the loud in-proc fallback.
        return std::filesystem::path();
    }
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path() / "openrar_worker";
}

bool spawn_worker_process(const std::string& exe, const std::vector<std::string>& argv_tail,
                          SpawnProfile profile, void** proc_out, std::string& err) {
    (void)profile;
    if (profile == SpawnProfile::PlatformSandboxed) {
        err = "no sandbox model on this platform";
        return false;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : argv_tail) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        err = "fork failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        ::execv(exe.c_str(), argv.data());
        _exit(127);
    }
    *proc_out = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    return true;
}

} // namespace openrar::sandbox

#endif
