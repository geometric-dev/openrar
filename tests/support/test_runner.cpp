// In-repo unit-test runner implementation (v1.44 Verification Substrate, M0).
// Contract and console/JUnit protocol: tests/support/test_runner.hpp.
//
// v1.44 M5: run-mode dispatch is driven by the vendored doctest (third_party/
// doctest, pinned v2.4.12 — docs/SBOM.md). This TU owns the DOCTEST_CONFIG_
// IMPLEMENT half of the header: top-level runs register each selected case
// through doctest's TestCase table and execute under doctest::Context::run()
// with a custom IReporter that reproduces this runner's [RUN]/[RUNNER]
// protocol instead of doctest's default printer. The M5 assert->REQUIRE
// per-file migration therefore leaves this console/JUnit surface untouched.
// Nested dispatches (a case body calling openrar::test::run() again — the
// runner self-examination and crash-hop suites) keep the direct loop: a
// nested doctest::Context::run() inside an active one is not a supported
// doctest configuration, and the nested surface is what those suites parse
// back, so it stays byte-identical by construction.
//
// Crash containment: doctest's SEH-crash translation would let the process
// continue past a hard fault in a case; the pre-doctest runner died there.
// The bridge reporter re-aborts on is_crash so the post-wrap observable
// (process death, piggybacked rc, orphaned JUnit temp) matches the
// pre-wrap one exactly.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "test_runner.hpp"

#include "../unit/test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace openrar::test {

namespace {

bool ascii_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

std::string argv0_stem(const char* argv0) {
    std::string s = argv0 ? argv0 : "testsuite";
    size_t pos = s.find_last_of("/\\");
    if (pos != std::string::npos) s = s.substr(pos + 1);
    pos = s.find('.');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s.empty() ? "testsuite" : s;
}

std::string xml_attr(std::string v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::vector<std::string> split_filter(const std::string& raw) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t comma = raw.find(',', start);
        if (comma == std::string::npos) {
            parts.push_back(raw.substr(start));
            break;
        }
        parts.push_back(raw.substr(start, comma - start));
        start = comma + 1;
    }
    for (std::string& p : parts) {
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        while (!p.empty() && p.back() == ' ') p.pop_back();
    }
    return parts;
}

int g_depth = 0;
bool g_fielded_last_run = false;

void emit(const std::string& line, bool nested) {
    if (nested) std::cout << "[NESTED] ";
    std::cout << line << "\n";
    std::cout.flush();
}

// Fail-closed JUnit writer. Elements are streamed to a temp sibling; the
// footer is appended and the file atomically renamed onto the real path
// only after the suite completes. On abort the temp stays orphaned and the
// real path was never touched.
class JUnitWriter {
public:
    bool begin(const std::string& out_path, const std::string& suite) {
        suite_ = suite;
        out_path_ = out_path;
        std::string suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        tmp_path_ = out_path_ + ".jrun." + suffix + ".tmp";
        std::error_code ec;
        std::filesystem::remove(out_path_, ec); // stale artifact from an earlier run
        file_.open(tmp_path_, std::ios::binary | std::ios::trunc);
        if (!file_.good()) {
            std::cerr << "runner: cannot open JUnit temp file '" << tmp_path_
                      << "' (final exit will be non-zero)\n";
            return false;
        }
        file_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        file_ << "<testsuite name=\"" << xml_attr(suite_) << "\">\n";
        return true;
    }

    void add_case(const std::string& name, double seconds, const std::string& result) {
        if (!file_.is_open()) return;
        file_ << "  <testcase classname=\"" << xml_attr(suite_) << "\" name=\"" << xml_attr(name)
              << "\" time=\"" << seconds << "\">\n";
        if (result != "pass") {
            file_ << "    <failure message=\"result=" << xml_attr(result) << "\"/>\n";
        }
        file_ << "  </testcase>\n";
        ++written_;
    }

    // Writes the footer and renames the temp onto the real path. Returns
    // false when any step failed (caller makes the exit non-zero).
    bool finalize() {
        if (!file_.is_open()) return false;
        file_ << "</testsuite>\n";
        file_.flush();
        bool ok = file_.good();
        file_.close();
        if (!ok) {
            std::cerr << "runner: JUnit temp write failed ('" << tmp_path_ << "')\n";
            std::error_code ec;
            std::filesystem::remove(tmp_path_, ec);
            return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path_, out_path_, ec);
        if (ec) {
            std::cerr << "runner: atomic JUnit rename failed ('" << tmp_path_ << "' -> '"
                      << out_path_ << "'): " << ec.message() << "\n";
            std::filesystem::remove(tmp_path_, ec);
            return false;
        }
        return true;
    }

    bool active() const { return file_.is_open(); }
    unsigned written() const { return written_; }

private:
    std::ofstream file_;
    std::string out_path_;
    std::string tmp_path_;
    std::string suite_;
    unsigned written_ = 0;
};

// ── M5 doctest bridge ────────────────────────────────────────────────────
// State of the one active top-level doctest run. The reporter callbacks run
// inside doctest::Context::run() on the same thread; run() fills this,
// registers the selected cases, and consumes the totals after the run.
// Nested dispatches (g_depth > 1) take the legacy direct loop instead, so
// at most one bridge state is live at any time.
struct BridgeState {
    std::vector<const Case*> selected; // registration order == run order
    const Case* current = nullptr;     // set at test_case_start
    JUnitWriter* junit = nullptr;      // fail-closed writer (may be inactive)
    unsigned pass = 0, fail = 0, expected_fail = 0, unexpected_pass = 0;

    const Case* lookup(const char* name) const {
        for (const Case* c : selected)
            if (std::strcmp(c->name, name) == 0) return c;
        return nullptr;
    }
};

BridgeState* g_bridge = nullptr;

void bridge_complete_case(const Case* c, double secs, bool failed) {
    BridgeState& bs = *g_bridge;
    std::string result;
    if (!failed) {
        result = c->expect_fail ? "unexpected_pass" : "pass";
        if (c->expect_fail)
            ++bs.unexpected_pass;
        else
            ++bs.pass;
    } else {
        result = c->expect_fail ? "expected_fail" : "fail";
        if (c->expect_fail)
            ++bs.expected_fail;
        else
            ++bs.fail;
    }
    bs.junit->add_case(c->name, secs, result);
    emit(std::string("[RUNNER] case=\"") + c->name + "\" result=" + result +
             " time=" + std::to_string(secs),
         false);
}

class BridgeReporter : public doctest::IReporter {
public:
    explicit BridgeReporter(const doctest::ContextOptions&) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& tc) override {
        g_bridge->current = g_bridge->lookup(tc.m_name);
        emit(std::string("[RUN] ") + tc.m_name, false);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& st) override {
        const Case* c = g_bridge->current;
        if (!c) return; // unreachable: every run case is registered
        bridge_complete_case(c, st.seconds, !st.testCaseSuccess);
    }
    void test_case_exception(const doctest::TestCaseException& e) override {
        // Pre-doctest semantics: a hard fault in a case killed the process.
        // doctest's SEH translation would instead continue the run past
        // undefined state; re-abort so the post-wrap observable (process
        // death, flushed [RUN] header, orphaned JUnit temp) is unchanged.
        if (e.is_crash) std::abort();
        // Non-crash exceptions: test_case_end follows with success=false,
        // which records the case as fail.
    }
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_REPORTER("runner_bridge", 0, BridgeReporter);

} // namespace

bool name_ok(const char* name) {
    if (!name || name[0] == '\0') return false;
    for (const char* p = name; *p; ++p) {
        if (!ascii_ok(*p)) return false;
    }
    return true;
}

const Case* find_duplicate(const Case* cases, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!name_ok(cases[i].name)) return &cases[i];
    }
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (std::strcmp(cases[i].name, cases[j].name) == 0) return &cases[i];
        }
    }
    return nullptr;
}

bool glob_match(const char* pattern, const char* name) {
    if (!pattern || !name) return false;
    // Segment-by-segment over '*' delimiters: each literal segment must
    // appear cursor-anchored and in order.
    const char* p = pattern;
    const char* n = name;
    while (*p) {
        if (*p == '*') {
            ++p;
            if (*p == '\0') return true; // trailing '*' eats the rest
            // Find the next literal segment.
            const char* seg_end = p;
            while (*seg_end && *seg_end != '*') ++seg_end;
            size_t seg_len = static_cast<size_t>(seg_end - p);
            // Search for the segment from the current cursor.
            const char* hit = nullptr;
            for (const char* cand = n;; ++cand) {
                if (std::strncmp(cand, p, seg_len) == 0) {
                    hit = cand;
                    break;
                }
                if (*cand == '\0') return false;
            }
            n = hit + seg_len;
            p = seg_end;
        } else {
            if (*p != *n) return false;
            ++p;
            ++n;
        }
    }
    return *n == '\0';
}

bool last_run_was_filtered() {
    return g_fielded_last_run;
}

int run(const Case* cases, std::size_t count, int argc, char** argv) {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();

    ++g_depth;
    const bool nested = g_depth > 1;

    std::string suite = argv0_stem(argc > 0 ? argv[0] : "");
    std::vector<std::string> filters;
    bool filtered = false;
    bool list_mode = false;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        auto starts = [&](const char* flag) {
            return std::strncmp(s, flag, std::strlen(flag)) == 0;
        };
        if (starts("--test-case=")) {
            filters = split_filter(s + 12);
            filtered = true;
        } else if (starts("-tc=")) {
            filters = split_filter(s + 4);
            filtered = true;
        } else if (starts("--junit=")) {
            out_path = s + 8;
        } else if (starts("--out=")) {
            out_path = s + 6;
        } else if (std::strcmp(s, "--list-test-cases") == 0 || std::strcmp(s, "-ltc") == 0) {
            list_mode = true;
        } else if (std::strcmp(s, "--runner-help") == 0) {
            g_fielded_last_run = true; // suppress suite banners on help runs
            emit("[RUNNER] flags: --test-case= | -tc= | --list-test-cases | -ltc | "
                 "--out=<junit.xml> | --junit=<junit.xml>",
                 nested);
            --g_depth;
            return 0;
        } else if (s[0] == '-' && s[1] != '\0') {
            std::cerr << "runner: unknown flag '" << s
                      << "' (use --runner-help; file-specific args are consumed by the "
                         "suite's prehook)\n";
            --g_depth;
            return 1;
        }
        // Non-flag args are file-domain (sfx stub paths etc.) and ignored.
    }

    auto bail = [&](const std::string& msg) {
        std::cerr << "runner: " << msg << "\n";
        --g_depth;
        return 1;
    };

    if (count == 0) return bail("suite registered zero cases (G-9)");
    if (const Case* bad = find_duplicate(cases, count)) {
        return bail(std::string("case registration invalid near '") +
                    (bad->name ? bad->name : "(null)") + "' (grammar violation or duplicate)");
    }

    std::vector<const Case*> selected;
    if (filtered) {
        for (std::size_t i = 0; i < count; ++i) {
            for (const std::string& f : filters) {
                if (glob_match(f.c_str(), cases[i].name)) {
                    selected.push_back(&cases[i]);
                    break;
                }
            }
        }
        if (selected.empty()) {
            std::string head;
            for (const std::string& f : filters) head += f + ",";
            return bail("filter matched no case (G-9): " + head);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) selected.push_back(&cases[i]);
    }

    if (list_mode) {
        g_fielded_last_run = true; // suppress suite banners on listing runs
        emit("[RUNNER] suite=\"" + suite + "\" cases=" + std::to_string(selected.size()), nested);
        for (const Case* c : selected) emit(c->name, nested);
        --g_depth;
        return 0;
    }

    g_fielded_last_run = filtered;

    JUnitWriter junit;
    bool junit_ok = true;
    if (!out_path.empty()) junit_ok = junit.begin(out_path, suite);

    unsigned pass = 0, fail = 0, expected_fail = 0, unexpected_pass = 0;
    auto t_start_run = std::chrono::steady_clock::now();
    bool runner_disaster = false;

    if (g_depth == 1) {
        // Top-level run: doctest bridges the dispatch (M5). Registration
        // order is the execution order (distinct ascending __LINE__ keys;
        // doctest's default sort is file then line, stable on ties).
        BridgeState bridge;
        bridge.selected = selected;
        bridge.junit = &junit;
        g_bridge = &bridge;
        doctest::TestSuite ts;
        ts.m_test_suite = suite.c_str();
        unsigned line = 0;
        for (const Case* c : selected) {
            doctest::detail::regTest(
                doctest::detail::TestCase(c->fn, "test_runner.cpp", ++line, ts) * c->name);
        }
        const char* dtargv[] = {"openrar-test-runner", "-r=runner_bridge"};
        doctest::Context ctx;
        ctx.applyCommandLine(2, dtargv);
        int dt_rc = ctx.run();
        g_bridge = nullptr;
        if (dt_rc < 0 || dt_rc > 1) runner_disaster = true; // doctest-level failure not in counters
        pass = bridge.pass;
        fail = bridge.fail;
        expected_fail = bridge.expected_fail;
        unexpected_pass = bridge.unexpected_pass;
    } else {
        for (const Case* c : selected) {
            emit(std::string("[RUN] ") + c->name, nested);
            auto t0 = std::chrono::steady_clock::now();
            bool threw = false;
            try {
                c->fn();
            } catch (...) {
                threw = true;
            }
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();

            const bool failed = threw;
            std::string result;
            if (!failed) {
                result = c->expect_fail ? "unexpected_pass" : "pass";
                if (c->expect_fail)
                    ++unexpected_pass;
                else
                    ++pass;
            } else {
                // M0 semantics: expect_fail applies to thrown failures; an
                // assert() abort terminates the process (the [RUN] header stays
                // flushed, later cases are skipped, ctest sees the abort rc).
                result = c->expect_fail ? "expected_fail" : "fail";
                if (c->expect_fail)
                    ++expected_fail;
                else
                    ++fail;
            }
            junit.add_case(c->name, secs, result);
            emit(std::string("[RUNNER] case=\"") + c->name + "\" result=" + result +
                     " time=" + std::to_string(secs),
                 nested);
        }
    }

    auto t_end_run = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(t_end_run - t_start_run).count();

    emit(std::string("[RUNNER] summary suite=\"") + suite + "\" cases=" + std::to_string(count) +
             " selected=" + std::to_string(selected.size()) + " pass=" + std::to_string(pass) +
             " fail=" + std::to_string(fail) + " expected_fail=" + std::to_string(expected_fail) +
             " unexpected_pass=" + std::to_string(unexpected_pass) +
             " wall=" + std::to_string(wall) + "s",
         nested);

    bool rc = fail > 0 || !junit_ok || runner_disaster;
    if (junit.active()) {
        bool finalized = junit.finalize();
        emit(std::string("[RUNNER] junit entries=") + std::to_string(junit.written()) +
                 (junit_ok && finalized ? " ok" : " FAILED"),
             nested);
        if (junit_ok && !finalized) rc = true;
    } else if (!out_path.empty()) {
        rc = true; // writer never opened: fail-closed, not silently skipped
    }
    --g_depth;
    return rc ? 1 : 0;
}

int openrar_test_main(const Case* cases, std::size_t count, int argc, char** argv) {
    return run(cases, count, argc, argv);
}

} // namespace openrar::test
