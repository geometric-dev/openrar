#!/bin/sh
# Local CI-matrix preflight — run the legs you CAN run before pushing, and
# list exactly which CI legs remain uncovered ("commit-and-pray" scope).
#
# Usage:
#   sh tools/preflight.sh              # all legs that are available locally
#   sh tools/preflight.sh --quick      # skip the slow Windows ctest leg
#
# Legs (auto-detected):
#   [win]  MSVC Release build + full CTest           (native, slow)
#   [fmt]  clang-format full tree (CI format leg)     (needs clang-format 18)
#   [wsl]  Ubuntu gcc -Werror build + full CTest      (needs WSL + gcc + cmake;
#          mirrors the CI ubuntu-gcc gate leg, built on ext4 for speed)
#   [sde]  GFNI/AVX-512 kernel gates under Intel SDE  (needs dev/sde-external-*)
#   [ci]   gh run status for the current branch       (needs gh)
#
# Exit: 0 iff every EXECUTED leg passed. Skipped legs are listed so the
# residual push-and-pray scope stays visible.

set -u
cd "$(dirname "$0")/.." || exit 1

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

PASS=""
FAIL=""
SKIP=""

note() { echo "$1"; }
leg_pass() { PASS="$PASS $2"; note "  [PASS] $1"; }
leg_fail() { FAIL="$FAIL $2"; note "  [FAIL] $1"; }
leg_skip() { SKIP="$SKIP $2"; note "  [SKIP] $1"; }

echo "=== OpenRAR local preflight ==="

# ── [fmt] clang-format (CI format leg, full tree) ────────────────────────────
CF=""
for candidate in clang-format-18 clang-format; do
    command -v "$candidate" >/dev/null 2>&1 && CF="$candidate" && break
done
if [ -z "$CF" ] && command -v python >/dev/null 2>&1; then
    CF=$(python -c "
import os, sys, sysconfig
out = sys.stdout.buffer
cands = []
for scheme in ('nt_user', 'posix_user', 'default', None):
    try:
        p = sysconfig.get_path('scripts', scheme) if scheme else sysconfig.get_path('scripts')
    except Exception:
        p = None
    if p:
        cands.append(p)
for p in cands:
    for name in ('clang-format.exe', 'clang-format'):
        full = os.path.join(p, name)
        if os.path.isfile(full):
            out.write(full.encode() + b'\n')
            sys.exit(0)
out.write(b'\n')
" 2>/dev/null)
    CF=${CF%$'\r'}
fi
if [ -n "$CF" ]; then
    if find src include tests -name '*.cpp' -o -name '*.hpp' -o -name '*.h' 2>/dev/null |
        xargs "$CF" --dry-run --Werror 2>&1 | grep -q "error:"; then
        leg_fail "format gate (clang-format)" fmt
    else
        leg_pass "format gate (clang-format)" fmt
    fi
else
    leg_skip "format gate (clang-format not found)" fmt
fi

# ── [ver] version consistency (CI gate leg) ──────────────────────────────────
CMAKE_VER=$(grep -E 'project\(openrar VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt |
    sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
PKG_VER=$(node -e "console.log(require('./wasm/js/package.json').version)" 2>/dev/null)
if [ "$CMAKE_VER" = "$PKG_VER" ] && [ -n "$PKG_VER" ]; then
    leg_pass "version consistency ($CMAKE_VER)" ver
else
    leg_fail "version consistency (CMake=$CMAKE_VER pkg=$PKG_VER)" ver
fi

# ── [win] MSVC Release build + full CTest ────────────────────────────────────
if command -v cmake >/dev/null 2>&1 && [ "${PREFLIGHT_NO_WIN:-0}" != "1" ]; then
    if [ $QUICK -eq 1 ] && [ -d build ]; then
        note "  [SKIP] win MSVC ctest (--quick)"
        SKIP="$SKIP win"
    else
        cmake --build build --config Release >/dev/null 2>&1
        if ctest --test-dir build -C Release 2>&1 | grep -q "tests passed"; then
            leg_pass "win MSVC build + ctest" win
        else
            leg_fail "win MSVC build + ctest" win
        fi
    fi
else
    leg_skip "win MSVC (cmake missing or PREFLIGHT_NO_WIN=1)" win
fi

# ── [wsl] Ubuntu gcc -Werror build + full CTest (mirrors CI's gate leg) ──────
if command -v wsl >/dev/null 2>&1; then
    WSL_GCC=$(wsl -e bash -c "command -v g++ >/dev/null 2>&1 && echo yes || echo no" 2>/dev/null |
        tr -d '\r\n')
    if [ "$WSL_GCC" = "yes" ]; then
        # Copy the source to ext4 (/mnt/c 9P I/O is 5-15x slower) and build.
        wsl -e bash -c "
            set -e
            mkdir -p ~/openrar-preflight
            cd ~/openrar-preflight
            for d in cmake include src tests; do
                mkdir -p \$d
                cp -ru /mnt/c/Users/Matt/dev/openrar/\$d/. \$d/ 2>/dev/null || true
            done
            cp -u /mnt/c/Users/Matt/dev/openrar/CMakeLists.txt .
            cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENRAR_WARNINGS_AS_ERRORS=ON \
                  -DOPENRAR_INMEM_ARCHIVE=ON >/dev/null 2>&1
            cmake --build build --parallel \$(nproc) >/dev/null
            ctest --test-dir build -C Release 2>&1 | grep -q 'tests passed'
        " >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            leg_pass "wsl gcc -Werror build + ctest" wsl
        else
            leg_fail "wsl gcc -Werror build + ctest" wsl
        fi
    else
        leg_skip "wsl gcc (g++ not installed in WSL)" wsl
    fi
else
    leg_skip "wsl gcc (no WSL)" wsl
fi

# ── [sde] kernel gates under Intel SDE ───────────────────────────────────────
SDE=""
for d in /c/Users/Matt/dev/sde-external-*/sde.exe "$HOME/dev/sde-external-"*/sde.exe; do
    [ -x "$d" ] && SDE="$d" && break
done
if [ -n "$SDE" ] && [ -x build/Release/recovery_tests.exe ]; then
    if "$SDE" -future -- ./build/Release/recovery_tests.exe >/dev/null 2>&1 &&
       "$SDE" -future -- ./build/Release/compress_tests.exe >/dev/null 2>&1; then
        leg_pass "SDE kernel gates (recovery + compress, AVX-512/GFNI)" sde
    else
        leg_fail "SDE kernel gates (recovery + compress, AVX-512/GFNI)" sde
    fi
else
    leg_skip "SDE kernel gates (no SDE kit or test binaries)" sde
fi

# ── [ci] pending/running CI runs for this branch ─────────────────────────────
if command -v gh >/dev/null 2>&1; then
    BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
    PENDING=$(gh run list --branch "$BRANCH" --status pending --limit 1 2>/dev/null | grep -c . || true)
    FAILED_RECENT=$(gh run list --branch "$BRANCH" --limit 3 --json conclusion --jq '[.[] | select(.conclusion == "failure")] | length' 2>/dev/null)
    if [ "${FAILED_RECENT:-0}" != "0" ]; then
        leg_fail "CI history (branch $BRANCH has $FAILED_RECENT failed run(s) in the last 3)" ci
    else
        leg_pass "CI history (branch $BRANCH, no recent failures)" ci
    fi
else
    leg_skip "CI history (gh not found)" ci
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo "=== Preflight summary ==="
for p in $PASS; do echo "  PASS  $p"; done
for f in $FAIL; do echo "  FAIL  $f"; done
for s in $SKIP; do echo "  SKIP  $s  <- still commit-and-pray for this leg"; done

if [ -n "$FAIL" ]; then
    echo "PREFLIGHT FAILED — fix the failing legs before pushing."
    exit 1
fi
echo "PREFLIGHT PASSED — all locally-runnable legs green."
exit 0
