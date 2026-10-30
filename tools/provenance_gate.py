#!/usr/bin/env python3
"""
tools/provenance_gate.py

OpenRAR Automated Clean-Room Provenance Gate.
Runs in CI on pull requests to ensure no reference-implementation files,
internal identifier residue, or contaminated specifications enter the codebase.

Section 6.7 of clean-room audit.
"""

import os
import re
import sys
import argparse

# Deny-listed file names (UnRAR-internal translation units)
BANNED_FILENAMES = {
    "archive_reader.cpp", "arcread.hpp",
    "errhnd.cpp", "errors.hpp",
    "isnt.cpp", "isnt.hpp",
    "pathfn.cpp", "pathfn.hpp",
    "strfn.cpp", "strfn.hpp",
    "filcreat.cpp", "filcreat.hpp",
    "rdwrfn.cpp", "rdwrfn.hpp",
    "recvol.cpp", "recvol.hpp",
    "recvol3.cpp", "recovery_record.cpp",
    "win32lnk.cpp",
    "headers.hpp",
    "unpack50.cpp", "unpack50mt.cpp",
}

# Deny-listed identifiers and symbols
BANNED_IDENTIFIERS = [
    r"\bReedSolomon16\b",
    r"\bgfExp\b",
    r"\bgfLog\b",
    r"\bDataLog\b",
    r"\blut_cache_\b",
    r"\bGfField\b",
    r"\bRAR_EXIT_[A-Z]+\b",
    r"\bMarkOfTheWeb\b",
    r"\bunp_ptr\b",
    r"\bwr_ptr\b",
    r"\bunp_buffer\b",
    r"\bfirst_win_done\b",
    r"\bdecode_pos\b",
    r"\bzero_count\b",
    r"\bdist_slot\b",
    r"\bapply_sfx_extension\b",
    r"\bunpacked_size\b",
    r"\bdata_volume_count\b",
    r"\bshard_slot_index\b",
    r"\bparity_volume_count\b",
    r"\brestore_volumes\b",
    r"\bCHANGE_VOLUME\b",
    r"4\s*\*\s*GF_SIZE",
]

# Source files to scan for banned identifiers
SCAN_EXTENSIONS = (".cpp", ".hpp", ".h", ".c", ".md")
SCAN_DIRS = ["src", "include", "docs/spec"]

def check_filenames(repo_root):
    violations = []
    for root, dirs, files in os.walk(repo_root):
        if ".git" in root or "build" in root or "tests/fixtures" in root:
            continue
        for f in files:
            if f.lower() in {b.lower() for b in BANNED_FILENAMES}:
                rel = os.path.relpath(os.path.join(root, f), repo_root)
                violations.append(f"Banned file name detected: {rel}")
    return violations

def check_identifiers(repo_root):
    compiled_patterns = [(pat, re.compile(pat)) for pat in BANNED_IDENTIFIERS]
    violations = []

    for d in SCAN_DIRS:
        target_dir = os.path.join(repo_root, d)
        if not os.path.exists(target_dir):
            continue
        for root, dirs, files in os.walk(target_dir):
            for f in files:
                if not f.endswith(SCAN_EXTENSIONS):
                    continue
                file_path = os.path.join(root, f)
                try:
                    with open(file_path, "r", encoding="utf-8", errors="replace") as fh:
                        for line_num, line in enumerate(fh, start=1):
                            if "provenance-allow" in line:
                                continue
                            for pat_str, pat in compiled_patterns:
                                if pat.search(line):
                                    rel = os.path.relpath(file_path, repo_root)
                                    violations.append(f"{rel}:{line_num}: match '{pat_str}': {line.strip()[:100]}")
                except Exception as e:
                    violations.append(f"Error reading {file_path}: {e}")
    return violations

def check_shingles_if_configured(repo_root):
    unrar_dir = os.environ.get("OPENRAR_UNRAR_DIR")
    if not unrar_dir or not os.path.exists(unrar_dir):
        return []

    print(f"Running shingle containment check against OPENRAR_UNRAR_DIR={unrar_dir}...")
    def tokenize(text):
        text = re.sub(r'//.*', '', text)
        text = re.sub(r'/\*[\s\S]*?\*/', '', text)
        text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
        return re.findall(r'[A-Za-z_]\w*|[0-9]+|!=|==|<=|>=|&&|\|\||<<|>>|[+\-*/%&|^~<>=!]', text)

    def shingles(tokens, k=14):
        if len(tokens) < k:
            return set()
        return set(tuple(tokens[i:i+k]) for i in range(len(tokens) - k + 1))

    # Exempt public standard implementations
    EXEMPT_PATHS = ["crypto/blake2", "crypto/arch/blake2", "io/win32_meta"]

    violations = []
    openrar_src = os.path.join(repo_root, "src")
    openrar_files = {}
    for root, dirs, files in os.walk(openrar_src):
        for f in files:
            if f.endswith((".cpp", ".hpp", ".h")):
                p = os.path.join(root, f)
                rel = os.path.relpath(p, openrar_src).replace("\\", "/")
                if any(ex in rel for ex in EXEMPT_PATHS):
                    continue
                openrar_files[rel] = tokenize(open(p, "r", encoding="utf-8", errors="replace").read())

    unrar_files = {}
    for f in os.listdir(unrar_dir):
        p = os.path.join(unrar_dir, f)
        if os.path.isfile(p) and f.endswith((".cpp", ".hpp", ".h")):
            unrar_files[f] = tokenize(open(p, "r", encoding="utf-8", errors="replace").read())

    for of, otoks in openrar_files.items():
        osh = shingles(otoks, k=14)
        if not osh:
            continue
        for uf, utoks in unrar_files.items():
            ush = shingles(utoks, k=14)
            if not ush:
                continue
            common = osh.intersection(ush)
            containment = len(common) / len(osh)
            if containment > 0.15:
                violations.append(f"Containment violation: {of} vs {uf} = {containment:.4f} > 0.15 ({len(common)} shared shingles)")

    return violations

def main():
    parser = argparse.ArgumentParser(description="OpenRAR Clean-Room Provenance Gate")
    parser.add_argument("--root", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..")),
                        help="Repository root path")
    args = parser.parse_args()

    repo_root = args.root
    print(f"=== OpenRAR Provenance Gate ===")
    print(f"Scanning repository root: {repo_root}")

    violations = []
    file_violations = check_filenames(repo_root)
    if file_violations:
        violations.extend(file_violations)

    id_violations = check_identifiers(repo_root)
    if id_violations:
        violations.extend(id_violations)

    shingle_violations = check_shingles_if_configured(repo_root)
    if shingle_violations:
        violations.extend(shingle_violations)

    if violations:
        print(f"\n[FAIL] Provenance gate detected {len(violations)} violation(s):")
        for v in violations:
            print(f"  - {v}")
        sys.exit(1)

    print("\n[PASS] Provenance gate clean: Zero banned files, zero banned identifiers.")
    sys.exit(0)

if __name__ == "__main__":
    main()
