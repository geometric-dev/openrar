#include "../../src/cli/json_output.hpp"

#include <iostream>
#include <string>
#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar::cli;

void test_escape_json_string_standard() {
    std::string s = "Hello \"World\"! Here is a \\ backslash.";
    std::string escaped = escape_json_string(s);
    REQUIRE(escaped == "Hello \\\"World\\\"! Here is a \\\\ backslash.");
    std::cout << "[PASS] Standard quotes and backslashes escaped.\n";
}

void test_escape_json_string_control_chars() {
    std::string s = "Line1\nLine2\tTabbed\rReturn\bBackspace\fFormfeed";
    std::string escaped = escape_json_string(s);
    REQUIRE(escaped == "Line1\\nLine2\\tTabbed\\rReturn\\bBackspace\\fFormfeed");
    std::cout << "[PASS] Control characters (\\n, \\t, \\r, \\b, \\f) escaped.\n";
}

void test_escape_json_string_hex_control_chars() {
    // Byte 0x01 (SOH) and 0x1F (US) should escape to \u0001 and \u001f
    std::string s;
    s.push_back('\x01');
    s.push_back('A');
    s.push_back('\x1F');
    std::string escaped = escape_json_string(s);
    REQUIRE(escaped == "\\u0001A\\u001f");
    std::cout << "[PASS] Low hex control codes correctly escaped to \\u00XX.\n";
}

void test_escape_json_string_hostile_path() {
    // Simulate a hostile directory traversal or file injection entry
    std::string hostile = "..\\..\\evil\"dir\n\t/file\x07.txt";
    std::string escaped = escape_json_string(hostile);
    REQUIRE(escaped.find("\\\"") != std::string::npos);
    REQUIRE(escaped.find("\\n") != std::string::npos);
    REQUIRE(escaped.find("\\t") != std::string::npos);
    REQUIRE(escaped.find("\\u0007") != std::string::npos);
    std::cout << "[PASS] Hostile injection vectors fully sanitized.\n";
}

void test_escape_json_string_utf8_validation() {
    std::string invalid = std::string("bad\xff\xfe") + "path\xc0\xaf.rar";
    std::string escaped = escape_json_string(invalid);
    REQUIRE(escaped.find("\\uFFFD") != std::string::npos);

    std::string valid = "\xe4\xb8\xad\xe6\x96\x87.txt";
    std::string escaped_valid = escape_json_string(valid);
    REQUIRE(escaped_valid == valid);
    std::cout << "[PASS] UTF-8 validation and \\uFFFD replacement verified.\n";
}

static const openrar::test::Case k_cases[] = {
    {"test_escape_json_string_standard", test_escape_json_string_standard, false},
    {"test_escape_json_string_control_chars", test_escape_json_string_control_chars, false},
    {"test_escape_json_string_hex_control_chars", test_escape_json_string_hex_control_chars, false},
    {"test_escape_json_string_hostile_path", test_escape_json_string_hostile_path, false},
    {"test_escape_json_string_utf8_validation", test_escape_json_string_utf8_validation, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All RFC 8259 JSON Output Unit Tests PASSED!\n";
    return rc;
}
