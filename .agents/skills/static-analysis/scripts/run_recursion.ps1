param(
    [string]$Root = "",
    [string]$BuildDir = "",
    [int]$ThrottleLimit = 4,
    [string]$File = ""
)

$ErrorActionPreference = 'Continue'

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

# Locate clang-tidy
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -property installationPath
    if ($found -and (Test-Path "$found\VC\Tools\Llvm\x64\bin\clang-tidy.exe")) {
        $vsPath = $found
    }
}
$ct = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
if (-not (Test-Path $ct)) {
    throw "clang-tidy.exe not found at: $ct"
}

$out = Join-Path $BuildDir 'tidy'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$dbPath = Join-Path $BuildDir 'compile_commands.json'
if (-not (Test-Path $dbPath)) {
    throw "compile_commands.json not found at: $dbPath. Run generate_db.ps1 first."
}

$db = Get-Content $dbPath -Raw | ConvertFrom-Json
$files = if ($File) {
    @($File)
} else {
    $db | ForEach-Object { $_.file } | Sort-Object -Unique
}

Write-Host "Running misc-no-recursion on $($files.Count) file(s)..."

$files | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $f = $_
    $name = ($f -replace '^.*?/src/', '') -replace '[\\/]', '_'
    $log = Join-Path $using:out ($name + '.recursion.txt')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $using:ct -p $using:BuildDir `
        --checks='-*,misc-no-recursion' `
        "--header-filter=.*[\\/]src[\\/].*" `
        --quiet $f *> $log
    $warns = Get-Content $log | Where-Object { $_ -match 'warning:.*\[misc-no-recursion\]' }
    $warnCount = if ($warns) { @($warns).Count } else { 0 }
    "{0,-45} {1,6:N1}s  (warnings: {2})" -f $name, $sw.Elapsed.TotalSeconds, $warnCount
}
Write-Host "Recursion check completed."
