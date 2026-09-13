$ErrorActionPreference = "Stop"

$candidates = @(
    "$PSScriptRoot\..\..\build\Release\openrar.exe",
    "$PSScriptRoot\..\..\build\openrar.exe",
    "$PSScriptRoot\..\..\build\openrar64\Release\openrar.exe"
)
$openrar = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$unrar = "C:\Program Files\WinRAR\UnRAR.exe"
$testDir = "openrar_a_unrar_x_test"

if (!(Test-Path $openrar)) { throw "openrar.exe not found at $openrar" }
if (!(Test-Path $unrar)) { throw "unrar.exe not found at $unrar" }

Write-Host "Creating test files..."
if (Test-Path $testDir) { Remove-Item -Recurse -Force $testDir }
New-Item -ItemType Directory -Path $testDir | Out-Null
$srcFile = "$testDir\test.bin"
$fs = [System.IO.File]::Create($srcFile)
$fs.SetLength(15MB)
$fs.Close()
$origHash = (Get-FileHash $srcFile -Algorithm SHA256).Hash

Write-Host "Compressing with openrar..."
$archive = "$testDir\test.rar"
& $openrar a $archive $srcFile

Write-Host "Extracting with unrar..."
$extractDir = "$testDir\extracted"
New-Item -ItemType Directory -Path $extractDir | Out-Null
& $unrar x -idq $archive $extractDir\

$extractedFile = Get-ChildItem -Path $extractDir -Recurse -Filter "test.bin" | Select-Object -First 1 -ExpandProperty FullName
if (!$extractedFile) { throw "test.bin not found after extraction" }
$newHash = (Get-FileHash $extractedFile -Algorithm SHA256).Hash

if ($origHash -ne $newHash) {
    throw "Hash mismatch! Expected $origHash, got $newHash"
}

Write-Host "openrar a -> unrar x PASSED." -ForegroundColor Green
Remove-Item -Recurse -Force $testDir
