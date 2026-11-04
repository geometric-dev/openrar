param(
    [string]$Root = "",
    [string]$BuildDir = "",
    [int]$ThrottleLimit = 4,
    [switch]$SkipMsvc
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $curr = $PSScriptRoot
    while ($curr -and -not (Test-Path (Join-Path $curr ".git"))) {
        $parent = Split-Path -Parent $curr
        if ($parent -eq $curr) { break }
        $curr = $parent
    }
    $Root = if ($curr -and (Test-Path (Join-Path $curr ".git"))) { $curr } else { (Get-Location).Path }
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $Root "build\static-analysis"
}

Write-Host "========================================="
Write-Host "OpenRAR Static Analysis Pipeline"
Write-Host "Root:     $Root"
Write-Host "BuildDir: $BuildDir"
Write-Host "========================================="

# Step 1: Generate compilation database
Write-Host "`n[1/5] Generating compilation database..."
& (Join-Path $PSScriptRoot "generate_db.ps1") -Root $Root -BuildDir $BuildDir

# Step 2: Main Clang-Tidy analysis (leaks, bounds, overflow)
Write-Host "`n[2/5] Running main Clang-Tidy analysis..."
& (Join-Path $PSScriptRoot "run_tidy.ps1") -Root $Root -BuildDir $BuildDir -ThrottleLimit $ThrottleLimit

# Step 3: Recursion check (misc-no-recursion)
Write-Host "`n[3/5] Running recursion check (misc-no-recursion)..."
& (Join-Path $PSScriptRoot "run_recursion.ps1") -Root $Root -BuildDir $BuildDir -ThrottleLimit $ThrottleLimit

# Step 4: Extra files (worker_main.cpp, archive_api.cpp)
Write-Host "`n[4/5] Running analysis on extra targets..."
& (Join-Path $PSScriptRoot "run_extra.ps1") -Root $Root -BuildDir $BuildDir

# Step 5: MSVC /analyze pass
if (-not $SkipMsvc) {
    Write-Host "`n[5/5] Running MSVC /analyze pass..."
    & (Join-Path $PSScriptRoot "run_msvc_analyze.ps1") -Root $Root -BuildDir $BuildDir -ThrottleLimit $ThrottleLimit
} else {
    Write-Host "`n[5/5] Skipping MSVC /analyze pass (-SkipMsvc specified)."
}

# Step 6: Export & deduplicate findings
Write-Host "`nExporting and deduplicating CSV findings..."
& (Join-Path $PSScriptRoot "export_findings.ps1") -Root $Root -BuildDir $BuildDir

Write-Host "`nStatic analysis pipeline complete!"
Write-Host "Tidy CSV: $(Join-Path $BuildDir 'tidy_findings.csv')"
if (-not $SkipMsvc) {
    Write-Host "MSVC CSV: $(Join-Path $BuildDir 'msvc_findings.csv')"
}
