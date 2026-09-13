$ErrorActionPreference = "Stop"

$candidates = @(
    "$PSScriptRoot\..\..\build\Release\openrar.exe",
    "$PSScriptRoot\..\..\build\openrar.exe",
    "$PSScriptRoot\..\..\build\openrar64\Release\openrar.exe"
)
$openrar = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$rar = "C:\Program Files\WinRAR\rar.exe"
$testDir = "rar_a_openrar_x_test"

if (!(Test-Path $openrar)) { throw "openrar.exe not found at $openrar" }
if (!(Test-Path $rar)) { throw "rar.exe not found at $rar" }

Write-Host "Creating test files..."
if (Test-Path $testDir) { Remove-Item -Recurse -Force $testDir }
New-Item -ItemType Directory -Path $testDir | Out-Null
$srcFile = "$testDir\test.bin"
$fs = [System.IO.File]::Create($srcFile)
$fs.SetLength(15MB)
$fs.Close()
$origHash = (Get-FileHash $srcFile -Algorithm SHA256).Hash

Write-Host "Compressing with rar..."
$archive = "$testDir\test.rar"
& $rar a -m5 $archive $srcFile

Write-Host "Extracting with openrar..."
$extractDir = "$testDir\extracted"
New-Item -ItemType Directory -Path $extractDir | Out-Null
& $openrar x $archive $extractDir\

$extractedFile = Get-ChildItem -Path $extractDir -Recurse -Filter "test.bin" | Select-Object -First 1 -ExpandProperty FullName
if (!$extractedFile) { throw "test.bin not found after extraction" }
$newHash = (Get-FileHash $extractedFile -Algorithm SHA256).Hash

if ($origHash -ne $newHash) {
    throw "Hash mismatch! Expected $origHash, got $newHash"
}

Write-Host "rar a -> openrar x PASSED." -ForegroundColor Green
Remove-Item -Recurse -Force $testDir
