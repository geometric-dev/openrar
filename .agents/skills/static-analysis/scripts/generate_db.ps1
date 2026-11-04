param(
    [string]$Root = "",
    [string]$BuildDir = ""
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

# Locate Visual Studio installation
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -property installationPath
    if ($found -and (Test-Path "$found\VC\Auxiliary\Build\vcvars64.bat")) {
        $vsPath = $found
    }
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}
if (-not (Test-Path $ninja)) {
    throw "ninja.exe not found at: $ninja"
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Restore auxiliary resources if missing
$extraDir = Join-Path $BuildDir "extra"
New-Item -ItemType Directory -Force -Path $extraDir | Out-Null

$resourceDir = Join-Path $PSScriptRoot "..\resources"
if (Test-Path (Join-Path $resourceDir "leaks_overflows.ruleset")) {
    $targetRuleset = Join-Path $BuildDir "leaks_overflows.ruleset"
    if (-not (Test-Path $targetRuleset)) {
        Copy-Item (Join-Path $resourceDir "leaks_overflows.ruleset") $targetRuleset
    }
}
if (Test-Path (Join-Path $resourceDir "extra_compile_commands.json")) {
    $targetExtraDb = Join-Path $extraDir "compile_commands.json"
    if (-not (Test-Path $targetExtraDb)) {
        Copy-Item (Join-Path $resourceDir "extra_compile_commands.json") $targetExtraDb
    }
}

Write-Host "Configuring CMake with Ninja in VS Developer Environment..."
$cmd = "`"$vcvars`" >nul && cmake -G Ninja -S `"$Root`" -B `"$BuildDir`" -DCMAKE_BUILD_TYPE=Debug -DOPENRAR_BUILD_TESTS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `"-DCMAKE_MAKE_PROGRAM=$ninja`""

cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE"
}

$targetDb = Join-Path $BuildDir "compile_commands.json"
if (-not (Test-Path $targetDb)) {
    throw "compile_commands.json was not generated at $targetDb"
}

$db = Get-Content $targetDb -Raw | ConvertFrom-Json
Write-Host "Success: Generated $targetDb with $($db.Count) compilation units."
