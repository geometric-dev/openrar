param(
    [string]$Root = "",
    [string]$BuildDir = ""
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
$xdir = Join-Path $BuildDir 'extra'
$extraDbPath = Join-Path $xdir 'compile_commands.json'

if (-not (Test-Path $extraDbPath)) {
    $resExtra = Join-Path $PSScriptRoot "..\resources\extra_compile_commands.json"
    if (Test-Path $resExtra) {
        New-Item -ItemType Directory -Force -Path $xdir | Out-Null
        Copy-Item $resExtra $extraDbPath
    } else {
        throw "Extra compile_commands.json not found at $extraDbPath"
    }
}

# Locate Visual Studio tools
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -property installationPath
    if ($found) { $vsPath = $found }
}
$ct = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

$checks = @(
    '-*'
    'clang-analyzer-core.*'
    'clang-analyzer-cplusplus.*'
    'clang-analyzer-unix.*'
    'clang-analyzer-security.*'
    '-clang-analyzer-security.insecureAPI.*'
    'clang-analyzer-alpha.unix.SimpleStream'
    'clang-analyzer-alpha.cplusplus.SmartPtr'
    'clang-analyzer-alpha.cplusplus.InvalidatedIterator'
    'clang-analyzer-alpha.cplusplus.IteratorRange'
    'clang-analyzer-alpha.cplusplus.MismatchedIterator'
    'clang-analyzer-alpha.security.ReturnPtrRange'
    'clang-analyzer-alpha.unix.cstring.OutOfBounds'
    'clang-analyzer-alpha.unix.cstring.BufferOverlap'
    'clang-analyzer-alpha.unix.cstring.UninitializedRead'
    'clang-analyzer-alpha.core.PointerArithm'
    'clang-analyzer-optin.taint.TaintedAlloc'
    'bugprone-unique-ptr-array-mismatch'
    'bugprone-shared-ptr-array-mismatch'
    'bugprone-unhandled-self-assignment'
    'bugprone-use-after-move'
    'bugprone-dangling-handle'
    'bugprone-return-const-ref-from-parameter'
    'cert-oop54-cpp'
    'cert-mem57-cpp'
    'bugprone-implicit-widening-of-multiplication-result'
    'bugprone-misplaced-widening-cast'
    'bugprone-too-small-loop-variable'
    'bugprone-signed-char-misuse'
    'bugprone-sizeof-expression'
    'bugprone-sizeof-container'
    'bugprone-not-null-terminated-result'
    'bugprone-misplaced-pointer-arithmetic-in-alloc'
    'bugprone-undefined-memory-manipulation'
    'bugprone-suspicious-memset-usage'
    'bugprone-suspicious-memory-comparison'
    'bugprone-suspicious-stringview-data-usage'
    'bugprone-string-constructor'
    'bugprone-infinite-loop'
    'bugprone-unchecked-optional-access'
    'bugprone-pointer-arithmetic-on-polymorphic-object'
    'bugprone-narrowing-conversions'
    'bugprone-unsafe-functions'
) -join ','

$db = Get-Content $extraDbPath -Raw | ConvertFrom-Json

Write-Host "Running Clang-Tidy on extra files (worker_main.cpp, archive_api.cpp)..."
foreach ($e in $db) {
    $name = ($e.file -replace '^.*?/src/', '') -replace '[\\/]', '_'
    $log = Join-Path $xdir ($name + '.tidy.txt')
    Write-Host "  Analyzing $name with clang-tidy..."
    & $ct -p $xdir --checks=$checks --allow-enabling-analyzer-alpha-checkers `
        --extra-arg=-Xclang --extra-arg=-analyzer-config --extra-arg=-Xclang --extra-arg=aggressive-binary-operation-simplification=true `
        "--header-filter=.*[\\/]src[\\/].*" --quiet $e.file *> $log
}

Write-Host "Running MSVC /analyze on extra files..."
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}

$ruleset = Join-Path $BuildDir 'leaks_overflows.ruleset'
if (-not (Test-Path $ruleset)) {
    $ruleset = Join-Path $PSScriptRoot '..\resources\leaks_overflows.ruleset'
}

foreach ($e in $db) {
    $name = ($e.file -replace '^.*?/src/', '') -replace '[\\/]', '_'
    $log = Join-Path $xdir ($name + '.msvc.txt')
    Write-Host "  Analyzing $name with cl /analyze..."
    $cmd = $e.command -replace '\s/Fo\S+', '' -replace '\s/Fd\S+', '' -replace '\s/RTC1', ''
    $cmd = $cmd + " /analyze:only /analyze:external- /external:anglebrackets /external:W0 /analyze:ruleset `"$ruleset`" /analyze:plugin EspXEngine.dll"
    Push-Location $e.directory
    cmd /c "$cmd 2>&1" *> $log
    Pop-Location
}
Write-Host "Extra analysis completed."
