#!/usr/bin/env bash
# tools/clang_tidy_check.sh — naming gate (CONTRIBUTING.md "Code style").
#
# Runs clang-tidy with the repository's .clang-tidy (readability-identifier-
# naming only) over every first-party translation unit the build actually
# compiled on this platform.
#
# Scope note: the file list comes from build/compile_commands.json, so a
# source that is not built here (e.g. the Windows-only WinGUI SFX stub on a
# Linux runner) is skipped rather than mis-parsed. That is deliberate - a
# Windows-only TU cannot be analysed on Linux and would only produce false
# positives (WINAPI read as an identifier).
#
# Usage:
#   tools/clang_tidy_check.sh [build-dir]     (default: build)
#   CLANG_TIDY=/path/to/clang-tidy tools/clang_tidy_check.sh build
#
# Requires the build dir to have been configured with
# -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.
set -uo pipefail

BUILD="${1:-build}"
CT="${CLANG_TIDY:-clang-tidy}"
DB="$BUILD/compile_commands.json"

if ! command -v "$CT" >/dev/null 2>&1 && [ ! -x "$CT" ]; then
    echo "clang_tidy_check: '$CT' not found (set CLANG_TIDY=)" >&2
    exit 2
fi
if [ ! -f "$DB" ]; then
    echo "clang_tidy_check: $DB not found - configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 2
fi

# First-party TUs the build compiled here. Exclude the wasm emscripten TUs if
# present (they are not part of the native gate).
mapfile -t FILES < <(python3 - "$DB" <<'PY'
import json, sys
for e in json.load(open(sys.argv[1])):
    f = e["file"].replace("\\", "/")
    if "/src/" in f and f.endswith((".cpp", ".cc", ".cxx")):
        print(f)
PY
)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "clang_tidy_check: no source files found in $DB" >&2
    exit 2
fi

echo "clang_tidy_check: analysing ${#FILES[@]} translation units with $CT"
OUT="$(mktemp)"
printf '%s\0' "${FILES[@]}" | xargs -0 -P "$(nproc 2>/dev/null || echo 4)" -n 1 \
    "$CT" -p "$BUILD" -header-filter='(/src/|/include/)' --quiet >"$OUT" 2>&1

# Anything the naming check flagged, plus real parse errors.
HITS=$(grep -c 'readability-identifier-naming' "$OUT" || true)
ERRS=$(grep -c 'error:' "$OUT" || true)

if [ "$ERRS" -gt 0 ]; then
    echo "clang_tidy_check: clang-tidy reported parse errors:" >&2
    grep 'error:' "$OUT" | head -40 >&2
    rm -f "$OUT"
    exit 1
fi

if [ "$HITS" -gt 0 ]; then
    echo "clang_tidy_check: $HITS naming violation(s):" >&2
    grep 'readability-identifier-naming' "$OUT" | sed 's#^[^:]*/##' | sort -u | head -80 >&2
    rm -f "$OUT"
    exit 1
fi

echo "clang_tidy_check: OK (naming clean)"
rm -f "$OUT"