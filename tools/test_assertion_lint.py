#!/usr/bin/env python3
"""
OpenRAR Test Assertion Integrity Linter (tools/test_assertion_lint.py)
Prevents vacuous, tautological, or fake tests ("BS tests") from entering the codebase.

Enforces:
1. TAUTOLOGY_LITERAL: Rejects literal true/1/!false assertions (e.g. REQUIRE(true)).
2. TAUTOLOGY_OR_TRUE: Rejects '|| true' disjunctive tautologies in assertions or asserted booleans.
3. TAUTOLOGY_UNSIGNED_GE_ZERO: Rejects comparisons of unsigned expressions >= 0 (e.g. v.size() >= 0, inc >= 0).
4. TAUTOLOGY_SELF_COMPARE: Rejects self-comparisons (e.g. REQUIRE(x == x), memcmp(p, p, n)).
4b. TAUTOLOGY_ADDRESS_NOT_NULL: Rejects REQUIRE(&obj != nullptr); an object's address is never null.
5. ZERO_ASSERTIONS: Rejects test functions or test cases containing zero assertions or test suite calls.
6. FAIL_OPEN_ENV_BYPASS: Rejects env-var assertion guards that lack fail-closed termination (std::exit/FAIL).

Exit code:
  0: All test cases pass assertion integrity checks.
  1: One or more violations detected, or self-test failed.
"""

import sys
import os
import glob
import re
import argparse

# ─── Rule Definitions & Regular Expressions ─────────────────────────────────

# 1. Literal tautologies: REQUIRE(true), CHECK(true), assert(true), REQUIRE(1), etc.
RE_TAUTOLOGY_LITERAL = re.compile(
    r'\b(REQUIRE|CHECK|assert|WARN)\s*\(\s*(true|1|!false|!0|false\s*==\s*false|true\s*==\s*true)\s*\)|'
    r'\b(REQUIRE_FALSE|CHECK_FALSE)\s*\(\s*(false|0|!true)\s*\)|'
    r'\b(REQUIRE_EQ|CHECK_EQ)\s*\(\s*(true,\s*true|false,\s*false|0,\s*0|1,\s*1)\s*\)',
    re.IGNORECASE
)

# 2. '|| true' or 'true ||' tautology in assertions or variable assignment:
# e.g. bool ok = (!active || true); REQUIRE(ok); or REQUIRE(foo || true);
RE_TAUTOLOGY_OR_TRUE = re.compile(
    r'(?:\|\|\s*true\b|\btrue\s*\|\|)',
    re.IGNORECASE
)

# 3. Always-true unsigned >= 0 comparisons:
# e.g. REQUIRE(v.size() >= 0); REQUIRE(inc >= 0);
RE_UNSIGNED_GE_ZERO = re.compile(
    r'\b(?:REQUIRE|CHECK|assert)\s*\(\s*.*(?:\.(?:size|length|count)\(\)|sizeof\s*\([^)]+\)|'
    r'\b(?:inc|count|len|size|offset|pos|idx|index|c|bytes)\b)\s*>=\s*0\s*\)',
    re.IGNORECASE
)

# 4. Identity / Self-comparisons: strictly CASE-SENSITIVE
# e.g. REQUIRE(x == x); CHECK(res >= res); memcmp(buf, buf, len) == 0;
RE_SELF_COMPARE = re.compile(
    r'\b(?:REQUIRE|CHECK|assert)\s*\(\s*([a-zA-Z_][a-zA-Z0-9_.]*)\s*(?:==|>=|<=)\s*\1\s*\)|'
    r'\b(?:REQUIRE_EQ|CHECK_EQ)\s*\(\s*([a-zA-Z_][a-zA-Z0-9_.]*)\s*,\s*\2\s*\)|'
    r'\bmemcmp\s*\(\s*([a-zA-Z_][a-zA-Z0-9_.]*)\s*,\s*\3\s*,'
)

# 5. Address-of-object non-null checks: the address of a named object is never null, so
# REQUIRE(&cpu != nullptr) can only pass. (Pointers returned by functions are fine.)
RE_ADDRESS_NOT_NULL = re.compile(
    r'\b(?:REQUIRE|CHECK|assert)\s*\(\s*&\s*[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*'
    r'\s*!=\s*(?:nullptr|NULL|0)\s*\)'
)

# Test function declarations
RE_FN_START = re.compile(r'^\s*(?:static\s+)?void\s+(test_[A-Za-z0-9_]+)\s*\(\s*\)')
RE_CASE_START = re.compile(r'^\s*(?:TEST_CASE|DOCTEST_TEST_CASE)\s*\(\s*"([^"]+)"\s*\)')

# Assertion markers
RE_ASSERTION = re.compile(
    r'\b(REQUIRE|CHECK|assert|FAIL|REQUIRE_FALSE|CHECK_FALSE|REQUIRE_THROWS|CHECK_THROWS|'
    r'REQUIRE_EQ|CHECK_EQ|suite_[a-zA-Z0-9_]+|assert_[a-zA-Z0-9_]+|check_[a-zA-Z0-9_]+|verify_[a-zA-Z0-9_]+)\b'
)


def lint_content(content: str, filename: str = "<memory>") -> list:
    """Analyze C++ source content and return a list of (line_no, rule_id, message) tuples."""
    violations = []
    lines = content.splitlines()

    current_test = None
    test_start_line = 0
    test_body = []
    brace_depth = 0
    in_test = False

    for idx, raw_line in enumerate(lines, 1):
        # Strip line comments for single-line checks
        line = raw_line.split('//')[0]

        # 1. Literal tautologies
        m = RE_TAUTOLOGY_LITERAL.search(line)
        if m:
            violations.append((idx, "TAUTOLOGY_LITERAL", f"Literal tautological assertion: '{m.group(0).strip()}'"))

        # 2. '|| true' disjunctive tautologies
        m = RE_TAUTOLOGY_OR_TRUE.search(line)
        if m:
            violations.append((idx, "TAUTOLOGY_OR_TRUE", f"Tautological '|| true' condition: '{m.group(0).strip()}'"))

        # 3. Unsigned >= 0
        m = RE_UNSIGNED_GE_ZERO.search(line)
        if m:
            violations.append((idx, "TAUTOLOGY_UNSIGNED_GE_ZERO", f"Always-true unsigned comparison >= 0: '{m.group(0).strip()}'"))

        # 4. Self-comparison
        m = RE_SELF_COMPARE.search(line)
        if m:
            violations.append((idx, "TAUTOLOGY_SELF_COMPARE", f"Self-comparison tautology: '{m.group(0).strip()}'"))

        # 5. Address of a named object compared with null
        m = RE_ADDRESS_NOT_NULL.search(line)
        if m:
            violations.append((idx, "TAUTOLOGY_ADDRESS_NOT_NULL",
                               f"Address of an object is never null: '{m.group(0).strip()}'"))

        # Test block boundary tracking
        if not in_test:
            m_fn = RE_FN_START.search(line)
            m_case = RE_CASE_START.search(line)
            if m_fn:
                current_test = m_fn.group(1)
                in_test = True
                test_start_line = idx
                test_body = [line]
                brace_depth = line.count('{') - line.count('}')
            elif m_case:
                current_test = m_case.group(1)
                in_test = True
                test_start_line = idx
                test_body = [line]
                brace_depth = line.count('{') - line.count('}')
        else:
            test_body.append(line)
            brace_depth += line.count('{') - line.count('}')
            if brace_depth <= 0:
                full_body = "\n".join(test_body)
                
                # Check 5: Zero assertions
                if not RE_ASSERTION.search(full_body):
                    violations.append((test_start_line, "ZERO_ASSERTIONS", f"Test '{current_test}' contains 0 assertions or suite calls"))

                # Check 6: Fail-open env bypass
                # If test references OPENRAR_REFCAP or getenv, verify it does not skip assertions without fail-closed termination
                if "OPENRAR_REFCAP" in full_body or "getenv(" in full_body:
                    if "else" in full_body and "REQUIRE(" in full_body:
                        # If assertions are in an else branch, check if the if branch calls exit/abort/FAIL/throw
                        if not re.search(r'\b(exit|abort|FAIL|throw)\b', full_body):
                            violations.append((test_start_line, "FAIL_OPEN_ENV_BYPASS",
                                               f"Test '{current_test}' conditionally guards assertions with env check without fail-closed termination"))

                in_test = False
                current_test = None

    return violations


def lint_file(filepath: str) -> list:
    """Lint a single file and return violations."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        return lint_content(content, filepath)
    except Exception as e:
        return [(0, "FILE_READ_ERROR", f"Could not read {filepath}: {e}")]


def run_selftest() -> bool:
    """Validate that the linter catches all audit-rejected bad patterns and passes valid tests."""
    print("Running test_assertion_lint self-test suite...")

    bad_snippets = [
        # 1. NT-12 disjunctive tautology
        ("""
void test_nt12_bad() {
    bool ok = (!active || true);
    REQUIRE(ok);
}
""", "TAUTOLOGY_OR_TRUE"),

        # 2. Literal true
        ("""
void test_literal_true() {
    REQUIRE(true);
}
""", "TAUTOLOGY_LITERAL"),

        # 3. Unsigned size >= 0 (BT-03)
        ("""
void test_bt03_bad() {
    std::vector<int> matches;
    REQUIRE(matches.size() >= 0);
}
""", "TAUTOLOGY_UNSIGNED_GE_ZERO"),

        # 4. Unsigned variable inc >= 0 (NT-15)
        ("""
void test_nt15_bad() {
    size_t inc = 4;
    REQUIRE(inc >= 0);
}
""", "TAUTOLOGY_UNSIGNED_GE_ZERO"),

        # 5. Self-comparison
        ("""
void test_self_comp() {
    int cand = 5;
    REQUIRE(cand == cand);
}
""", "TAUTOLOGY_SELF_COMPARE"),

        # 5b. Address-of-object null check (test_cpu_features)
        ("""
void test_cpu_bad() {
    const auto& cpu = core::get_cpu_features();
    REQUIRE(&cpu != nullptr);
}
""", "TAUTOLOGY_ADDRESS_NOT_NULL"),

        # 6. Zero assertions
        ("""
void test_empty_suite() {
    std::cout << "[PASS] All good!\\n";
}
""", "ZERO_ASSERTIONS"),

        # 7. Fail-open REFCAP bypass
        ("""
void test_fail_open_refcap() {
    if (std::getenv("OPENRAR_REFCAP")) {
        std::cout << "Suppressing hash check\\n";
    } else {
        REQUIRE(hash == "pinned");
    }
}
""", "FAIL_OPEN_ENV_BYPASS"),
    ]

    all_passed = True
    for idx, (snippet, expected_rule) in enumerate(bad_snippets, 1):
        v = lint_content(snippet, f"<bad_snippet_{idx}>")
        detected_rules = [rule for _, rule, _ in v]
        if expected_rule not in detected_rules:
            print(f"  [FAIL] Selftest #{idx}: expected rule {expected_rule}, got {detected_rules}")
            all_passed = False
        else:
            print(f"  [PASS] Selftest #{idx}: correctly caught {expected_rule}")

    # Good snippet: should have 0 violations
    good_snippet = """
void test_legitimate_feature() {
    openrar::Compressor50 comp;
    auto data = make_patterned_data(1024);
    std::vector<core::byte> out;
    comp.compress_buffer(data.data(), data.size(), out);
    REQUIRE(!out.empty());
    REQUIRE(out.size() < data.size());
}
"""
    v_good = lint_content(good_snippet, "<good_snippet>")
    if v_good:
        print(f"  [FAIL] Selftest clean snippet had false positives: {v_good}")
        all_passed = False
    else:
        print("  [PASS] Selftest clean snippet: 0 false positives")

    return all_passed


def main():
    parser = argparse.ArgumentParser(description="OpenRAR Test Assertion Integrity Linter")
    parser.add_argument("paths", nargs="*", help="Files or glob patterns to lint (default: tests/unit/*.cpp)")
    parser.add_argument("--selftest", action="store_true", help="Run internal self-test suite")
    parser.add_argument("--verbose", action="store_true", help="Show all scanned tests")
    args = parser.parse_args()

    if args.selftest:
        success = run_selftest()
        sys.exit(0 if success else 1)

    # Determine files to scan
    target_files = []
    if args.paths:
        for p in args.paths:
            if os.path.isfile(p):
                target_files.append(p)
            else:
                target_files.extend(glob.glob(p))
    else:
        target_files = sorted(glob.glob("tests/unit/*.cpp"))

    if not target_files:
        print("[test_assertion_lint] No test files found to scan.")
        sys.exit(0)

    total_violations = 0
    scanned_count = len(target_files)

    for fpath in target_files:
        violations = lint_file(fpath)
        if violations:
            for lno, rule, msg in violations:
                print(f"[VIOLATION] {fpath}:{lno}: [{rule}] {msg}")
                total_violations += 1
        elif args.verbose:
            print(f"[OK] {fpath}")

    if total_violations > 0:
        print(f"\n[ERROR] test_assertion_lint: {total_violations} violation(s) detected across {scanned_count} files!")
        print("        Vacuous, tautological, or fail-open tests are strictly blocked.")
        sys.exit(1)
    else:
        print(f"[PASS] test_assertion_lint: {scanned_count} test file(s) checked, 0 violations found.")
        sys.exit(0)


if __name__ == "__main__":
    main()
