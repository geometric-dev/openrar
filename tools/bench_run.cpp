// v1.40 standalone benchmark driver (SFX bundle payload; not a test).
//
// runbench.exe executes the full OpenRAR-vs-WinRAR-vs-UnRAR benchmark
// matrix with NO external dependencies beyond the VC runtime, generating
// its own deterministic corpora at runtime (nothing bundled), verifying
// every archive by SHA-256 tree hash, writing a feedback-ready JSON
// report to %USERPROFILE%, and removing every file it created.
//
// It is the Setup= payload of the openrar-bench.exe SFX bundle
// (tools/make_bench_bundle.ps1): the SFX stub extracts to
// %TEMP%\OpenRAR-<hex>, runs this driver (CWD = extraction dir), then
// TempMode deletes the whole subtree — engines and driver included.
//
// Protocol mirrors tools/perf_vs_winrar.py (see its header) so results
// are comparable with the dev harness:
//   - deterministic seeded corpora (CPython MT19937 port, byte-identical
//     where the dev generators have no repo dependency; code.cpp is a
//     synthetic source-like generator — dev embeds the repo src tree,
//     which a standalone bundle cannot see: fingerprints in the report
//     detect the drift);
//   - full mode: 1 untimed warm-up + 3 timed runs, MEDIAN reported;
//     --quick: 2 timed runs, no warm-up (dev quick parity);
//   - fresh outputs each run, pre-deleted OUTSIDE the timed window;
//   - every compression archive cross-extracted (openrar + unrar when
//     available), tree hash compared against the source corpus;
//   - extraction rows verify each run's tree too (dev leaves them
//     unverified — rows keep dev labels for grep-ability).
//
// Flags: --quick  --runs N  --keep  --no-pause  --out <path>
//        --engines openrar,unrar,winrar  --gen-only  --help
//        --prng-selftest (maintenance: validates the MT port against
//        CPython ground-truth vectors)
//
// Exit: 0 all rows ok; 1 some rows failed; 2 fatal (no openrar, gen fail).

#include "../src/archive/extraction_report.hpp"
#include "../src/crypto/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#else
#error "runbench is Windows-only (WinRAR comparison harness)"
#endif

namespace fs = std::filesystem;
using openrar::archive::json_escape;
using openrar::crypto::Sha256;

namespace {

// ─── strings / paths ────────────────────────────────────────────────────────

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

fs::path self_exe() {
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    if (len > 0) return fs::path(buf);
    return {};
}

std::string to_hex(const unsigned char* d, size_t n, size_t take) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    size_t lim = std::min(n, take);
    s.reserve(lim * 2);
    for (size_t i = 0; i < lim; i++) {
        s += kHex[d[i] >> 4];
        s += kHex[d[i] & 0xf];
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ─── child process execution ────────────────────────────────────────────────

struct ProcResult {
    int rc = -1;
    double sec = 0.0;
    bool timed_out = false;
    bool spawned = false;
};

std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            bs++;
            continue;
        }
        if (c == L'"') {
            out.append(bs * 2 + 1, L'\\');
            out += L'"';
            bs = 0;
            continue;
        }
        out.append(bs, L'\\');
        bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring join_cmdline(const std::vector<std::wstring>& args) {
    std::wstring s;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) s += L' ';
        s += quote_arg(args[i]);
    }
    return s;
}

// Runs one child with stdout+stderr captured to `log` (truncated each run),
// cwd optional, hard timeout. Wall time covers spawn->exit (dev parity:
// python subprocess.run timing).
ProcResult run_proc(const std::vector<std::wstring>& args, const fs::path& cwd, const fs::path& log,
                    double timeout_sec) {
    ProcResult res;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) return res;
    HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hIn != INVALID_HANDLE_VALUE) ? hIn : nullptr;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    std::wstring cmd = join_cmdline(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    auto t0 = std::chrono::steady_clock::now();
    BOOL ok =
        CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                       nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(hLog);
    if (hIn != INVALID_HANDLE_VALUE) CloseHandle(hIn);
    if (!ok) return res;
    res.spawned = true;
    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)(timeout_sec * 1000.0));
    res.sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 999);
        WaitForSingleObject(pi.hProcess, 5000);
        res.timed_out = true;
        res.rc = 999;
    } else {
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(pi.hProcess, &code);
        res.rc = (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
}

std::string read_log(const fs::path& log) {
    std::ifstream f(log, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string first_nonempty_line(const fs::path& log) {
    std::istringstream is(read_log(log));
    std::string line;
    while (std::getline(is, line)) {
        std::string t = trim(line);
        if (!t.empty()) return t;
    }
    return {};
}

void log_tail(const fs::path& log) {
    std::string s = read_log(log);
    if (s.size() > 600) s = s.substr(s.size() - 600);
    std::fprintf(stdout, "%s\n", s.c_str());
}

// ─── CPython MT19937 port (corpus byte-parity with the dev generators) ──────

class PyMt {
public:
    // Candidate init/step variants; resolve_prng() identifies the one
    // matching CPython (see g_variant below).
    void seed(uint32_t s, int variant) {
        const bool use_array = (variant % 2) == 0;
        const bool twist_first = (variant / 2) == 1;
        if (use_array) {
            // init_by_array = init_genrand(19650218) first, then mix the key.
            mt_[0] = 19650218u;
            for (int i = 1; i < 624; i++)
                mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + (uint32_t)i;
            int i = 1;
            uint32_t j = 0;
            for (int k = 624; k; --k) { // init_by_array, single key word
                mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + s + j;
                ++i;
                ++j;
                if (i >= 624) {
                    mt_[0] = mt_[623];
                    i = 1;
                }
                if (j >= 1) j = 0;
            }
            for (int k = 623; k; --k) {
                mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
                ++i;
                if (i >= 624) {
                    mt_[0] = mt_[623];
                    i = 1;
                }
            }
            mt_[0] = 0x80000000u;
        } else { // init_genrand
            mt_[0] = s;
            for (int i = 1; i < 624; i++)
                mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + (uint32_t)i;
        }
        idx_ = twist_first ? 624 : 0;
    }
    void seed(uint32_t s) { seed(s, g_variant); }

    uint32_t genrand() {
        if (idx_ >= 624) twist();
        uint32_t y = mt_[idx_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }

    // CPython getrandbits: k<=32 keeps the HIGH bits of one draw.
    uint32_t getrandbits(int k) {
        if (k <= 0) return 0;
        if (k <= 32) return genrand() >> (32 - k);
        uint32_t acc = 0;
        int bytes = (k + 7) / 8;
        int words = (k + 31) / 32;
        std::vector<unsigned char> buf((size_t)words * 4, 0);
        for (int w = 0; w < words; w++) {
            uint32_t r = genrand();
            if (w == words - 1 && (k % 32) != 0) r >>= (32 - (k % 32));
            buf[(size_t)w * 4 + 0] = (unsigned char)(r & 0xff);
            buf[(size_t)w * 4 + 1] = (unsigned char)((r >> 8) & 0xff);
            buf[(size_t)w * 4 + 2] = (unsigned char)((r >> 16) & 0xff);
            buf[(size_t)w * 4 + 3] = (unsigned char)((r >> 24) & 0xff);
        }
        for (int i = 0; i < bytes; i++) acc |= (uint32_t)buf[i] << (8 * i);
        return acc;
    }

    // CPython _randbelow_with_getrandbits: k = n.bit_length(), reject r >= n.
    uint32_t randbelow(uint32_t n) {
        if (n == 0) return 0;
        int k = bit_length(n);
        if (k <= 0) k = 1;
        uint32_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }
    uint32_t randint(uint32_t a, uint32_t b) { return a + randbelow(b - a + 1); } // inclusive

    static int g_variant; // matched against CPython by resolve_prng()

private:
    static int bit_length(uint32_t x) {
        if (x == 0) return 0;
#if defined(_MSC_VER)
        unsigned long idx = 0;
        _BitScanReverse(&idx, x);
        return (int)idx + 1;
#else
        int c = 0;
        while (x) {
            x >>= 1;
            c++;
        }
        return c;
#endif
    }
    void twist() {
        static const uint32_t mag01[2] = {0u, 0x9908b0dfu};
        for (int kk = 0; kk < 624 - 397; kk++) {
            uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
            mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ mag01[y & 1u];
        }
        for (int kk = 624 - 397; kk < 623; kk++) {
            uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
            mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1u];
        }
        uint32_t y = (mt_[623] & 0x80000000u) | (mt_[0] & 0x7fffffffu);
        mt_[623] = mt_[396] ^ (y >> 1) ^ mag01[y & 1u];
        idx_ = 0;
    }

    uint32_t mt_[624]{};
    int idx_ = 624;
};

int PyMt::g_variant = -1; // resolved by --prng-selftest

// ─── corpora (deterministic; ported from tools/perf_vs_winrar.py) ───────────

// Verbatim from perf_vs_winrar.py:69 — note "window" occurs twice; the
// 41-entry array (duplicate included) is required for byte parity.
const std::vector<std::string>& words_v() {
    static const std::vector<std::string> w = [] {
        std::string s = "the quick open rar archive stream window huffman literal distance slot "
                        "compress block table symbol length code bit reader flush buffer sector "
                        "recovery parity shard cipher salt iterate vector match chain hash delta "
                        "filter chunk window flag header payload volume digest";
        std::vector<std::string> v;
        std::istringstream is(s);
        std::string t;
        while (is >> t) v.push_back(t);
        return v;
    }();
    return w;
}

constexpr size_t MB = 1024ull * 1024ull;

uint32_t seed_text() {
    return 20260912u;
} // gen_text (all text corpora)
uint32_t seed_rand_canon() {
    return 20260913u;
}
uint32_t seed_rand_big() {
    return 20260916u;
}
uint32_t seed_rand_mix0() {
    return 20260915u;
} // bin_{i}: 20260915+i
uint32_t seed_rand_mix1() {
    return 20260930u;
} // img_{i}: 20260930+i
// Synthetic code corpus (D2 deviation: dev embeds the repo src tree, a
// standalone bundle has no repo — fixed fresh seeds keep bundle runs
// self-comparable; fingerprints in the report expose the drift).
uint32_t seed_code_canon() {
    return 20270917u;
}
uint32_t seed_code_big() {
    return 20270918u;
}
uint32_t seed_code_mix(uint32_t i) {
    return 20271000u + i;
}

void write_text_file(const fs::path& p, size_t target) {
    PyMt rng;
    rng.seed(seed_text());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const auto& W = words_v();
    size_t tell = 0;
    std::string line;
    line.reserve(256);
    while (tell < target) {
        uint32_t n = rng.randint(6, 24);
        line.clear();
        for (uint32_t i = 0; i < n; i++) {
            if (i) line += ' ';
            line += W[rng.randbelow((uint32_t)W.size())];
        }
        line += '\n';
        f.write(line.data(), (std::streamsize)line.size());
        tell += line.size();
    }
}

void write_random_file(const fs::path& p, size_t target, uint32_t seed) {
    PyMt rng;
    rng.seed(seed);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    size_t rem = target;
    unsigned char buf[4];
    while (rem > 0) {
        size_t words = rem / 4;
        rem -= words * 4;
        for (size_t i = 0; i < words; i++) {
            uint32_t r = rng.genrand();
            buf[0] = (unsigned char)(r & 0xff);
            buf[1] = (unsigned char)((r >> 8) & 0xff);
            buf[2] = (unsigned char)((r >> 16) & 0xff);
            buf[3] = (unsigned char)((r >> 24) & 0xff);
            f.write((const char*)buf, 4);
        }
        if (rem > 0) { // partial tail (never hit by current corpora sizes)
            uint32_t r = rng.genrand();
            size_t n = std::min<size_t>(4, rem);
            for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)((r >> (8 * i)) & 0xff);
            f.write((const char*)buf, (std::streamsize)n);
            rem -= n;
        }
    }
}

void write_zeros_file(const fs::path& p, size_t target) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    static const char zeros[1024 * 1024] = {0};
    size_t rem = target;
    while (rem > 0) {
        size_t n = std::min<size_t>(sizeof(zeros), rem);
        f.write(zeros, (std::streamsize)n);
        rem -= n;
    }
}

// Synthetic C++-like source (D2): source-shaped entropy (keywords,
// identifiers, punctuation, numbers) without the repo's src tree.
void write_code_file(const fs::path& p, size_t target, uint32_t seed) {
    PyMt rng;
    rng.seed(seed);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const auto& W = words_v();
    auto pick = [&]() -> const std::string& {
        return W[rng.randbelow((uint32_t)W.size())];
    };
    auto dec = [](uint32_t v) {
        return std::to_string(v);
    };
    auto hex8 = [&]() {
        std::string s;
        for (int i = 0; i < 8; i++) s += "0123456789abcdef"[rng.randbelow(16)];
        return s;
    };
    auto ident = [&]() {
        return pick() + "_" + dec(rng.randbelow(1000));
    };
    auto phrase = [&](int a, int b) {
        std::string s;
        uint32_t n = (uint32_t)a + rng.randbelow((uint32_t)(b - a + 1));
        for (uint32_t i = 0; i < n; i++) {
            if (i) s += ' ';
            s += pick();
        }
        return s;
    };
    size_t tell = 0;
    std::string b;
    while (tell < target) {
        switch (rng.randbelow(5)) {
        case 0:
            b = "// " + phrase(2, 6) + "\nstatic uint32_t " + ident() +
                "(uint32_t x, uint32_t y) {\n    uint32_t z = x ^ (y << " +
                dec(1 + rng.randbelow(31)) + ");\n    return z * 0x" + hex8() + "u;\n}\n\n";
            break;
        case 1:
            b = "struct " + ident() +
                " {\n    uint32_t count;\n    uint64_t hash;\n    char name[" +
                dec(64 + rng.randbelow(449)) + "];\n};\n\n";
            break;
        case 2:
            b = "for (size_t i = 0; i < " + dec(1 + rng.randbelow(4096)) +
                "; ++i) {\n    buf[i % " + dec(7 + rng.randbelow(1014)) + "] = " + ident() +
                "(i, 0x" + hex8() + ");\n}\n\n";
            break;
        case 3:
            b = "// " + phrase(3, 8) + "\n";
            break;
        default:
            b = "static inline const char* " + ident() +
                "(int state) {\n    if (state == " + dec(rng.randbelow(100)) + ") return \"" +
                phrase(1, 4) + "\";\n    return 0;\n}\n\n";
            break;
        }
        f.write(b.data(), (std::streamsize)b.size());
        tell += b.size();
    }
}

// many-entry corpus: sha256("openrar-many-<i>") repeated (bench_extract_entries.py:47).
std::string many_file_bytes(uint32_t i, size_t size) {
    std::string key = "openrar-many-" + std::to_string(i);
    unsigned char d[Sha256::DIGEST_SIZE];
    Sha256::compute(key.data(), key.size(), d);
    std::string out;
    out.reserve(size);
    while (out.size() < size) out.append((const char*)d, sizeof(d));
    out.resize(size);
    return out;
}

size_t gen_many(const fs::path& base) {
    constexpr int kDirs = 20, kPerDir = 30, kDeep = 10;
    constexpr size_t kFile = 4096;
    uint32_t idx = 0;
    char name[64];
    for (int d = 0; d < kDirs; d++) {
        fs::path dd = base / (std::string("d") + [&] {
                          std::string s = "00";
                          s[1] = (char)('0' + d % 10);
                          s[0] = (char)('0' + d / 10);
                          return s;
                      }());
        fs::create_directories(dd);
        for (int i = 0; i < kPerDir; i++) {
            std::snprintf(name, sizeof(name), "f%03d.bin", i);
            std::ofstream f(dd / name, std::ios::binary | std::ios::trunc);
            std::string bytes = many_file_bytes(idx++, kFile);
            f.write(bytes.data(), (std::streamsize)bytes.size());
        }
    }
    fs::path deep = base / "deep" / "a" / "b" / "c";
    fs::create_directories(deep);
    for (int i = 0; i < kDeep; i++) {
        std::snprintf(name, sizeof(name), "deep%02d.bin", i);
        std::ofstream f(deep / name, std::ios::binary | std::ios::trunc);
        std::string bytes = many_file_bytes(idx++, kFile);
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    return idx;
}

// ─── tree hashing (dev tree_hash: sorted key bytes + per-file sha256) ───────

// 16-hex fingerprint/verification hash over sorted relative-path keys.
// Flat corpora => keys are basenames => byte-identical to dev tree_hash().
std::string tree_hash_dir(const fs::path& base, bool exclude_marker = false) {
    std::vector<std::pair<std::string, fs::path>> items;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(base, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string key = it->path().lexically_relative(base).string();
        if (exclude_marker && key == ".generated") continue;
        items.emplace_back(key, it->path());
    }
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    Sha256 outer;
    for (const auto& [key, path] : items) {
        outer.update(key.data(), key.size());
        Sha256 inner;
        std::ifstream f(path, std::ios::binary);
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize got = f.gcount();
            if (got > 0) inner.update(buf.data(), (size_t)got);
        }
        unsigned char d[Sha256::DIGEST_SIZE];
        inner.finish(d);
        outer.update(d, sizeof(d));
    }
    unsigned char d[Sha256::DIGEST_SIZE];
    outer.finish(d);
    return to_hex(d, sizeof(d), 8); // 16 hex chars
}

long long dir_size(const fs::path& base) {
    long long total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(base, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += (long long)it->file_size(ec);
    }
    return total;
}

// ─── host / tool disclosure ─────────────────────────────────────────────────

struct Host {
    std::string cpu = "unknown";
    int cores = 0;
    double ram_gb = 0.0;
    std::string os = "windows";
};

Host detect_host() {
    Host h;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int regs[4] = {0};
    char brand[49] = {0};
    __cpuid(regs, (int)0x80000000u);
    if ((unsigned)regs[0] >= 0x80000004u) {
        __cpuid(reinterpret_cast<int*>(brand), (int)0x80000002u);
        __cpuid(reinterpret_cast<int*>(brand + 16), (int)0x80000003u);
        __cpuid(reinterpret_cast<int*>(brand + 32), (int)0x80000004u);
        std::string s(brand);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        if (!s.empty()) h.cpu = s;
    }
#endif
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    h.cores = (int)si.dwNumberOfProcessors;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        h.ram_gb = std::round((double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0;
    wchar_t buf[256] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"ProductName", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        h.os = narrow(buf);
        sz = sizeof(buf);
        wchar_t build[64] = {0};
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build,
                         &sz) == ERROR_SUCCESS) {
            h.os += " build " + narrow(build);
        }
    }
    return h;
}

// ─── engines ────────────────────────────────────────────────────────────────

struct Engine {
    std::string name;
    fs::path exe;
    bool found = false;
    bool requested = true;
};

struct Engines {
    Engine openrar{"openrar"};
    Engine unrar{"unrar"};
    Engine winrar{"winrar"};
    Engine* by_name(const std::string& n) {
        if (n == "openrar") return &openrar;
        if (n == "unrar") return &unrar;
        if (n == "winrar") return &winrar;
        return nullptr;
    }
};

fs::path env_path(const wchar_t* var) {
    DWORD n = GetEnvironmentVariableW(var, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    GetEnvironmentVariableW(var, v.data(), n);
    v.resize(wcslen(v.c_str()));
    fs::path p(v);
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    return {};
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

fs::path find_openrar(const fs::path& exe_dir) {
    if (fs::path p = env_path(L"OPENRAR_EXE"); !p.empty()) return p;
    if (file_exists(exe_dir / L"openrar.exe")) return exe_dir / L"openrar.exe";
    fs::path p = exe_dir;
    for (int d = 0; d < 7 && p != p.parent_path(); d++, p = p.parent_path()) {
        if (file_exists(p / L"build" / L"openrar64" / L"Release" / L"openrar.exe"))
            return p / L"build" / L"openrar64" / L"Release" / L"openrar.exe";
        if (file_exists(p / L"build" / L"Release" / L"openrar.exe"))
            return p / L"build" / L"Release" / L"openrar.exe";
        if (file_exists(p / L"openrar.exe")) return p / L"openrar.exe";
    }
    return {};
}

fs::path find_unrar(const fs::path& exe_dir) {
    if (fs::path p = env_path(L"UNRAR_EXE"); !p.empty()) return p;
    if (file_exists(exe_dir / L"UnRAR.exe")) return exe_dir / L"UnRAR.exe";
    fs::path p = exe_dir;
    for (int d = 0; d < 7 && p != p.parent_path(); d++, p = p.parent_path()) {
        fs::path sib =
            p.parent_path() / L"unrar" / L"build" / L"unrar64" / L"Release" / L"UnRAR.exe";
        if (file_exists(sib)) return sib;
    }
    for (const wchar_t* base :
         {L"C:\\Program Files\\WinRAR\\UnRAR.exe", L"C:\\Program Files (x86)\\WinRAR\\UnRAR.exe"})
        if (file_exists(base)) return base;
    return {};
}

fs::path find_winrar(const fs::path& exe_dir) {
    if (fs::path p = env_path(L"RAR_EXE"); !p.empty()) return p;
    if (file_exists(exe_dir / L"rar.exe")) return exe_dir / L"rar.exe";
    for (const wchar_t* base :
         {L"C:\\Program Files\\WinRAR\\rar.exe", L"C:\\Program Files (x86)\\WinRAR\\rar.exe"})
        if (file_exists(base)) return base;
    return {};
}

// ─── report ─────────────────────────────────────────────────────────────────

struct Row {
    std::string engine, label, corpus, status = "ok", note;
    std::string config; // many-entry section only
    std::vector<double> runs;
    double median = 0.0, minv = 0.0, spread = 0.0;
    long long size = 0;
    double input_mb = 0.0;
    bool has_input_mb = false;
    std::vector<std::string> verified;
};

struct Failure {
    std::string label, stage, detail;
};

struct Report {
    std::string run_id;
    bool quick = false;
    Host host;
    std::string ver_openrar, ver_unrar, ver_winrar;
    int warmup = 1, timed = 3;
    bool runs_flag = false;
    std::map<std::string, std::string> corpora;
    std::vector<Row> rows;
    std::vector<Failure> failures;
    std::string status = "running"; // running | ok | failed | fatal | gen-only
    std::string note;
    std::string engines_line;
};

std::string json_num(double v, int prec) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

std::string build_json(const Report& r) {
    std::ostringstream o;
    o << "{\"schema_version\":1";
    o << ",\"run_id\":\"" << json_escape(r.run_id) << "\"";
    o << ",\"quick\":" << (r.quick ? "true" : "false");
    o << ",\"status\":\"" << json_escape(r.status) << "\"";
    if (!r.note.empty()) o << ",\"note\":\"" << json_escape(r.note) << "\"";
    o << ",\"host\":{\"cpu\":\"" << json_escape(r.host.cpu) << "\",\"cores\":" << r.host.cores
      << ",\"ram_gb\":" << json_num(r.host.ram_gb, 1) << ",\"os\":\"" << json_escape(r.host.os)
      << "\"}";
    o << ",\"tools\":{\"openrar\":\"" << json_escape(r.ver_openrar) << "\",\"unrar\":\""
      << json_escape(r.ver_unrar) << "\",\"winrar\":\"" << json_escape(r.ver_winrar) << "\"}";
    o << ",\"protocol\":{\"warmup\":" << r.warmup << ",\"timed\":" << r.timed
      << ",\"quick\":" << (r.quick ? "true" : "false")
      << ",\"runs_flag\":" << (r.runs_flag ? "true" : "false")
      << ",\"spread_metric\":\"(max-min)/median\",\"mt_switch\":\"-mt4\"}";
    o << ",\"engines\":\"" << json_escape(r.engines_line) << "\"";
    o << ",\"corpora\":{";
    {
        bool first = true;
        for (const auto& [k, v] : r.corpora) {
            if (!first) o << ",";
            first = false;
            o << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
        }
    }
    o << "}";
    o << ",\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); i++) {
        const Row& w = r.rows[i];
        if (i) o << ",";
        o << "\n{\"engine\":\"" << json_escape(w.engine) << "\",\"label\":\""
          << json_escape(w.label) << "\",\"corpus\":\"" << json_escape(w.corpus)
          << "\",\"status\":\"" << json_escape(w.status) << "\"";
        if (!w.note.empty()) o << ",\"note\":\"" << json_escape(w.note) << "\"";
        if (!w.config.empty()) o << ",\"config\":\"" << json_escape(w.config) << "\"";
        o << ",\"runs\":[";
        for (size_t j = 0; j < w.runs.size(); j++) {
            if (j) o << ",";
            o << json_num(w.runs[j], 6);
        }
        o << "],\"median\":" << json_num(w.median, 6) << ",\"min\":" << json_num(w.minv, 6)
          << ",\"spread\":" << json_num(w.spread, 6) << ",\"size\":" << w.size;
        if (w.has_input_mb) o << ",\"input_mb\":" << json_num(w.input_mb, 1);
        if (!w.verified.empty()) {
            o << ",\"verified\":[";
            for (size_t j = 0; j < w.verified.size(); j++) {
                if (j) o << ",";
                o << "\"" << json_escape(w.verified[j]) << "\"";
            }
            o << "]";
        }
        o << "}";
    }
    o << "]";
    o << ",\"failures\":[";
    for (size_t i = 0; i < r.failures.size(); i++) {
        if (i) o << ",";
        o << "{\"label\":\"" << json_escape(r.failures[i].label) << "\",\"stage\":\""
          << json_escape(r.failures[i].stage) << "\",\"detail\":\""
          << json_escape(r.failures[i].detail) << "\"}";
    }
    o << "]";
    size_t ok = 0, failed = 0, skipped = 0;
    for (const auto& w : r.rows) {
        if (w.status == "ok")
            ok++;
        else if (w.status == "failed")
            failed++;
        else
            skipped++;
    }
    o << ",\"summary\":{\"rows\":" << r.rows.size() << ",\"ok\":" << ok << ",\"failed\":" << failed
      << ",\"skipped\":" << skipped << "}";
    o << "}\n";
    return o.str();
}

bool write_report_atomic(const Report& r, const fs::path& out) {
    std::error_code ec;
    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
    fs::path tmp = out;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        std::string s = build_json(r);
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return false;
    }
    fs::rename(tmp, out, ec);
    if (!ec) return true;
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    return !ec;
}

// ─── options ────────────────────────────────────────────────────────────────

struct Options {
    bool quick = false;
    bool keep = false;
    bool no_pause = false;
    bool gen_only = false;
    bool help = false;
    bool prng_selftest = false;
    bool runs_set = false; // --runs was passed explicitly
    int runs = -1;         // timed runs override
    fs::path out;
    std::vector<std::string> engines{"openrar", "unrar", "winrar"};
};

void usage() {
    std::printf("runbench - OpenRAR benchmark bundle driver\n"
                "\n"
                "  --quick               smoke: 2 timed runs, no warm-up, skip 1 GB section\n"
                "  --runs N              timed runs per config (default: 3, quick 2)\n"
                "  --engines LIST        subset of openrar,unrar,winrar (default all)\n"
                "  --out PATH            report path (default "
                "%%USERPROFILE%%\\openrar-bench-results.json)\n"
                "  --keep                keep the work directory (corpora + archives)\n"
                "  --no-pause            do not wait for Enter at exit\n"
                "  --gen-only            generate corpora, print fingerprints, run nothing\n"
                "  --prng-selftest       validate the MT19937 port against CPython vectors\n"
                "  --help                this text\n");
}

bool parse(const int argc, char** argv, Options& o) {
    o.out.clear();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick")
            o.quick = true;
        else if (a == "--keep")
            o.keep = true;
        else if (a == "--no-pause")
            o.no_pause = true;
        else if (a == "--gen-only")
            o.gen_only = true;
        else if (a == "--help" || a == "-h")
            o.help = true;
        else if (a == "--prng-selftest")
            o.prng_selftest = true;
        else if (a == "--runs" && i + 1 < argc) {
            o.runs = std::atoi(argv[++i]);
            o.runs_set = true;
        } else if (a == "--out" && i + 1 < argc)
            o.out = fs::path(argv[++i]);
        else if (a == "--engines" && i + 1 < argc) {
            o.engines.clear();
            std::stringstream ss(argv[++i]);
            std::string t;
            while (std::getline(ss, t, ',')) {
                if (!t.empty()) o.engines.push_back(t);
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (o.runs < 0) o.runs = o.quick ? 2 : 3;
    if (o.out.empty()) {
        wchar_t prof[32768] = {0};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, (DWORD)std::size(prof));
        fs::path base = n > 0 ? fs::path(prof) : fs::current_path();
        o.out = base / L"openrar-bench-results.json";
    }
    // openrar is always required.
    if (std::find(o.engines.begin(), o.engines.end(), "openrar") == o.engines.end())
        o.engines.insert(o.engines.begin(), "openrar");
    return true;
}

// ─── PRNG selftest (CPython ground truth captured from python3) ────────────

int prng_selftest(bool verbose) {
    static const uint32_t v1[6] = {266925430u, 2935605626u, 1852667266u,
                                   922643357u, 513105137u,  128244345u};
    static const uint32_t v2[4] = {1768413672u, 3168271198u, 2670327695u, 2012949514u};
    static const char* v3 = "92017218903a429f1ec152e73946e0c856aa95305a9f2ae8590e943b962e54d0";
    static const int v4[10] = {7, 19, 12, 9, 6, 24, 21, 8, 12, 8};
    // random.Random(20260912).choice(WORDS) x8 — capture: ['rar', 'iterate', 'table',
    // 'huffman', 'quick', 'volume', 'flag', 'payload']
    static const char* v5[8] = {"rar",   "iterate", "table", "huffman",
                                "quick", "volume",  "flag",  "payload"};

    int matched = -1;
    for (int variant = 0; variant < 4; variant++) {
        const char* fail = nullptr;
        PyMt r;
        r.seed(20260912u, variant);
        for (int i = 0; i < 6 && !fail; i++)
            if (r.getrandbits(32) != v1[i]) fail = "v1 getrandbits(32)";
        r.seed(20260914u, variant);
        for (int i = 0; i < 4 && !fail; i++)
            if (r.getrandbits(32) != v2[i]) fail = "v2 getrandbits(32)";
        r.seed(20260913u, variant);
        if (!fail) {
            std::string s;
            for (int w = 0; w < 8 && !fail; w++) {
                uint32_t x = r.genrand();
                char b[9];
                std::snprintf(b, sizeof(b), "%02x%02x%02x%02x", x & 0xff, (x >> 8) & 0xff,
                              (x >> 16) & 0xff, (x >> 24) & 0xff);
                s += b;
            }
            if (s != v3) fail = "v3 randbytes(32)";
        }
        r.seed(20260912u, variant);
        const auto& W = words_v();
        if (!fail && W.size() != 41) fail = "WORDS size"; // dev WORDS (duplicate "window")
        static const char* probe[7] = {"the",     "quick",  "open",  "rar",
                                       "archive", "stream", "window"};
        if (!fail)
            for (int i = 0; i < 7 && !fail; i++)
                if (W[i] != probe[i]) fail = "WORDS content";
        for (int i = 0; i < 10 && !fail; i++)
            if ((int)r.randint(6, 24) != v4[i]) fail = "v4 randint(6,24)";
        r.seed(20260912u, variant);
        for (int i = 0; i < 8 && !fail; i++) {
            uint32_t idx = r.randbelow((uint32_t)W.size());
            if (W[idx] != v5[i]) fail = "v5 choice(WORDS)";
        }
        if (verbose)
            std::printf("variant %d (init=%s, %s): %s\n", variant,
                        (variant % 2) == 0 ? "init_by_array" : "init_genrand",
                        (variant / 2) == 1 ? "twist-first" : "direct", fail ? fail : "PASS");
        if (!fail && matched < 0) matched = variant;
    }
    if (matched < 0) {
        std::fprintf(stderr, "prng selftest: NO variant matches CPython\n");
        return 1;
    }
    if (verbose) std::printf("prng selftest: using variant %d\n", matched);
    PyMt::g_variant = matched;
    return 0;
}

// ─── bench context ──────────────────────────────────────────────────────────

struct Ctx {
    Options opt;
    Engines eng;
    Report rep;
    fs::path work, corpora, out_dir, tmp, child_log;
    std::map<std::string, std::string> src_hash; // corpus -> 16hex
    std::map<std::string, long long> src_bytes;
    int warmup = 1;
    double timeout_sec = 1800.0;

    Engine& engine(const std::string& n) { return *eng.by_name(n); }
    fs::path corpus_dir(const std::string& c) { return corpora / c; }

    const std::string& corpus_src_hash(const std::string& c) {
        auto it = src_hash.find(c);
        if (it == src_hash.end()) {
            std::string h = tree_hash_dir(corpus_dir(c));
            it = src_hash.emplace(c, h).first;
        }
        return it->second;
    }
    long long corpus_bytes(const std::string& c) {
        auto it = src_bytes.find(c);
        if (it == src_bytes.end()) {
            long long sz = dir_size(corpus_dir(c));
            it = src_bytes.emplace(c, sz).first;
        }
        return it->second;
    }
    void failure(const std::string& label, const std::string& stage, const std::string& detail) {
        rep.failures.push_back({label, stage, detail});
        std::fprintf(stdout, "  FAIL [%s/%s]: %s\n", label.c_str(), stage.c_str(), detail.c_str());
    }
    void save() { write_report_atomic(rep, opt.out); }
};

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void finalize_row(Row& r) {
    if (r.runs.empty()) return;
    double lo = *std::min_element(r.runs.begin(), r.runs.end());
    double hi = *std::max_element(r.runs.begin(), r.runs.end());
    r.minv = lo;
    r.median = median_of(r.runs);
    r.spread = (r.runs.size() > 1 && r.median > 0.0) ? (hi - lo) / r.median : 0.0;
}

std::vector<std::wstring> split_out_files(const fs::path& out_path) {
    // files matching stem* + ".rar" (volumes included), sorted by name
    std::vector<fs::path> found;
    std::error_code ec;
    std::string stem = out_path.stem().string();
    for (auto it = fs::directory_iterator(out_path.parent_path(), ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".rar") == 0 &&
            name.compare(0, stem.size(), stem) == 0)
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    std::vector<std::wstring> w;
    for (auto& p : found) w.push_back(p.wstring());
    return w;
}

void remove_out_files(const fs::path& out_path) {
    for (const auto& w : split_out_files(out_path)) {
        std::error_code ec;
        fs::remove(w, ec);
    }
}

fs::path pick_primary(const std::vector<std::wstring>& files) {
    for (const auto& f : files)
        if (f.find(L".part01.") != std::wstring::npos) return fs::path(f);
    return files.empty() ? fs::path() : fs::path(files.front());
}

// ─── compression config bench ───────────────────────────────────────────────

Row bench_compress(Ctx& cx, const std::string& engine_name, const std::string& corpus,
                   const std::vector<std::string>& files, const std::string& label,
                   const std::vector<std::wstring>& switches) {
    Row row;
    row.engine = engine_name;
    row.label = label;
    row.corpus = corpus;

    Engine& e = cx.engine(engine_name);
    if (!e.requested) {
        row.status = "skipped";
        row.note = "engine not selected";
        return row;
    }
    if (!e.found) {
        row.status = "skipped";
        row.note = "engine executable not found";
        return row;
    }

    fs::path out = cx.out_dir / (corpus + "_" + label + ".rar");
    fs::path cwd = cx.corpus_dir(corpus);
    std::vector<std::wstring> argv;
    argv.push_back(e.exe.wstring());
    if (engine_name == "openrar") {
        argv.push_back(L"a");
        argv.push_back(L"-q");
    } else {
        argv.push_back(L"a");
        argv.push_back(L"-inul");
        argv.push_back(L"-y");
    }
    for (const auto& s : switches) argv.push_back(s);
    argv.push_back(out.wstring());
    for (const auto& f : files) argv.push_back(widen(f));

    std::printf("[%s] compress %s %s\n", label.c_str(), corpus.c_str(), engine_name.c_str());
    int total = cx.warmup + cx.opt.runs;
    for (int i = 0; i < total; i++) {
        remove_out_files(out); // pre-deleted OUTSIDE the timed window
        ProcResult pr = run_proc(argv, cwd, cx.child_log, cx.timeout_sec);
        if (!pr.spawned) {
            row.status = "failed";
            row.note = "spawn failed";
            cx.failure(label, "spawn", narrow(e.exe.wstring()));
            finalize_row(row);
            return row;
        }
        if (pr.timed_out || pr.rc != 0) {
            row.status = "failed";
            row.note = pr.timed_out ? "timeout" : ("child rc=" + std::to_string(pr.rc));
            log_tail(cx.child_log);
            cx.failure(label, pr.timed_out ? "timeout" : "rc", row.note);
            finalize_row(row);
            return row;
        }
        bool counted = i >= cx.warmup;
        if (counted) row.runs.push_back(pr.sec);
        std::printf("    run%d%s: %.2fs\n", i, counted ? "" : " (warm-up)", pr.sec);
    }

    auto parts = split_out_files(out);
    for (const auto& w : parts) {
        std::error_code ec;
        row.size += (long long)fs::file_size(w, ec);
    }
    row.input_mb = (double)cx.corpus_bytes(corpus) / (double)MB;
    row.has_input_mb = true;

    // cross-extraction verification (openrar + unrar, when available)
    std::string src = cx.corpus_src_hash(corpus);
    fs::path primary = pick_primary(parts);
    if (parts.empty()) {
        row.status = "failed";
        row.note = "no archive produced";
        cx.failure(label, "pack", "no archive produced");
        finalize_row(row);
        return row;
    }
    for (const std::string& ve : {"openrar", "unrar"}) {
        Engine& ve_e = cx.engine(ve);
        if (!ve_e.requested || !ve_e.found) continue;
        fs::path dest = cx.tmp / ("x_" + label + "_" + ve);
        std::error_code ec;
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);
        std::vector<std::wstring> xargv;
        xargv.push_back(ve_e.exe.wstring());
        xargv.push_back(L"x");
        xargv.push_back(L"-y");
        if (ve == "openrar") {
            xargv.push_back(L"-q");
        } else {
            // unrar verification: dev extract_and_hash unrar shape
        }
        xargv.push_back(primary.wstring());
        std::wstring destw = dest.wstring();
        if (ve != "openrar") destw += L"\\";
        xargv.push_back(destw);
        ProcResult pr = run_proc(xargv, {}, cx.child_log, cx.timeout_sec);
        std::string got = pr.spawned && pr.rc == 0 ? tree_hash_dir(dest) : std::string();
        fs::remove_all(dest, ec);
        if (got.empty() || got != src) {
            row.status = "failed";
            row.note = "verify mismatch (" + ve + ")";
            cx.failure(label, "verify", ve + " extracted hash " + got + " != source " + src);
        } else {
            row.verified.push_back(got);
        }
    }
    std::sort(row.verified.begin(), row.verified.end());
    finalize_row(row);
    return row;
}

// ─── extraction section (H) ─────────────────────────────────────────────────

Row bench_extract(Ctx& cx, const std::string& engine_name, const fs::path& arc,
                  const std::string& label, const std::vector<std::wstring>& extra,
                  bool trailing_sep) {
    Row row;
    row.engine = engine_name;
    row.label = label;
    row.corpus = "canonical";

    Engine& e = cx.engine(engine_name);
    if (!e.requested) {
        row.status = "skipped";
        row.note = "engine not selected";
        return row;
    }
    if (!e.found) {
        row.status = "skipped";
        row.note = "engine executable not found";
        return row;
    }
    if (!file_exists(arc)) {
        row.status = "skipped";
        row.note = "archive missing (" + narrow(arc.wstring()) + ")";
        return row;
    }

    std::vector<std::wstring> argv;
    argv.push_back(e.exe.wstring());
    argv.push_back(L"x");
    argv.push_back(L"-y");
    if (engine_name == "openrar") argv.push_back(L"-q");
    if (engine_name == "winrar") argv.push_back(L"-inul");
    for (const auto& s : extra) argv.push_back(s);
    argv.push_back(arc.wstring());

    std::printf("[%s] extract %s\n", label.c_str(), arc.filename().string().c_str());
    std::string src = cx.corpus_src_hash("canonical");
    std::string seen_hash;
    int total = cx.warmup + cx.opt.runs;
    for (int i = 0; i < total; i++) {
        fs::path dest = cx.tmp / ("ex_" + label + "_" + std::to_string(i));
        std::error_code ec;
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);
        std::vector<std::wstring> full = argv;
        std::wstring destw = dest.wstring();
        if (trailing_sep) destw += L"\\";
        full.push_back(destw);
        ProcResult pr = run_proc(full, {}, cx.child_log, cx.timeout_sec);
        bool counted = i >= cx.warmup;
        if (pr.spawned && !pr.timed_out && pr.rc == 0) {
            std::string got = tree_hash_dir(dest);
            if (got != src) {
                row.status = "failed";
                row.note = "verify mismatch";
                cx.failure(label, "verify", "extracted hash " + got + " != source " + src);
            } else if (seen_hash.empty()) {
                seen_hash = got;
            }
        } else {
            row.status = "failed";
            row.note = pr.timed_out ? "timeout" : ("child rc=" + std::to_string(pr.rc));
            log_tail(cx.child_log);
            cx.failure(label, pr.timed_out ? "timeout" : "rc", row.note);
        }
        if (counted) row.runs.push_back(pr.sec);
        std::printf("    run%d%s: %.3fs\n", i, counted ? "" : " (warm-up)", pr.sec);
        fs::remove_all(dest, ec);
        if (row.status == "failed") break;
    }
    if (!seen_hash.empty()) row.verified.push_back(seen_hash);
    finalize_row(row);
    return row;
}

// ─── many-entry section (I) ─────────────────────────────────────────────────

Row bench_many(Ctx& cx, const std::string& engine_name, const fs::path& arc,
               const std::string& config, const std::vector<std::wstring>& extra, bool trailing_sep,
               const std::string& src) {
    Row row;
    row.engine = engine_name;
    row.label = "many_" + config;
    row.config = config;
    row.corpus = "many";

    Engine& e = cx.engine(engine_name);
    if (!e.requested) {
        row.status = "skipped";
        row.note = "engine not selected";
        return row;
    }
    if (!e.found) {
        row.status = "skipped";
        row.note = "engine executable not found";
        return row;
    }
    if (!file_exists(arc)) {
        row.status = "skipped";
        row.note = "archive missing";
        return row;
    }

    std::vector<std::wstring> argv;
    argv.push_back(e.exe.wstring());
    argv.push_back(L"x");
    argv.push_back(L"-y");
    if (engine_name == "openrar") argv.push_back(L"-q");
    for (const auto& s : extra) argv.push_back(s);
    argv.push_back(arc.wstring());

    std::printf("[many_%s] many-entry extraction\n", config.c_str());
    int total = cx.warmup + cx.opt.runs;
    for (int i = 0; i < total; i++) {
        fs::path exroot = cx.tmp / ("many_" + config + "_" + std::to_string(i));
        fs::path deep = exroot / "a" / "b" / "c" / "d";
        std::error_code ec;
        fs::remove_all(exroot, ec);
        fs::create_directories(deep, ec);
        std::vector<std::wstring> full = argv;
        std::wstring destw = deep.wstring();
        if (trailing_sep) destw += L"\\";
        full.push_back(destw);
        ProcResult pr = run_proc(full, {}, cx.child_log, cx.timeout_sec);
        bool counted = i >= cx.warmup;
        if (pr.spawned && !pr.timed_out && pr.rc == 0) {
            std::string got = tree_hash_dir(deep);
            if (got != src) {
                row.status = "failed";
                row.note = "verify mismatch";
                cx.failure(row.label, "verify", "hash mismatch run" + std::to_string(i));
            } else if (row.verified.empty()) {
                row.verified.push_back(got);
            }
        } else {
            row.status = "failed";
            row.note = pr.timed_out ? "timeout" : ("child rc=" + std::to_string(pr.rc));
            log_tail(cx.child_log);
            cx.failure(row.label, pr.timed_out ? "timeout" : "rc", row.note);
        }
        if (counted) row.runs.push_back(pr.sec);
        std::printf("    run%d%s: %.3fs\n", i, counted ? "" : " (warm-up)", pr.sec);
        fs::remove_all(exroot, ec);
        if (row.status == "failed") break;
    }
    finalize_row(row);
    return row;
}

// ─── corpora generation ─────────────────────────────────────────────────────

void gen_one(const char* name, const fs::path& p, void (*fn)(const fs::path&, size_t),
             size_t bytes) {
    auto t0 = std::chrono::steady_clock::now();
    fn(p, bytes);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  generated %-16s %llu bytes (%.1fs)\n", name, (unsigned long long)bytes, sec);
}

bool gen_corpora(Ctx& cx, std::string& err) {
    std::error_code ec;
    fs::remove_all(cx.corpora, ec);
    fs::create_directories(cx.corpora, ec);
    if (!fs::exists(cx.corpora)) {
        err = "cannot create corpus dir " + narrow(cx.corpora.wstring());
        return false;
    }

    // canonical: 43 MB text + 43 MB synthetic code + 42 MB random
    {
        fs::path d = cx.corpora / "canonical";
        fs::create_directories(d);
        gen_one("canonical/text", d / "text.txt", write_text_file, 43ull * MB);
        write_code_file(d / "code.cpp", 43ull * MB, seed_code_canon());
        std::printf("  generated %-16s %llu bytes\n", "canonical/code",
                    (unsigned long long)fs::file_size(d / "code.cpp", ec));
        write_random_file(d / "random.bin", 42ull * MB, seed_rand_canon());
        std::printf("  generated %-16s %llu bytes\n", "canonical/random",
                    (unsigned long long)fs::file_size(d / "random.bin", ec));
    }

    // mixed: 36 files, sizes 2-6 MB, incl. duplicate pair (CDC/-oi dedup)
    {
        fs::path d = cx.corpora / "mixed";
        fs::create_directories(d);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 36; i++) {
            int kind = i % 6;
            size_t size = (size_t)(2 + (i % 5)) * MB;
            char name[64];
            if (kind == 0) {
                std::snprintf(name, sizeof(name), "doc_%02d.txt", i);
                write_text_file(d / name, size);
            } else if (kind == 1) {
                std::snprintf(name, sizeof(name), "src_%02d.cpp", i);
                write_code_file(d / name, size, seed_code_mix((uint32_t)i));
            } else if (kind == 2) {
                std::snprintf(name, sizeof(name), "bin_%02d.dat", i);
                write_random_file(d / name, size, seed_rand_mix0() + (uint32_t)i);
            } else if (kind == 3) {
                std::snprintf(name, sizeof(name), "pad_%02d.raw", i);
                write_zeros_file(d / name, size);
            } else if (kind == 4) {
                std::snprintf(name, sizeof(name), "report_%02d.log", i);
                write_text_file(d / name, size);
            } else {
                std::snprintf(name, sizeof(name), "img_%02d.bin", i);
                write_random_file(d / name, size, seed_rand_mix1() + (uint32_t)i);
            }
        }
        fs::copy_file(d / "doc_00.txt", d / "doc_00_copy.txt", fs::copy_options::overwrite_existing,
                      ec);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  generated %-16s 36 files (%.1fs)\n", "mixed/", sec);
    }

    // zeros512: single 512 MB zero file (ratio highlight)
    {
        fs::path d = cx.corpora / "zeros512";
        fs::create_directories(d);
        gen_one("zeros512/zeros", d / "zeros.bin", write_zeros_file, 512ull * MB);
    }

    // big: ~1 GB canonical thirds (headline; skipped by --quick)
    if (!cx.opt.quick) {
        fs::path d = cx.corpora / "big";
        fs::create_directories(d);
        gen_one("big/text", d / "text.txt", write_text_file, 340ull * MB);
        write_code_file(d / "code.cpp", 340ull * MB, seed_code_big());
        std::printf("  generated %-16s %llu bytes\n", "big/code",
                    (unsigned long long)fs::file_size(d / "code.cpp", ec));
        write_random_file(d / "random.bin", 344ull * MB, seed_rand_big());
        std::printf("  generated %-16s %llu bytes\n", "big/random",
                    (unsigned long long)fs::file_size(d / "random.bin", ec));
    }

    // many: 610 small files in nested dirs + deep chain (bench_extract_entries)
    {
        fs::path d = cx.corpora / "many";
        fs::create_directories(d);
        auto t0 = std::chrono::steady_clock::now();
        size_t n = gen_many(d);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  generated %-16s %zu files (%.1fs)\n", "many/", n, sec);
    }
    return true;
}

void fingerprint_corpora(Ctx& cx) {
    for (const char* c : {"canonical", "mixed", "zeros512", "many"}) {
        std::error_code ec;
        if (!fs::exists(cx.corpus_dir(c), ec)) continue;
        std::string fp = tree_hash_dir(cx.corpus_dir(c));
        cx.rep.corpora[c] = fp;
        std::printf("  fingerprint %-10s %s\n", c, fp.c_str());
    }
    if (!cx.opt.quick) {
        std::error_code ec;
        if (fs::exists(cx.corpus_dir("big"), ec)) {
            std::string fp = tree_hash_dir(cx.corpus_dir("big"));
            cx.rep.corpora["big"] = fp;
            std::printf("  fingerprint %-10s %s\n", "big", fp.c_str());
        }
    }
}

// ─── matrix ─────────────────────────────────────────────────────────────────

void add_row(Ctx& cx, Row&& row) {
    std::printf("  -> %s: %s (median %.3fs, %zu runs)\n", row.label.c_str(), row.status.c_str(),
                row.median, row.runs.size());
    cx.rep.rows.push_back(std::move(row));
    cx.save(); // incremental: a killed run still leaves feedback-ready JSON
}

int run_matrix(Ctx& cx) {
    const std::vector<std::string> CANON = {"text.txt", "code.cpp", "random.bin"};

    // A. method sweep, ST (all engines always recorded; off-engines -> skipped)
    for (uint32_t m : {0u, 1u, 3u, 5u}) {
        std::wstring ms = L"-m" + std::to_wstring(m);
        add_row(cx, bench_compress(cx, "openrar", "canonical", CANON,
                                   "o_m" + std::to_string(m) + "_st", {ms, L"-mt1"}));
        add_row(cx, bench_compress(cx, "winrar", "canonical", CANON,
                                   "w_m" + std::to_string(m) + "_st", {ms, L"-mt1"}));
    }
    // B. MT
    add_row(cx, bench_compress(cx, "openrar", "canonical", CANON, "o_m3_mt", {L"-m3", L"-mt4"}));
    add_row(cx, bench_compress(cx, "winrar", "canonical", CANON, "w_m3_mt", {L"-m3", L"-mt4"}));
    // C. solid
    add_row(cx, bench_compress(cx, "openrar", "canonical", CANON, "o_m3_solid",
                               {L"-m3", L"-s", L"-mt1"}));
    add_row(cx, bench_compress(cx, "winrar", "canonical", CANON, "w_m3_solid",
                               {L"-m3", L"-s", L"-mt1"}));
    // D. multivolume 32 MB parts (digit-free label: openrar volume naming)
    add_row(cx, bench_compress(cx, "openrar", "canonical", CANON, "o_volumes",
                               {L"-m3", L"-v32m", L"-mt1"}));
    add_row(cx, bench_compress(cx, "winrar", "canonical", CANON, "w_volumes",
                               {L"-m3", L"-v32m", L"-mt1"}));

    // E. mixed corpus, MT + CDC dedup (openrar-only)
    {
        std::vector<std::string> MIXED;
        std::error_code ec;
        for (auto it = fs::directory_iterator(cx.corpus_dir("mixed"), ec);
             !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) MIXED.push_back(it->path().filename().string());
        std::sort(MIXED.begin(), MIXED.end());
        add_row(cx,
                bench_compress(cx, "openrar", "mixed", MIXED, "o_mix_m3_mt", {L"-m3", L"-mt4"}));
        add_row(cx, bench_compress(cx, "winrar", "mixed", MIXED, "w_mix_m3_mt", {L"-m3", L"-mt4"}));
        add_row(cx, bench_compress(cx, "openrar", "mixed", MIXED, "o_mix_cdc",
                                   {L"-cdc", L"-m3", L"-mt4"}));
    }

    // F. zeros 512 MB, m1 (ratio highlight)
    add_row(cx, bench_compress(cx, "openrar", "zeros512", {"zeros.bin"}, "o_zeros_m1",
                               {L"-m1", L"-mt1"}));
    add_row(cx, bench_compress(cx, "winrar", "zeros512", {"zeros.bin"}, "w_zeros_m1",
                               {L"-m1", L"-mt1"}));

    // G. big 1 GB canonical, m3 MT (headline; skipped by --quick)
    if (!cx.opt.quick) {
        add_row(cx, bench_compress(cx, "openrar", "big", CANON, "o_big_m3_mt", {L"-m3", L"-mt4"}));
        add_row(cx, bench_compress(cx, "winrar", "big", CANON, "w_big_m3_mt", {L"-m3", L"-mt4"}));
    }

    // H. extraction of the canonical m3 ST archives, ST and MT4
    fs::path arc_o = cx.out_dir / "canonical_o_m3_st.rar";
    fs::path arc_w = cx.out_dir / "canonical_w_m3_st.rar";
    struct ExCfg {
        const char* engine;
        fs::path* arc;
        const char* label;
        std::vector<std::wstring> extra;
        bool sep;
    };
    std::vector<ExCfg> ex = {
        {"openrar", &arc_o, "extract_m3_o_st", {L"-mt1"}, false},
        {"openrar", &arc_o, "extract_m3_o_mt4", {L"-mt4"}, false},
        {"openrar", &arc_w, "extract_m3_w_st", {L"-mt1"}, false},
        {"openrar", &arc_w, "extract_m3_w_mt4", {L"-mt4"}, false},
        {"unrar", &arc_w, "extract_m3_unrar_st", {L"-mt1"}, true},
        {"unrar", &arc_w, "extract_m3_unrar_mt4", {L"-mt4"}, true},
        {"winrar", &arc_w, "extract_m3_winrar_mt4", {L"-mt4"}, true},
    };
    for (auto& c : ex) add_row(cx, bench_extract(cx, c.engine, *c.arc, c.label, c.extra, c.sep));

    // I. many-entry extraction (per-entry cost section)
    {
        fs::path d = cx.corpus_dir("many");
        fs::path arc0 = cx.out_dir / "many_m0.rar";
        fs::path arc3 = cx.out_dir / "many_m3.rar";
        // pack once (untimed): directory args preserve relative paths
        std::vector<std::wstring> dirs;
        std::error_code ec;
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator();
             it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(it->path().filename().wstring());
        std::sort(dirs.begin(), dirs.end());
        Engine& oe = cx.engine("openrar");
        auto pack = [&](const fs::path& arc, int m) {
            std::vector<std::wstring> argv{oe.exe.wstring(),           L"a",    L"-q",
                                           L"-m" + std::to_wstring(m), L"-mt1", arc.wstring()};
            for (auto& dn : dirs) argv.push_back(dn);
            remove_out_files(arc);
            ProcResult pr = run_proc(argv, d, cx.child_log, cx.timeout_sec);
            if (!pr.spawned || pr.rc != 0) {
                cx.failure(narrow(arc.filename().wstring()), "pack",
                           pr.spawned ? ("rc=" + std::to_string(pr.rc)) : "spawn failed");
                log_tail(cx.child_log);
                return false;
            }
            return true;
        };
        bool p0 = pack(arc0, 0);
        bool p3 = pack(arc3, 3);
        std::string src = cx.corpus_src_hash("many");
        if (p0) {
            add_row(cx, bench_many(cx, "openrar", arc0, "m0_entry", {L"-mt1"}, false, src));
            add_row(cx, bench_many(cx, "openrar", arc0, "m0_batch", {L"-mt1", L"-db"}, false, src));
        }
        if (p3) {
            add_row(cx, bench_many(cx, "openrar", arc3, "m3_entry", {L"-mt1"}, false, src));
            add_row(cx, bench_many(cx, "openrar", arc3, "m3_batch", {L"-mt1", L"-db"}, false, src));
            add_row(cx, bench_many(cx, "unrar", arc3, "m3_unrar", {L"-mt1"}, true, src));
        }
    }
    return 0;
}

// ─── main ───────────────────────────────────────────────────────────────────

struct WorkDir {
    fs::path path;
    bool keep = false;
    bool done = false;
    void cleanup() {
        if (done || keep || path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
        done = !fs::exists(path, ec);
        std::printf("[runbench] work dir %s: %s\n", done ? "removed" : "remove failed",
                    narrow(path.wstring()).c_str());
    }
    ~WorkDir() { cleanup(); }
};

bool stdin_is_tty() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

// Crash bookkeeping: main()'s catch and a std::terminate handler persist
// status=fatal so an exception or crash mid-run can never leave the report
// frozen at "running" (indistinguishable from a run still in progress).
Report* g_live_report = nullptr;
fs::path g_live_out;

void save_fatal_live(const char* note) {
    if (!g_live_report) return;
    try {
        g_live_report->status = "fatal";
        g_live_report->note = note;
        write_report_atomic(*g_live_report, g_live_out);
    } catch (...) {
        // best-effort only; never throw out of a crash path
    }
}

int runbench_main(const Options& opt) {
    std::string run_id;
    {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        char b[32];
        std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
        run_id = b;
    }

    Report rep;
    rep.run_id = run_id;
    rep.quick = opt.quick;
    rep.warmup = opt.quick ? 0 : 1;
    rep.timed = opt.runs;
    rep.runs_flag = opt.runs_set;
    rep.status = "running";

    // engines
    fs::path exe = self_exe();
    fs::path exe_dir = exe.parent_path();
    Engines eng;
    struct Map {
        Engine* e;
        fs::path found;
    } maps[] = {{&eng.openrar, find_openrar(exe_dir)},
                {&eng.unrar, find_unrar(exe_dir)},
                {&eng.winrar, find_winrar(exe_dir)}};
    std::string engines_line;
    for (auto& m : maps) {
        m.e->requested =
            std::find(opt.engines.begin(), opt.engines.end(), m.e->name) != opt.engines.end();
        m.e->exe = m.found;
        m.e->found = !m.found.empty();
        engines_line += (engines_line.empty() ? "" : ",");
        engines_line += m.e->name;
        if (!m.e->requested)
            engines_line += "(off)";
        else if (!m.e->found)
            engines_line += "(missing)";
    }
    rep.engines_line = engines_line;

    if (!eng.openrar.found) {
        rep.status = "fatal";
        rep.note = "openrar.exe not found (build Release, or set OPENRAR_EXE)";
        rep.host = detect_host();
        write_report_atomic(rep, opt.out);
        std::fprintf(stderr, "[runbench] FATAL: %s\n", rep.note.c_str());
        std::fprintf(stderr, "          report: %s\n", narrow(opt.out.wstring()).c_str());
        return 2;
    }

    // work dir: beside the exe (SFX temp dir when bundled), TEMP fallback
    WorkDir wd;
    wd.keep = opt.keep;
    std::error_code ec;
    wd.path = exe_dir / L"bench-work";
    fs::remove_all(wd.path, ec);
    fs::create_directories(wd.path, ec);
    if (!fs::exists(wd.path)) {
        wchar_t tbuf[32768] = {0};
        DWORD tn = GetTempPathW((DWORD)std::size(tbuf), tbuf);
        if (tn == 0 || tn >= std::size(tbuf)) {
            rep.status = "fatal";
            rep.note = "cannot create work directory";
            write_report_atomic(rep, opt.out);
            return 2;
        }
        wd.path = fs::path(tbuf) / L"openrar-bench-work";
        fs::remove_all(wd.path, ec);
        fs::create_directories(wd.path, ec);
        if (!fs::exists(wd.path)) {
            rep.status = "fatal";
            rep.note = "cannot create work directory";
            write_report_atomic(rep, opt.out);
            return 2;
        }
    }

    Ctx cx;
    cx.opt = opt;
    cx.eng = std::move(eng);
    cx.rep = std::move(rep);
    cx.warmup = opt.quick ? 0 : 1;
    cx.work = wd.path;
    cx.corpora = cx.work / "corpora";
    cx.out_dir = cx.work / "out";
    cx.tmp = cx.work / "tmp";
    fs::create_directories(cx.out_dir, ec);
    fs::create_directories(cx.tmp, ec);
    cx.child_log = cx.tmp / "child.log";

    g_live_report = &cx.rep;
    g_live_out = opt.out;

    std::printf("[runbench] OpenRAR benchmark bundle driver (%s)\n", run_id.c_str());
    std::printf("[runbench] report: %s\n", narrow(opt.out.wstring()).c_str());
    std::printf("[runbench] engines: %s\n", engines_line.c_str());

    // host + tool versions
    cx.rep.host = detect_host();
    {
        std::vector<std::wstring> q{cx.eng.openrar.exe.wstring(), L"--version"};
        if (run_proc(q, {}, cx.child_log, 30.0).spawned)
            cx.rep.ver_openrar = first_nonempty_line(cx.child_log);
        if (cx.eng.unrar.found) {
            std::vector<std::wstring> u{cx.eng.unrar.exe.wstring()};
            run_proc(u, {}, cx.child_log, 30.0);
            cx.rep.ver_unrar = first_nonempty_line(cx.child_log);
        }
        if (cx.eng.winrar.found) {
            std::vector<std::wstring> w{cx.eng.winrar.exe.wstring()};
            run_proc(w, {}, cx.child_log, 30.0);
            cx.rep.ver_winrar = first_nonempty_line(cx.child_log);
        }
    }
    std::printf("[runbench] host: %s | %d cores | %.1f GB | %s\n", cx.rep.host.cpu.c_str(),
                cx.rep.host.cores, cx.rep.host.ram_gb, cx.rep.host.os.c_str());
    std::printf("[runbench] tools: %s | %s | %s\n", cx.rep.ver_openrar.c_str(),
                cx.rep.ver_unrar.c_str(), cx.rep.ver_winrar.c_str());
    cx.save();

    // corpora
    std::printf("[runbench] generating corpora (deterministic)...\n");
    std::string err;
    if (!gen_corpora(cx, err)) {
        cx.rep.status = "fatal";
        cx.rep.note = err;
        cx.save();
        wd.cleanup();
        return 2;
    }
    std::printf("[runbench] fingerprints:\n");
    fingerprint_corpora(cx);
    cx.save();

    if (opt.gen_only) {
        cx.rep.status = "gen-only";
        cx.rep.note = "corpora generated; no benchmarks run";
        cx.save();
        std::printf("[runbench] --gen-only done; report: %s\n", narrow(opt.out.wstring()).c_str());
        wd.cleanup();
        if (!opt.no_pause && stdin_is_tty()) {
            std::printf("Press Enter to exit...");
            std::fflush(stdout);
            (void)std::getchar();
        }
        return 0;
    }

    // matrix
    auto t0 = std::chrono::steady_clock::now();
    run_matrix(cx);
    double total_min =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;

    size_t ok = 0, failed = 0, skipped = 0;
    for (const auto& r : cx.rep.rows) {
        if (r.status == "ok")
            ok++;
        else if (r.status == "failed")
            failed++;
        else
            skipped++;
    }
    cx.rep.status = failed > 0 ? "failed" : "ok";
    cx.save();

    std::printf("[runbench] done in %.1f min: %zu rows (%zu ok, %zu failed, %zu skipped)\n",
                total_min, cx.rep.rows.size(), ok, failed, skipped);
    std::printf("[runbench] report: %s\n", narrow(opt.out.wstring()).c_str());

    wd.cleanup();
    if (failed > 0)
        std::printf("[runbench] %zu row(s) FAILED - see failures[] in the report\n", failed);

    if (!opt.no_pause && stdin_is_tty()) {
        std::printf("Press Enter to exit...");
        std::fflush(stdout);
        (void)std::getchar();
    }
    return failed > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate([]() {
        std::fprintf(stderr, "[runbench] FATAL: unhandled exception or crash\n");
        save_fatal_live("crash: unhandled exception (terminate handler)");
        std::abort();
    });
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }
    if (opt.help) {
        usage();
        return 0;
    }
    if (opt.prng_selftest) return prng_selftest(true) == 0 ? 0 : 1;
    // Pin the validated CPython variant (set by the last selftest run; when
    // selftest was not run this invocation, validate lazily ONCE here so a
    // wrong PRNG can never silently produce drifted corpora).
    if (PyMt::g_variant < 0) {
        if (prng_selftest(false) != 0) return 1;
    }
    try {
        return runbench_main(opt);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[runbench] FATAL: %s\n", ex.what());
        save_fatal_live((std::string("unhandled exception: ") + ex.what()).c_str());
        std::fprintf(stderr, "          report saved with status=fatal\n");
        return 2;
    }
}
