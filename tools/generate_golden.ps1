# tools/generate_golden.ps1 - Deterministic Golden Archive Generator for OpenRAR
#
# Generates writer and mutator golden archives with fixed timestamps and seeds,
# along with companion .sha256 checksum files.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/generate_golden.ps1

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
$goldenWriterDir = "$fixturesDir\golden\writer"
$goldenMutatorDir = "$fixturesDir\golden\mutator"
$rar5Dir = "$fixturesDir\rar5"

New-Item -ItemType Directory -Force -Path $goldenWriterDir | Out-Null
New-Item -ItemType Directory -Force -Path $goldenMutatorDir | Out-Null
New-Item -ItemType Directory -Force -Path $rar5Dir | Out-Null

$scratch = "$root\build\golden_scratch"
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

# Fixed timestamp: 2026-01-01 00:00:00 UTC
$fixedDate = [DateTime]::SpecifyKind([DateTime]::Parse("2026-01-01T00:00:00.000Z"), [DateTimeKind]::Utc)

function Set-FixedTimestamp($path) {
    [System.IO.File]::SetLastWriteTimeUtc($path, $fixedDate)
    [System.IO.File]::SetCreationTimeUtc($path, $fixedDate)
    [System.IO.File]::SetLastAccessTimeUtc($path, $fixedDate)
}

function Write-Sha256Companion($archivePath) {
    $hash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLower()
    $leaf = Split-Path -Leaf $archivePath
    $shaPath = "$archivePath.sha256"
    "$hash *$leaf`n" | Set-Content -NoNewline -Path $shaPath -Encoding ASCII
}

# Create deterministic sample source files
$f1 = "$scratch\hello.txt"
"Hello from OpenRAR golden fixture!`n" | Set-Content -NoNewline -Path $f1 -Encoding ASCII
Set-FixedTimestamp $f1

$f2 = "$scratch\data.bin"
$bytes = New-Object byte[] 4096
$rng = New-Object System.Random(42)
$rng.NextBytes($bytes)
[System.IO.File]::WriteAllBytes($f2, $bytes)
Set-FixedTimestamp $f2

$f3 = "$scratch\extra.txt"
"Additional file for mutator add test.`n" | Set-Content -NoNewline -Path $f3 -Encoding ASCII
Set-FixedTimestamp $f3

Write-Host "=== Generating Writer Goldens ===" -ForegroundColor Cyan

# 1. writer_stored=comp=m0.rar
$arc1 = "$goldenWriterDir\writer_stored=comp=m0.rar"
if (Test-Path $arc1) { Remove-Item -Force $arc1 }
& $openrar a -m0 -ep -plain -y $arc1 $f1 $f2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to generate $arc1" }
Write-Sha256Companion $arc1
Write-Host "Created $arc1"

# 2. writer_compressed=comp=m3.rar
$arc2 = "$goldenWriterDir\writer_compressed=comp=m3.rar"
if (Test-Path $arc2) { Remove-Item -Force $arc2 }
& $openrar a -m3 -ep -plain -y $arc2 $f1 $f2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to generate $arc2" }
Write-Sha256Companion $arc2
Write-Host "Created $arc2"

# 3. writer_solid=solid=1=comp=m3.rar
$arc3 = "$goldenWriterDir\writer_solid=solid=1=comp=m3.rar"
if (Test-Path $arc3) { Remove-Item -Force $arc3 }
& $openrar a -s -m3 -ep -plain -y $arc3 $f1 $f2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to generate $arc3" }
Write-Sha256Companion $arc3
Write-Host "Created $arc3"

Write-Host "=== Generating Mutator Goldens ===" -ForegroundColor Cyan

# Baseline archive for mutator operations
$baseArc = "$scratch\mutator_base.rar"
if (Test-Path $baseArc) { Remove-Item -Force $baseArc }
& $openrar a -m0 -ep -plain -y $baseArc $f1 $f2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to generate $baseArc" }

# 1. mutator_add_file=action=add.rar
$mArc1 = "$goldenMutatorDir\mutator_add_file=action=add.rar"
Copy-Item -Force $baseArc $mArc1
& $openrar a -m0 -ep -plain -y $mArc1 $f3 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to update $mArc1" }
Write-Sha256Companion $mArc1
Write-Host "Created $mArc1"

# 2. mutator_delete_file=action=del.rar
$mArc2 = "$goldenMutatorDir\mutator_delete_file=action=del.rar"
Copy-Item -Force $baseArc $mArc2
& $openrar d -plain -y $mArc2 "data.bin" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to delete from $mArc2" }
Write-Sha256Companion $mArc2
Write-Host "Created $mArc2"

# 3. mutator_lock=action=lock.rar
$mArc3 = "$goldenMutatorDir\mutator_lock=action=lock.rar"
Copy-Item -Force $baseArc $mArc3
& $openrar k -plain -y $mArc3 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to lock $mArc3" }
Write-Sha256Companion $mArc3
Write-Host "Created $mArc3"

# Also copy existing synthetic coverage archives to tests/fixtures/rar5/
Write-Host "=== Populating Feature Coverage in tests/fixtures/rar5/ ===" -ForegroundColor Cyan
$existingRars = @("hello5.rar", "hello5_p.rar", "hello5_hp.rar")
foreach ($r in $existingRars) {
    $src = "$root\tests\$r"
    if (Test-Path $src) {
        $dst = "$rar5Dir\$r"
        Copy-Item -Force $src $dst
        Write-Sha256Companion $dst
        Write-Host "Copied $r to $dst"
    }
}

Remove-Item -Recurse -Force $scratch

# ── Cross-engine decode verification (v1.31.0 Gate 0 §2d) ────────────────────
# Every golden archive must extract hash-exact under the reference decoders
# before the new bytes are committed. Emitted bytes change whenever the
# encoder changes; UnRAR/WinRAR acceptance is the format-legality proof for
# the re-baseline. Skipped (with a loud marker) when the tools are absent.
Write-Host "`n=== Cross-Engine Decode Verification (UnRAR + WinRAR) ===" -ForegroundColor Cyan
$unrar = "C:\Program Files\WinRAR\UnRAR.exe"
$rar   = "C:\Program Files\WinRAR\rar.exe"
$goldenRars = Get-ChildItem -Path "$fixturesDir\golden" -Filter "*.rar" -Recurse
$verifyRoot = "$root\build\golden_scratch_verify"
New-Item -ItemType Directory -Force -Path $verifyRoot | Out-Null
$verFailed = 0
foreach ($file in $goldenRars) {
    # Extract with openrar first to learn the entry set, then require a
    # byte-identical extracted tree from both reference engines.
    $selfOut = "$verifyRoot\self"
    if (Test-Path $selfOut) { Remove-Item -Recurse -Force $selfOut }
    New-Item -ItemType Directory -Force -Path $selfOut | Out-Null
    & $openrar x -y -q $file.FullName $selfOut | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] openrar cannot decode $file" -ForegroundColor Red; $verFailed++; continue }
    $refFailed = $false
    foreach ($engine in @(@("UnRAR", $unrar, @("x", "-y")), @("WinRAR", $rar, @("x", "-y", "-inul")))) {
        if (-not (Test-Path $engine[1])) {
            Write-Host "[SKIP] $($engine[0]) not installed - cross-engine verification incomplete for this run" -ForegroundColor Yellow
            continue
        }
        $outDir = "$verifyRoot\$($engine[0])"
        if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        & $engine[1] $engine[2][0] $engine[2][1] $engine[2][2] $file.FullName "$outDir\" | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($engine[0]) rejected $($file.Name)" -ForegroundColor Red; $refFailed = $true; continue }
        $selfHash = (Get-ChildItem $selfOut -Recurse -File | Sort-Object FullName |
            ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash + ":" + $_.Name }) -join "`n"
        $refHash = (Get-ChildItem $outDir -Recurse -File | Sort-Object FullName |
            ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash + ":" + $_.Name }) -join "`n"
        if ($selfHash -ne $refHash) {
            Write-Host "[FAIL] $($engine[0]) tree differs for $($file.Name)" -ForegroundColor Red
            $refFailed = $true
        }
    }
    if (-not $refFailed) { Write-Host "[OK] $($file.Name) decodes identically" -ForegroundColor Green } else { $verFailed++ }
}
Remove-Item -Recurse -Force $verifyRoot -ErrorAction SilentlyContinue
if ($verFailed -gt 0) { throw "$verFailed golden archive(s) failed cross-engine verification" }
Write-Host "Cross-engine verification complete." -ForegroundColor Green

Write-Host "`nAll golden fixtures generated successfully." -ForegroundColor Green
