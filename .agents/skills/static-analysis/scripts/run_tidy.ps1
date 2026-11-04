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

$checks = @(
    '-*'
    # leaks / lifetime
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
    # overflow / bounds
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

Write-Host "Running main clang-tidy checks on $($files.Count) file(s) (parallel: $ThrottleLimit)..."

$files | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
    $f = $_
    $name = ($f -replace '^.*?/src/', '') -replace '[\\/]', '_'
    $log = Join-Path $using:out ($name + '.txt')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $using:ct -p $using:BuildDir `
        --checks=$using:checks `
        --extra-arg=-Xclang --extra-arg=-analyzer-config --extra-arg=-Xclang --extra-arg=aggressive-binary-operation-simplification=true `
        --allow-enabling-analyzer-alpha-checkers `
        "--header-filter=.*[\\/]src[\\/].*" `
        --quiet $f *> $log
    "{0,-45} {1,6:N1}s" -f $name, $sw.Elapsed.TotalSeconds
}
Write-Host "ALL DONE"
