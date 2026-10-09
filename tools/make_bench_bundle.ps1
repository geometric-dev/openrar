# v1.40 M4: build the self-contained benchmark bundle dist\openrar-bench.exe
# (SFX console stub + runbench.exe driver + engines; nothing proprietary is
# committed — WinRAR's rar.exe/UnRAR.exe are sourced from the local install at
# PACKAGE TIME ONLY and live only inside the gitignored dist/ tree).
#
# Layout inside the SFX (extracted flat into %TEMP%\OpenRAR-<hex>):
#   runbench.exe   this driver (Setup= target, CWD = extraction dir)
#   openrar.exe    built from this repo
#   UnRAR.exe      copied from the local WinRAR install (D4)
#   rar.exe        copied from the local WinRAR install (D4)
#
# Comment directives (parsed by src/sfx/sfx_config.cpp; a line without '=' is
# ignored, hence "TempMode=" and not "TempMode"):
#   Setup=runbench.exe   -> consent prompt (default: Don't Run; press A) ->
#                           contained spawn, stub waits for exit
#   TempMode=            -> extract to %TEMP%\OpenRAR-<hex>, Setup runs with
#                           that as CWD, subtree deleted afterwards
# The driver itself removes the work dir it creates beside its exe; the report
# stays at %USERPROFILE%\openrar-bench-results.json (D3).
#
# Smoke (no directives): dist\openrar-bench.exe -sfxnoexec -d<empty dir>
# Release check (manual, once per release): double-click the bundle, press A
# at the consent prompt, let the run finish, press Enter; confirm the report
# exists and no OpenRAR-* subtree remains in %TEMP%.

param(
    [string]$Config = "Release",
    [string]$Out = "dist\openrar-bench.exe",
    [string]$WinrarDir = "$env:ProgramFiles\WinRAR",
    [string]$RunbenchExe = "",
    [string]$OpenrarExe = "",
    [string]$Stub = "",
    [switch]$SkipSmoke
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Resolve-File([string]$label, [string]$given, [string[]]$candidates) {
    if ($given -ne "" ) {
        if (-not (Test-Path -LiteralPath $given)) { throw "$label not found: $given" }
        return (Resolve-Path -LiteralPath $given).Path
    }
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path } }
    throw "$label not found; tried: $($candidates -join ', ')"
}

$runbench = Resolve-File "runbench.exe" $RunbenchExe @(
    (Join-Path $Root "build\Release\runbench.exe"),
    (Join-Path $Root "build\runbench.exe"),
    (Join-Path $Root "build\Debug\runbench.exe")
)
$openrar = Resolve-File "openrar.exe" $OpenrarExe @(
    (Join-Path $Root "build\openrar64\$Config\openrar.exe"),
    (Join-Path $Root "build\Release\openrar.exe")
)
$stub = Resolve-File "Default.SFX" $Stub @(
    (Join-Path $Root "build\openrar64\$Config\Default.SFX.exe"),
    (Join-Path $Root "build\openrar64\Release\Default.SFX.exe")
)
# D4: stock engines from the local WinRAR install; package time only (D4) —
# never committed, only copied into the gitignored dist/ payload.
$unrar = Resolve-File "UnRAR.exe" "" @(
    (Join-Path $WinrarDir "UnRAR.exe"),
    "${env:ProgramFiles(x86)}\WinRAR\UnRAR.exe"
)
$rar = Resolve-File "rar.exe" "" @(
    (Join-Path $WinrarDir "rar.exe"),
    "${env:ProgramFiles(x86)}\WinRAR\rar.exe"
)

$outPath = Join-Path $Root $Out
$outDir = Split-Path -Parent $outPath
$stage = Join-Path $outDir "stage"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $runbench (Join-Path $stage "runbench.exe")
Copy-Item $openrar  (Join-Path $stage "openrar.exe")
Copy-Item $unrar    (Join-Path $stage "UnRAR.exe")
Copy-Item $rar      (Join-Path $stage "rar.exe")
# Bare -sfx resolves "default.sfx" against cwd then the exe dir
# (ArchiveMutator::resolve_sfx_stub); stage it under that name, NOT packed.
Copy-Item $stub     (Join-Path $stage "default.sfx")

$comment = Join-Path $outDir "sfx-comment.txt"
@(
    "Title=OpenRAR Benchmark Bundle"
    "Text=Runs the OpenRAR vs WinRAR vs UnRAR benchmark and writes a JSON report to %USERPROFILE%\openrar-bench-results.json."
    "Setup=runbench.exe"
    "TempMode="
) | Set-Content -LiteralPath $comment -Encoding ascii

if (Test-Path -LiteralPath $outPath) { Remove-Item -Force -LiteralPath $outPath }

Push-Location $stage
try {
    $commentAbs = (Resolve-Path -LiteralPath $comment).Path
    & $openrar a -sfx "-z$commentAbs" $outPath runbench.exe openrar.exe UnRAR.exe rar.exe
    if ($LASTEXITCODE -ne 0) { throw "openrar a -sfx failed rc=$LASTEXITCODE" }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $outPath)) {
    # apply_sfx_extension may have appended .exe to a non-.exe target
    $alt = [System.IO.Path]::ChangeExtension($outPath, ".exe")
    if (-not (Test-Path -LiteralPath $alt)) { throw "bundle not produced: $outPath" }
    $outPath = $alt
}

$payload = Get-ChildItem $stage | Where-Object { $_.Name -ne "default.sfx" } |
    ForEach-Object { "{0}  {1} bytes" -f $_.Name, $_.Length }
Write-Host "bundle: $outPath"
$payload | ForEach-Object { Write-Host "  $_" }

if (-not $SkipSmoke) {
    # Directives suppressed (-sfxnoexec): extraction-only smoke, no consent
    # prompt, no Setup. Two stub behaviors shape this check:
    #   - TempMode= overrides any -d destination (sfx_pipeline.cpp:207), so the
    #     payload lands in a NEW %TEMP%\OpenRAR-<hex> dir, not in -d;
    #   - the -sfxnoexec branch returns before the TempMode cleanup
    #     (sfx_pipeline.cpp:232), so THIS smoke must remove that dir itself —
    #     exactly what the interactive run's cleanup does after Setup exits.
    $before = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'OpenRAR-*' -ErrorAction SilentlyContinue |
        ForEach-Object Name)
    & $outPath -sfxnoexec
    if ($LASTEXITCODE -ne 0) { throw "smoke extraction failed rc=$LASTEXITCODE" }
    $new = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'OpenRAR-*' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin $before })
    $found = $false
    foreach ($d in $new) {
        $ok = $true
        foreach ($f in @("runbench.exe", "openrar.exe", "UnRAR.exe", "rar.exe")) {
            if (-not (Test-Path -LiteralPath (Join-Path $d.FullName $f))) { $ok = $false }
        }
        if ($ok) { $found = $true }
        Remove-Item -Recurse -Force -LiteralPath $d.FullName -ErrorAction SilentlyContinue
    }
    if (-not $found) { throw "smoke: payload files not found in a new OpenRAR-* temp dir" }
    Write-Host "smoke: -sfxnoexec extraction ok (directives suppressed, temp dir cleaned)"
    Write-Host "release check (manual): double-click the bundle, press A at the"
    Write-Host "  consent prompt, finish the run, then verify no OpenRAR-* dir"
    Write-Host "  remains in %TEMP% and the report landed in %USERPROFILE%."
}
