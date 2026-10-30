#include "../../src/cli/diff.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar::cli;

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

static std::string fixture_path(const std::string& rel) {
    if (std::filesystem::exists(rel)) return rel;
    auto p = std::filesystem::path(OPENRAR_SOURCE_DIR) / rel;
    return p.string();
}

void test_diff_identical_archives() {
    DiffOptions opt;
    opt.arc1_path = fixture_path("tests/hello5.rar");
    opt.arc2_path = fixture_path("tests/hello5.rar");
    opt.json = true;

    std::streambuf* orig_cout = std::cout.rdbuf();
    std::stringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    int rc = diff_archives(opt);

    std::cout.rdbuf(orig_cout);

    REQUIRE(rc == 0); // Directive 3: POSIX 0 on match
    std::string json_str = captured.str();
    REQUIRE(json_str.find("\"identical\": true") != std::string::npos);
    REQUIRE(json_str.find("\"modified\": 0") != std::string::npos);

    std::cout << "[PASS] Identical archives return POSIX 0 with clean JSON.\n";
}

void test_diff_modified_archives() {
    DiffOptions opt;
    opt.arc1_path = fixture_path("tests/hello5.rar");
    opt.arc2_path = fixture_path("tests/fixtures/golden/writer/writer_compressed=comp=m3.rar");
    opt.json = true;

    std::streambuf* orig_cout = std::cout.rdbuf();
    std::stringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    int rc = diff_archives(opt);

    std::cout.rdbuf(orig_cout);

    REQUIRE(rc == 1); // Directive 3: POSIX 1 on differences
    std::string json_str = captured.str();
    REQUIRE(json_str.find("\"identical\": false") != std::string::npos);
    REQUIRE(json_str.find("\"modified\": 2") != std::string::npos);
    REQUIRE(json_str.find("\"path\": \"data.bin\"") != std::string::npos);

    std::cout << "[PASS] Modified archives return POSIX 1 with delta itemization.\n";
}

void test_diff_missing_archive_error_code() {
    DiffOptions opt;
    opt.arc1_path = "tests/hello5.rar";
    opt.arc2_path = "tests/nonexistent_12345.rar";
    opt.json = true;

    int rc = diff_archives(opt);
    REQUIRE(rc == 2); // Directive 3: POSIX 2 on operational error

    std::cout << "[PASS] Missing archive returns POSIX error code 2.\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_diff_identical_archives", test_diff_identical_archives, false},
    {"test_diff_modified_archives", test_diff_modified_archives, false},
    {"test_diff_missing_archive_error_code", test_diff_missing_archive_error_code, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All In-Archive Structural Diff Unit Tests PASSED!\n";
    return rc;
}
