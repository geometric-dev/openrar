# tools/gate.ps1 - pre-flight lints + build + ctest + interop gate in a single invocation.
#
# Purpose: reduce per-check permission prompts by folding the core validation
# commands after every fix into one script. Exits non-zero on the first failure
# so callers can rely on the exit code.
param(
    [switch]$SkipInterop,
    [switch]$SkipLint
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# 0. Pre-flight Lints ---------------------------------------------------------
if (-not $SkipLint) {
    Write-Host '=== pre-flight lints ===' -ForegroundColor Cyan
    python "$root\tools\bump_version.py" --check
    if ($LASTEXITCODE -ne 0) { Write-Host 'VERSION CHECK FAILED' -ForegroundColor Red; exit 1 }
    python "$root\tools\anchor_lint.py"
    if ($LASTEXITCODE -ne 0) { Write-Host 'ANCHOR LINT FAILED' -ForegroundColor Red; exit 1 }
    python "$root\tools\layer_check.py"
    if ($LASTEXITCODE -ne 0) { Write-Host 'LAYER CHECK FAILED' -ForegroundColor Red; exit 1 }
    Write-Host 'PRE-FLIGHT LINTS OK' -ForegroundColor Green
}

# 1. Build ---------------------------------------------------------------------
Write-Host '=== build (Release) ===' -ForegroundColor Cyan
cmake --build "$root\build" --config Release --parallel |
    Select-String -Pattern 'error C|error :|LNK\d+|FAILED' -SimpleMatch:$false |
    ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAILED' -ForegroundColor Red; exit 1 }
Write-Host 'BUILD OK' -ForegroundColor Green

# 2. ctest ---------------------------------------------------------------------
Write-Host '=== ctest ===' -ForegroundColor Cyan
$ctest = & ctest --test-dir "$root\build" -C Release --output-on-failure 2>&1
$ctest | Select-Object -Last 2 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host 'CTEST FAILED' -ForegroundColor Red
    $ctest | ForEach-Object { Write-Host $_ }
    exit 1
}

# 3. Interop gate --------------------------------------------------------------
if (-not $SkipInterop) {
    Write-Host '=== interop gate (--quick) ===' -ForegroundColor Cyan
    python "$root\tools\interop_gate.py" --quick
    if ($LASTEXITCODE -ne 0) { Write-Host 'INTEROP GATE FAILED' -ForegroundColor Red; exit 1 }
}

Write-Host ''
Write-Host 'GATE PASSED' -ForegroundColor Green
exit 0
