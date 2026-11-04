param(
    [string]$Root = "",
    [string]$BuildDir = "",
    [int]$ThrottleLimit = 4
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
$out = Join-Path $BuildDir 'msvc'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# Import VS developer environment
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -property installationPath
    if ($found) { $vsPath = $found }
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}

$ruleset = Join-Path $BuildDir 'leaks_overflows.ruleset'
if (-not (Test-Path $ruleset)) {
    $resRuleset = Join-Path $PSScriptRoot '..\resources\leaks_overflows.ruleset'
    if (Test-Path $resRuleset) {
        Copy-Item $resRuleset $ruleset
    } else {
        throw "leaks_overflows.ruleset not found at: $ruleset"
    }
}

$dbPath = Join-Path $BuildDir 'compile_commands.json'
if (-not (Test-Path $dbPath)) {
    throw "compile_commands.json not found at: $dbPath. Run generate_db.ps1 first."
}

$db = Get-Content $dbPath -Raw | ConvertFrom-Json

Write-Host "Running MSVC /analyze on $($db.Count) file(s) (parallel: $ThrottleLimit)..."

$db | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $e = $_
    $name = ($e.file -replace '^.*?/src/', '') -replace '[\\/]', '_'
    $log = Join-Path $using:out ($name + '.txt')
    # Drop object/pdb outputs and RTC (not needed for analysis-only runs).
    $cmd = $e.command -replace '\s/Fo\S+', '' -replace '\s/Fd\S+', '' -replace '\s/RTC1', ''
    $cmd = $cmd + " /analyze:only /analyze:external- /external:anglebrackets /external:W0 /analyze:ruleset `"$using:ruleset`" /analyze:plugin EspXEngine.dll"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Push-Location $e.directory
    cmd /c "$cmd 2>&1" *> $log
    Pop-Location
    "{0,-45} {1,6:N1}s" -f $name, $sw.Elapsed.TotalSeconds
}
Write-Host "ALL DONE"
