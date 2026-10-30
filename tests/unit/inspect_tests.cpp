#include "../../src/cli/inspect.hpp"

#include <iostream>
#include <sstream>
#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar::cli;

void test_inspect_plain_archive() {
    InspectOptions opt;
    opt.arc_path = "tests/hello5.rar";
    opt.json = true;

    std::streambuf* orig_cout = std::cout.rdbuf();
    std::stringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    int rc = inspect_archive(opt);

    std::cout.rdbuf(orig_cout);

    REQUIRE(rc == 0);
    std::string json_str = captured.str();
    REQUIRE(json_str.find("\"command\": \"inspect\"") != std::string::npos);
    REQUIRE(json_str.find("\"status\": \"OK\"") != std::string::npos);
    REQUIRE(json_str.find("\"format\": \"RAR 5.0\"") != std::string::npos);
    REQUIRE(json_str.find("\"files\": 2") != std::string::npos);

    std::cout << "[PASS] Plain archive inspected successfully.\n";
}

void test_inspect_encrypted_archive_unauthenticated() {
    InspectOptions opt;
    opt.arc_path = "tests/hello5_hp.rar";
    opt.password = "";
    opt.json = true;

    std::streambuf* orig_cout = std::cout.rdbuf();
    std::stringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    int rc = inspect_archive(opt);

    std::cout.rdbuf(orig_cout);

    // Permissive diagnostic reports PASSWORD_REQUIRED with exit code 1
    REQUIRE(rc == 1);
    std::string json_str = captured.str();
    REQUIRE(json_str.find("\"header_encrypted\": true") != std::string::npos);
    REQUIRE(json_str.find("\"cipher\": \"AES-256-CBC\"") != std::string::npos);
    REQUIRE(json_str.find("\"status\": \"PASSWORD_REQUIRED\"") != std::string::npos);

    std::cout << "[PASS] Encrypted archive forensics extracted without password.\n";
}

void test_inspect_nonexistent_archive() {
    InspectOptions opt;
    opt.arc_path = "tests/nonexistent_file_12345.rar";
    opt.json = true;

    int rc = inspect_archive(opt);
    REQUIRE(rc == 10); // EXIT_NO_FILES

    std::cout << "[PASS] Missing archive handled with EXIT_NO_FILES.\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_inspect_plain_archive", test_inspect_plain_archive, false},
    {"test_inspect_encrypted_archive_unauthenticated",
     test_inspect_encrypted_archive_unauthenticated, false},
    {"test_inspect_nonexistent_archive", test_inspect_nonexistent_archive, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All Archive Forensics & Health X-Ray Tests PASSED!\n";
    return rc;
}
