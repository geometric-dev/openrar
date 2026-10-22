// v1.23.0 SFX phase-pipeline tests (M4): destination containment, the
// -sfxnoexec kill switch, consent-driven phases, Delete scoping, and the
// §8 exit-code mapping. Spawn tests use the self-probe modes of sfx_tests
// (--sfx-probe-touch/--sfx-probe-exit) so no shell or external fixture
// binary is involved.

#include "../../src/archive/archive_reader.hpp"
#include "../../src/core/types.hpp"
#include "../../src/sfx/prompt_console.hpp"
#include "../../src/sfx/sfx_consent.hpp"
#include "../../src/sfx/sfx_pipeline.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar;
namespace fs = std::filesystem;

static std::string g_self_exe;

static std::vector<core::byte> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

static void write_file(const fs::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

// Builds the invocation for a self-probe child, emulator-aware on cross legs.
static void probe_invocation(const std::string& probe_args, std::string& command) {
#ifdef OPENRAR_SFX_EMULATOR
    command = std::string(OPENRAR_SFX_EMULATOR) + " \"" + g_self_exe + "\" " + probe_args;
#else
    command = "\"" + g_self_exe + "\" " + probe_args;
#endif
}

namespace {
class FakeBackend final : public sfx::IPromptBackend {
public:
    explicit FakeBackend(std::vector<sfx::ConsentAnswer> script,
                         sfx::ConsentAnswer default_answer = sfx::ConsentAnswer::Deny)
        : script_(std::move(script)), default_(default_answer) {}
    bool interactive() const override { return true; }
    sfx::ConsentAnswer ask(const sfx::ConsentRequest& req) override {
        asks_.push_back(req);
        if (cursor_ >= script_.size()) return default_;
        return script_[cursor_++];
    }
    size_t asks() const { return asks_.size(); }

private:
    std::vector<sfx::ConsentAnswer> script_;
    std::vector<sfx::ConsentRequest> asks_;
    sfx::ConsentAnswer default_;
    size_t cursor_{0};
};
} // namespace

static void test_pipeline_noexec_suppresses_and_reports() {
    std::cout << "[+] test_pipeline_noexec_suppresses_and_reports" << std::endl;
    const std::string sentinel = "build/sfx_pipe_noexec_sentinel.txt";
    fs::remove(sentinel);
    std::string probe_cmd;
    probe_invocation("--sfx-probe-touch \"" + sentinel + "\"", probe_cmd);
    std::string cmt = "Setup=" + probe_cmd + "\nDelete=*.tmp\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));
    REQUIRE(cfg.has_directives());

    sfx::PipelineOptions opts;
    opts.no_exec = true;
    opts.argv_dest = "build/sfx_pipe_dest";
    int extract_calls = 0;
    sfx::OverwriteDirective seen_overwrite = sfx::OverwriteDirective::OverwriteAll;
    auto extract = [&](const std::string& dest, sfx::OverwriteDirective ow) {
        ++extract_calls;
        seen_overwrite = ow;
        return dest.empty() ? 2 : 0;
    };

    FakeBackend be({}); // would deny everything if asked — must not be asked
    sfx::ConsolePromptBackend unused_for_engine;
    (void)unused_for_engine;
    sfx::SfxConsentEngine eng(be);
    sfx::PipelineResult r = sfx::run_sfx_pipeline(cfg, eng, opts, extract);

    REQUIRE(extract_calls == 1);                                      // extraction still ran
    REQUIRE(seen_overwrite != sfx::OverwriteDirective::OverwriteAll); // de-escalated
    REQUIRE(r.suppressed_directives == 2);                            // Setup + Delete
    bool reported = false;
    for (const auto& line : r.report)
        if (line.status == "skipped-noexec") reported = true;
    REQUIRE(reported);
    REQUIRE(!fs::exists(sentinel)); // no spawn happened
    fs::remove_all("build/sfx_pipe_dest");
    std::cout << "[PASS] pipeline: -sfxnoexec suppresses + reports, extraction runs" << std::endl;
}

static void test_pipeline_consent_and_exit_mapping() {
    std::cout << "[+] test_pipeline_consent_and_exit_mapping" << std::endl;
    // Setup runs a probe that exits 7 => first nonzero child code propagates.
    const std::string sentinel = "build/sfx_pipe_consent_sentinel.txt";
    fs::remove(sentinel);
    std::string probe_cmd;
    probe_invocation("--sfx-probe-exit 7", probe_cmd);
    std::string cmt = "Setup=" + probe_cmd + "\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));

    sfx::PipelineOptions opts;
    opts.argv_dest = "build/sfx_pipe_dest2";
    FakeBackend be({sfx::ConsentAnswer::Run}); // consent once
    sfx::SfxConsentEngine eng(be);
    sfx::PipelineResult r = sfx::run_sfx_pipeline(
        cfg, eng, opts, [](const std::string&, sfx::OverwriteDirective) { return 0; });
    REQUIRE(r.exit_code == 7);
    bool executed = false;
    for (const auto& line : r.report)
        if (line.phase == "setup" && line.status == "executed") executed = true;
    REQUIRE(executed);

    // Consent denied => skipped-consent, exit 0, no prompt-side effects.
    FakeBackend be2({sfx::ConsentAnswer::Deny});
    sfx::SfxConsentEngine eng2(be2);
    r = sfx::run_sfx_pipeline(cfg, eng2, opts,
                              [](const std::string&, sfx::OverwriteDirective) { return 0; });
    REQUIRE(r.exit_code == 0);
    for (const auto& line : r.report)
        if (line.phase == "setup") REQUIRE(line.status == "skipped-consent");
    std::cout << "[PASS] pipeline: child exit propagation + consent denial mapping" << std::endl;
}

static void test_pipeline_prompt_cap_aborts() {
    std::cout << "[+] test_pipeline_prompt_cap_aborts" << std::endl;
    // 9 Setup directives with cap 8: the run aborts with exit 2.
    std::string cmt;
    for (int i = 0; i < 9; ++i) {
        std::string probe_cmd;
        probe_invocation("--sfx-probe-exit 0", probe_cmd);
        cmt += "Setup=" + probe_cmd + "\n";
    }
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));
    REQUIRE(cfg.setup.size() == 9);

    sfx::PipelineOptions opts;
    opts.argv_dest = "build/sfx_pipe_dest3";
    FakeBackend be({}, sfx::ConsentAnswer::Run); // always willing
    sfx::SfxConsentEngine eng(be, /*prompt_cap=*/8);
    sfx::PipelineResult r = sfx::run_sfx_pipeline(
        cfg, eng, opts, [](const std::string&, sfx::OverwriteDirective) { return 0; });
    REQUIRE(eng.aborted());
    REQUIRE((r.aborted && r.exit_code == 2));
    std::cout << "[PASS] pipeline: prompt cap aborts the run (exit 2)" << std::endl;
}

static void test_pipeline_extraction_failure_blocks_setup() {
    std::cout << "[+] test_pipeline_extraction_failure_blocks_setup" << std::endl;
    std::string probe_cmd;
    probe_invocation("--sfx-probe-exit 0", probe_cmd);
    std::string cmt = "Setup=" + probe_cmd + "\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));

    sfx::PipelineOptions opts;
    opts.argv_dest = "build/sfx_pipe_dest4";
    FakeBackend be({}, sfx::ConsentAnswer::Run);
    sfx::SfxConsentEngine eng(be);
    sfx::PipelineResult r =
        sfx::run_sfx_pipeline(cfg, eng, opts, [](const std::string&, sfx::OverwriteDirective) {
            return 3;
        }); // CRC-fail analogue
    REQUIRE(r.exit_code == 3);
    REQUIRE(be.asks() == 0); // Setup never consented, never spawned
    std::cout << "[PASS] pipeline: extraction failure blocks Setup (exit 3)" << std::endl;
}

static void test_pipeline_delete_scoped_and_consented() {
    std::cout << "[+] test_pipeline_delete_scoped_and_consented" << std::endl;
    const fs::path dest = "build/sfx_pipe_del_dest";
    fs::remove_all(dest);
    fs::create_directories(dest / "sub");
    write_file(dest / "keep.txt", "keep");
    write_file(dest / "kill.tmp", "kill");
    write_file(dest / "sub" / "kill.tmp", "kill2");

    std::string cmt = "Delete=*.tmp\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));
    sfx::PipelineOptions opts;
    opts.argv_dest = dest.string();
    FakeBackend be({sfx::ConsentAnswer::RunAll}); // one coalesced consent
    sfx::SfxConsentEngine eng(be);
    sfx::PipelineResult r = sfx::run_sfx_pipeline(
        cfg, eng, opts, [](const std::string&, sfx::OverwriteDirective) { return 0; });

    REQUIRE(!fs::exists(dest / "kill.tmp"));
    REQUIRE(!fs::exists(dest / "sub" / "kill.tmp")); // recursive match
    REQUIRE(fs::exists(dest / "keep.txt"));          // non-matching untouched
    REQUIRE(be.asks() == 1);                         // ONE coalesced prompt
    fs::remove_all(dest);
    std::cout << "[PASS] pipeline: Delete= coalesced consent, recursive, scoped" << std::endl;
}

static void test_pipeline_path_containment() {
    std::cout << "[+] test_pipeline_path_containment" << std::endl;
    // Path= outside the user profile is refused: fallback destination is used
    // and a report line records the refusal.
#ifdef _WIN32
    const char* hostile = "C:\\Windows";
#else
    const char* hostile = "/usr";
#endif
    std::string cmt = std::string("Path=") + hostile + "\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config(bytes(cmt));
    REQUIRE(cfg.path == hostile);

    sfx::PipelineOptions opts; // no argv override
    FakeBackend be({});
    sfx::SfxConsentEngine eng(be);
    std::string seen_dest;
    sfx::PipelineResult r = sfx::run_sfx_pipeline(
        cfg, eng, opts, [&](const std::string& dest, sfx::OverwriteDirective) {
            seen_dest = dest;
            return 0;
        });
    bool refused = false;
    for (const auto& line : r.report)
        if (line.status == "refused-containment-path") refused = true;
    REQUIRE(refused);
    REQUIRE(seen_dest != hostile);
    std::cout << "[PASS] pipeline: Path= outside profile refused (containment)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-exit") == 0) {
        return std::atoi(argv[2]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--sfx-probe-sleep") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-touch") == 0) {
        std::ofstream ofs(argv[2], std::ios::binary);
        ofs << "touched";
        return 0;
    }
    if (argc > 0 && argv[0] != nullptr) g_self_exe = argv[0];
    static const openrar::test::Case k_cases[] = {
        {"test_pipeline_noexec_suppresses_and_reports", test_pipeline_noexec_suppresses_and_reports,
         false},
        {"test_pipeline_consent_and_exit_mapping", test_pipeline_consent_and_exit_mapping, false},
        {"test_pipeline_prompt_cap_aborts", test_pipeline_prompt_cap_aborts, false},
        {"test_pipeline_extraction_failure_blocks_setup",
         test_pipeline_extraction_failure_blocks_setup, false},
        {"test_pipeline_delete_scoped_and_consented", test_pipeline_delete_scoped_and_consented,
         false},
        {"test_pipeline_path_containment", test_pipeline_path_containment, false},
    };
    openrar::test::route_crt_asserts_to_stderr();
    if (!openrar::test::is_list_only(argc, argv) && !openrar::test::last_run_was_filtered())
        std::cout << "Running v1.23.0 SFX M4 pipeline tests...\n";
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All SFX M4 pipeline tests PASSED!" << std::endl;
    return rc;
}
