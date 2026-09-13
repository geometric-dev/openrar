# test_crypt.ps1 — Smoke-tests for AES-256-CBC RAR5 encryption (spec §4, §8).
#
# Golden archives (password = "secret", stored/no compression, RAR5):
#   hello5_p.rar   — file-data encrypted only (-p), headers in clear.
#   hello5_hp.rar  — headers + file data encrypted (-hp).
#
# Prerequisites:
#   - Built binary at  ..\build\openrar64\Release\openrar.exe (or pass -OpenrarExe)
#   - Golden archives  hello5_p.rar  and  hello5_hp.rar  in $PSScriptRoot
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_crypt.ps1
#
param([string]$OpenrarExe = "")
if ([string]::IsNullOrEmpty($OpenrarExe)) {
  $candidates = @(
    "$PSScriptRoot\..\build\Release\openrar.exe",
    "$PSScriptRoot\..\build\openrar.exe",
    "$PSScriptRoot\..\build\openrar64\Release\openrar.exe"
  )
  $OpenrarExe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
$ErrorActionPreference = 'Stop'

$Failures = 0

function Assert-True([bool]$Condition,[string]$Label) {
  if ($Condition) {
    Write-Host "  PASS  $Label" -ForegroundColor Green
  } else {
    Write-Host "  FAIL  $Label" -ForegroundColor Red
    $script:Failures++
  }
}

function Invoke-Openrar([string[]]$ArgList) {
  $proc = Start-Process -FilePath $OpenrarExe `
                        -ArgumentList $ArgList `
                        -NoNewWindow -Wait -PassThru `
                        -RedirectStandardOutput "$env:TEMP\openrar_stdout.txt" `
                        -RedirectStandardError  "$env:TEMP\openrar_stderr.txt"
  $out  = (Get-Content "$env:TEMP\openrar_stdout.txt" -Raw -ErrorAction SilentlyContinue) + `
          (Get-Content "$env:TEMP\openrar_stderr.txt"  -Raw -ErrorAction SilentlyContinue)
  return [PSCustomObject]@{ Output = $out; ExitCode = $proc.ExitCode }
}

if (-not (Test-Path $OpenrarExe)) {
  Write-Error "openrar.exe not found at '$OpenrarExe'. Build the project first."
  exit 1
}

$PArchive  = "$PSScriptRoot\hello5_p.rar"
$HpArchive = "$PSScriptRoot\hello5_hp.rar"
foreach ($f in $PArchive,$HpArchive) {
  if (-not (Test-Path $f)) { Write-Error "Missing test archive: $f"; exit 1 }
}

$OutDir = "$PSScriptRoot\out_crypt"
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item $OutDir -ItemType Directory | Out-Null

Write-Host "`n=== Encryption smoke tests ===" -ForegroundColor Cyan

# Test 1: extract -p archive with correct password
Write-Host "`n[1] File-encrypted (-p), correct password"
$r = Invoke-Openrar @("x", "-y", "-psecret", $PArchive, "$OutDir\p_ok/")
Assert-True ($r.ExitCode -eq 0) "exit code 0 for correct password"
Assert-True ($r.Output -notmatch "(?i)(wrong|bad|incorrect|error)") "no error in output"

$helloTxt = "$OutDir\p_ok\hello.txt"
if (Test-Path $helloTxt) {
  $content = (Get-Content $helloTxt -Raw).Replace("`r`n","`n")
  Assert-True ($content.StartsWith("Hello from open-rar!")) "hello.txt content correct"
} else {
  Assert-True $false "hello.txt was extracted"
}

# Test 2: list -hp archive with correct password (filenames visible)
Write-Host "`n[2] Header+file-encrypted (-hp), correct password listing"
$r = Invoke-Openrar @("l", "-psecret", $HpArchive)
Assert-True ($r.ExitCode -eq 0) "exit code 0"
Assert-True ($r.Output -match "hello\.txt") "hello.txt visible in listing"
Assert-True ($r.Output -match "data\.bin")  "data.bin visible in listing"

# Test 3: full extract -hp archive with correct password
Write-Host "`n[3] Header+file-encrypted (-hp), full extraction"
$r = Invoke-Openrar @("x", "-y", "-psecret", $HpArchive, "$OutDir\hp_ok/")
Assert-True ($r.ExitCode -eq 0) "exit code 0"
$helloTxt2 = "$OutDir\hp_ok\hello.txt"
if (Test-Path $helloTxt2) {
  $content2 = (Get-Content $helloTxt2 -Raw).Replace("`r`n","`n")
  Assert-True ($content2.StartsWith("Hello from open-rar!")) "hello.txt content correct"
} else {
  Assert-True $false "hello.txt was extracted"
}

# Test 4: wrong password on -p archive (must fail)
Write-Host "`n[4] File-encrypted (-p), wrong password must reject"
$r = Invoke-Openrar @("x", "-y", "-pwrong", $PArchive, "$OutDir\p_bad/")
Assert-True ($r.ExitCode -ne 0) "non-zero exit for wrong password"
Assert-True ($r.Output -match "(?i)(incorrect password|bad password|wrong password)") "BADPSW message in output"

# Test 5: wrong password on -hp archive (must fail)
Write-Host "`n[5] Header-encrypted (-hp), wrong password must reject"
$r = Invoke-Openrar @("l", "-pwrong", $HpArchive)
Assert-True ($r.ExitCode -ne 0) "non-zero exit for wrong password on -hp"

# Test 6: no password on -p archive -- headers in clear, listing still works
Write-Host "`n[6] File-encrypted (-p), no password -- headers must be visible"
$r = Invoke-Openrar @("l", $PArchive)
Assert-True ($r.ExitCode -eq 0) "exit code 0 for plain listing"
Assert-True ($r.Output -match "hello\.txt") "hello.txt visible without password"

# Summary
Write-Host ""
if ($Failures -eq 0) {
  Write-Host "All encryption tests passed." -ForegroundColor Green
  exit 0
} else {
  Write-Host "Tests FAILED." -ForegroundColor Red
  exit 1
}
