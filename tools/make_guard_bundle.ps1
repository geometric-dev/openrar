# Build the self-contained regression-guard bundle dist\openrar-guard.exe, the sibling of
# tools\make_bench_bundle.ps1 (dist\openrar-bench.exe). Purpose: run
# tools\release_regression_guard.py on a separate, quiet machine that has NO checkout, NO
# Python and NO git, and bring back one file: %USERPROFILE%\openrar-guard-results.json.
#
# Layout inside the SFX (extracted into %TEMP%\OpenRAR-<hex>, deleted afterwards):
#   python.exe + runtime       Python embeddable package (pinned version + SHA-256 below)
#   tools\                     release_regression_guard.py, bench_m0_census.py
#   build\m0_census\           the 8 pinned corpora (SHA-256 verified here AND on the target;
#                              this is what removes the need for git and the v2.0.23 tag)
#   build\openrar64\Release\   openrar.exe  = the binary under test (from this repo)
#   bin\v2.0.23\               openrar.exe  = the mandatory baseline (gitignored, local copy)
#   bundle_info.json           build identity of the binary under test (copied into the result)
#   run-guard.cmd              same command the Setup directive runs, for the .zip variant
#
# Nothing proprietary is bundled (no WinRAR/UnRAR: the guard compares OpenRAR to OpenRAR
# v2.0.23 only). Setup= runs `python.exe tools\release_regression_guard.py --report --pause`.
#
# Smoke (no directives): dist\openrar-guard.exe -sfxnoexec  (this script does it for you and
# also runs the guard's --selftest from the extracted payload).
#
# Usage:
#   powershell -File tools\make_guard_bundle.ps1                 # SFX
#   powershell -File tools\make_guard_bundle.ps1 -Zip            # SFX + plain .zip
#   -AllowStale permits a binary older than the newest source file (default: refuse).

param(
    [string]$Config = "Release",
    [string]$Out = "dist\openrar-guard.exe",
    [string]$OpenrarExe = "",
    [string]$BaselineExe = "",
    [string]$Stub = "",
    [string]$PythonEmbedZip = "",
    [switch]$SkipSmoke,
    [switch]$AllowStale,
    [switch]$Zip,
    [string]$ZipOut = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

# Pinned runtime. SHA-256 is the one python.org publishes on the release page for the
# "Windows embeddable package (64-bit)". Bump both together.
$PyVersion = "3.14.3"
$PyUrl = "https://www.python.org/ftp/python/$PyVersion/python-$PyVersion-embed-amd64.zip"
$PySha256 = "ad4961a479dedbeb7c7d113253f8db1b1935586b73c27488712beec4f2c894e6"

# The corpora the guard needs that cannot be regenerated without git + the v2.0.23 tag.
# (real_binary.bin is made by the guard from the baseline exe; zeros8/random8/synthetic/
# mixed16 are shipped too so the run does not spend minutes regenerating 8 MiB of PRNG bytes.)
$Corpora = @("zeros8.bin", "random8.bin", "synthetic_text8.txt", "mixed16.bin", "big24.bin",
             "real_src.txt", "real_tests.txt", "real_docs.md")

function Resolve-File([string]$label, [string]$given, [string[]]$candidates) {
    if ($given -ne "") {
        if (-not (Test-Path -LiteralPath $given)) { throw "$label not found: $given" }
        return (Resolve-Path -LiteralPath $given).Path
    }
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path } }
    throw "$label not found; tried: $($candidates -join ', ')"
}
function Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }

$openrar = Resolve-File "openrar.exe (binary under test)" $OpenrarExe @(
    (Join-Path $Root "build\openrar64\$Config\openrar.exe"),
    (Join-Path $Root "build\Release\openrar.exe")
)
$baseline = Resolve-File "v2.0.23 baseline openrar.exe" $BaselineExe @(
    (Join-Path $Root "bin\v2.0.23\openrar.exe")
)
$stub = Resolve-File "Default.SFX" $Stub @(
    (Join-Path $Root "build\openrar64\$Config\Default.SFX.exe"),
    (Join-Path $Root "build\openrar64\Release\Default.SFX.exe")
)

# Refuse to package a binary that does not contain the current source.
$newestSrc = Get-ChildItem -Path (Join-Path $Root "src") -Recurse -Include *.cpp, *.hpp, *.h |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$exeTime = (Get-Item -LiteralPath $openrar).LastWriteTimeUtc
if ($newestSrc.LastWriteTimeUtc -gt $exeTime) {
    $msg = "binary under test is older than $($newestSrc.FullName) (exe $exeTime, source $($newestSrc.LastWriteTimeUtc)); rebuild first"
    if ($AllowStale) { Write-Warning $msg } else { throw "$msg (or pass -AllowStale)" }
}

$baseVer = (& $baseline --version | Select-Object -First 1).ToString().Trim()
if ($baseVer -notmatch '2\.0\.23') { throw "baseline reports '$baseVer', expected 2.0.23" }
$curVer = (& $openrar --version | Select-Object -First 1).ToString().Trim()

# Python embeddable runtime: use the cached zip, else download once; always verify the pin.
if ($PythonEmbedZip -eq "") {
    $cache = Join-Path $Root "dist\_cache"
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $PythonEmbedZip = Join-Path $cache "python-$PyVersion-embed-amd64.zip"
    if (-not (Test-Path -LiteralPath $PythonEmbedZip)) {
        Write-Host "downloading $PyUrl"
        Invoke-WebRequest -Uri $PyUrl -OutFile $PythonEmbedZip -UseBasicParsing
    }
}
if ((Sha256 $PythonEmbedZip) -ne $PySha256) {
    throw "Python embeddable SHA-256 mismatch for $PythonEmbedZip (want $PySha256)"
}

# Make sure every corpus exists and matches its pin, using the guard's own tables, in the
# repo (where git and the v2.0.23 tag are available). Fails closed if anything is off.
$prep = Join-Path $env:TEMP "openrar_guard_prep.py"
@'
import sys
sys.path.insert(0, "tools")
import release_regression_guard as g, bench_m0_census as b
base = g.locate_baseline(sys.argv[1])
c = b.generate_corpora()
c.update(g.build_real_corpora(base))
for name in sorted(c):
    print("CORPUS", name, c[name])
'@ | Set-Content -LiteralPath $prep -Encoding ascii
Push-Location $Root
try {
    $prepOut = & python $prep $baseline
    if ($LASTEXITCODE -ne 0) { throw "corpus preparation failed (see output above)" }
} finally { Pop-Location; Remove-Item -LiteralPath $prep -ErrorAction SilentlyContinue }
$corpusDir = Join-Path $Root "build\m0_census"
foreach ($n in $Corpora) {
    if (-not (Test-Path -LiteralPath (Join-Path $corpusDir $n))) { throw "corpus missing after preparation: $n" }
}

$outPath = Join-Path $Root $Out
$outDir = Split-Path -Parent $outPath
$stage = Join-Path $outDir "guard-stage"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Expand-Archive -LiteralPath $PythonEmbedZip -DestinationPath $stage -Force
foreach ($d in "tools", "build\m0_census", "build\openrar64\Release", "bin\v2.0.23") {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $d) | Out-Null
}
Copy-Item (Join-Path $Root "tools\release_regression_guard.py") (Join-Path $stage "tools")
Copy-Item (Join-Path $Root "tools\bench_m0_census.py") (Join-Path $stage "tools")
foreach ($n in $Corpora) { Copy-Item (Join-Path $corpusDir $n) (Join-Path $stage "build\m0_census") }
Copy-Item $openrar (Join-Path $stage "build\openrar64\Release\openrar.exe")
Copy-Item $baseline (Join-Path $stage "bin\v2.0.23\openrar.exe")
# Bare -sfx resolves "default.sfx" against cwd (ArchiveMutator::resolve_sfx_stub); stage it
# under that name, NOT packed.
Copy-Item $stub (Join-Path $stage "default.sfx")

$corpusHashes = [ordered]@{}
foreach ($n in $Corpora) { $corpusHashes[$n] = Sha256 (Join-Path $corpusDir $n) }

$gitCommit = "unknown"; $gitDirty = $null
try {
    $gitCommit = (git -C $Root rev-parse HEAD).Trim()
    $gitDirty = [bool](git -C $Root status --porcelain)
} catch { }
[ordered]@{
    built_utc          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    git_commit         = $gitCommit
    git_tree_dirty     = $gitDirty
    under_test_version = $curVer
    under_test_sha256  = Sha256 $openrar
    baseline_version   = $baseVer
    baseline_sha256    = Sha256 $baseline
    python_embed       = $PyVersion
    python_embed_sha256 = $PySha256
    corpora_sha256     = $corpusHashes
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage "bundle_info.json") -Encoding utf8

$guardCmd = "python.exe tools\release_regression_guard.py --report --pause"
@(
    "@echo off",
    "cd /d ""%~dp0""",
    $guardCmd,
    "echo.",
    "pause"
) | Set-Content -LiteralPath (Join-Path $stage "run-guard.cmd") -Encoding ascii

$comment = Join-Path $outDir "guard-sfx-comment.txt"
@(
    "Title=OpenRAR Regression Guard"
    "Text=Runs the OpenRAR release regression guard (a few minutes; close other programs and leave the machine idle) and writes ONE result file: %USERPROFILE%\openrar-guard-results.json. Send that file back."
    "Setup=$guardCmd"
    "TempMode="
) | Set-Content -LiteralPath $comment -Encoding ascii

if (Test-Path -LiteralPath $outPath) { Remove-Item -Force -LiteralPath $outPath }

$items = Get-ChildItem -LiteralPath $stage | Where-Object { $_.Name -ne "default.sfx" } | ForEach-Object Name
Push-Location $stage
try {
    $commentAbs = (Resolve-Path -LiteralPath $comment).Path
    & $openrar a -sfx -r "-z$commentAbs" $outPath @items
    if ($LASTEXITCODE -ne 0) { throw "openrar a -sfx failed rc=$LASTEXITCODE" }
} finally { Pop-Location }

if (-not (Test-Path -LiteralPath $outPath)) {
    $alt = [System.IO.Path]::ChangeExtension($outPath, ".exe")
    if (-not (Test-Path -LiteralPath $alt)) { throw "bundle not produced: $outPath" }
    $outPath = $alt
}
$mb = [math]::Round((Get-Item -LiteralPath $outPath).Length / 1MB, 1)
Write-Host "bundle: $outPath ($mb MB)"
Write-Host "  under test : $curVer  sha256 $((Sha256 $openrar).Substring(0,16))  (git $($gitCommit.Substring(0,[math]::Min(9,$gitCommit.Length))), dirty=$gitDirty)"
Write-Host "  baseline   : $baseVer  sha256 $((Sha256 $baseline).Substring(0,16))"
Write-Host "  python     : $PyVersion embeddable (SHA-256 pinned)"

if ($Zip) {
    if ($ZipOut -eq "") { $ZipOut = "dist\openrar-guard.zip" }
    $zipPath = Join-Path $Root $ZipOut
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $zipPath) | Out-Null
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -Force -LiteralPath $zipPath }
    Compress-Archive -Force -DestinationPath $zipPath -Path ($items | ForEach-Object { Join-Path $stage $_ })
    Write-Host "zip: $zipPath (run run-guard.cmd after unpacking)"
}

if (-not $SkipSmoke) {
    # -sfxnoexec suppresses the directives (no consent prompt, no run). TempMode= still
    # extracts into a NEW %TEMP%\OpenRAR-<hex>, and the -sfxnoexec branch skips the cleanup,
    # so this smoke removes that dir itself (same as make_bench_bundle.ps1).
    $before = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'OpenRAR-*' -ErrorAction SilentlyContinue |
        ForEach-Object Name)
    & $outPath -sfxnoexec
    if ($LASTEXITCODE -ne 0) { throw "smoke extraction failed rc=$LASTEXITCODE" }
    $new = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'OpenRAR-*' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin $before })
    $selftestOk = $false
    foreach ($d in $new) {
        $must = @("python.exe", "tools\release_regression_guard.py", "tools\bench_m0_census.py",
                  "build\openrar64\Release\openrar.exe", "bin\v2.0.23\openrar.exe", "bundle_info.json",
                  "run-guard.cmd") + ($Corpora | ForEach-Object { "build\m0_census\$_" })
        $missing = @($must | Where-Object { -not (Test-Path -LiteralPath (Join-Path $d.FullName $_)) })
        if ($missing.Count -gt 0) { throw "smoke: payload incomplete, missing: $($missing -join ', ')" }
        # Run the guard's selftest from the extracted payload exactly as the target would
        # (no repo, no git, embedded interpreter): proves the bundle is self-contained.
        Push-Location $d.FullName
        try {
            & (Join-Path $d.FullName "python.exe") tools\release_regression_guard.py --selftest
            $selftestOk = ($LASTEXITCODE -eq 0)
        } finally { Pop-Location }
        Remove-Item -Recurse -Force -LiteralPath $d.FullName -ErrorAction SilentlyContinue
    }
    if (-not $selftestOk) { throw "smoke: guard --selftest failed from the extracted payload" }
    Write-Host "smoke: payload complete, --selftest passed from the extracted bundle, temp dir cleaned"
    Write-Host "release check (manual): run the bundle on the quiet machine, press A at the consent prompt."
}
