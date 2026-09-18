# tools/check_golden.ps1 - Validate Golden Archive Integrity and Decodability
#
# Verifies that every golden fixture matches its companion .sha256 checksum
# and extracts/tests cleanly with openrar.exe.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/check_golden.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$openrar = "$root\build\openrar64\Release\openrar.exe"
if (-not (Test-Path $openrar)) {
    $openrar = "$root\build\openrar64\openrar.exe"
}
if (-not (Test-Path $openrar)) {
    Write-Error "openrar.exe not found. Please build target openrar first."
    exit 1
}

$fixturesDir = "$root\tests\fixtures"
$goldenFiles = Get-ChildItem -Path "$fixturesDir\golden" -Filter "*.rar" -Recurse

if ($goldenFiles.Count -eq 0) {
    Write-Error "No golden fixtures found under $fixturesDir\golden"
    exit 1
}

$failed = 0
$passed = 0

Write-Host "=== Checking Golden Fixtures SHA256 & Decodability ===" -ForegroundColor Cyan

foreach ($file in $goldenFiles) {
    $relPath = $file.FullName.Substring($root.Length + 1)
    $shaPath = "$($file.FullName).sha256"
    
    if (-not (Test-Path $shaPath)) {
        Write-Host "[FAIL] Missing .sha256 companion: $relPath" -ForegroundColor Red
        $failed++
        continue
    }

    $expectedContent = (Get-Content -Path $shaPath -Raw).Trim()
    $expectedHash = ($expectedContent -split '\s+')[0].ToLower()
    $actualHash = (Get-FileHash -Algorithm SHA256 -Path $file.FullName).Hash.ToLower()

    if ($expectedHash -ne $actualHash) {
        Write-Host "[FAIL] SHA256 mismatch for $relPath" -ForegroundColor Red
        Write-Host "  Expected: $expectedHash" -ForegroundColor Red
        Write-Host "  Actual:   $actualHash" -ForegroundColor Red
        $failed++
        continue
    }

    # Test integrity with openrar t
    $testOut = & $openrar t -plain -y $file.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] openrar t failed for $relPath" -ForegroundColor Red
        $testOut | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
        $failed++
        continue
    }

    Write-Host "[PASS] $relPath (sha256: $actualHash)" -ForegroundColor Green
    $passed++
}

Write-Host "`nSummary: $passed passed, $failed failed."
if ($failed -gt 0) {
    exit 1
}

Write-Host "ALL GOLDEN FIXTURES VERIFIED" -ForegroundColor Green
exit 0
