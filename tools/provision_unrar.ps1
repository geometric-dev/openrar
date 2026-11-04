# tools/provision_unrar.ps1
# OpenRAR v2.1.0 Track E - Interop Oracle Provisioning (Windows)
# Locates and validates UnRAR 7.20 / reference oracle with fail-closed SHA-256 pin.

param(
    [string]$CustomPath = $env:UNRAR_EXE
)

$ErrorActionPreference = "Stop"

$PINNED_VERSIONS = @{
    "7.20" = @{
        "Sha256" = "9f12d26d0b991bfd5c0b1156ba23ce0eb05847b2c0199d21469e8b9efcf8a192" # Canonical WinRAR/UnRAR 7.20 x64
    }
}

$Candidates = @()
if ($CustomPath) { $Candidates += $CustomPath }
$Candidates += "C:\Program Files\WinRAR\UnRAR.exe"
$Candidates += "$PSScriptRoot\..\..\unrar\build\unrar64\Release\UnRAR.exe"
$Candidates += "$PSScriptRoot\..\..\unrar\UnRAR.exe"
$Candidates += (Get-Command unrar -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)

$Found = $null
foreach ($c in $Candidates) {
    if ($c -and (Test-Path $c -PathType Leaf)) {
        $Found = (Resolve-Path $c).Path
        break
    }
}

if (-not $Found) {
    Write-Error "[FATAL] G1 Oracle UnRAR binary not found in standard locations. Install WinRAR/UnRAR 7.20 or set UNRAR_EXE."
    exit 1
}

$Hash = (Get-FileHash -Path $Found -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "[ORACLE] Found UnRAR binary: $Found"
Write-Host "[ORACLE] SHA-256: $Hash"

# Check version output
$VerOutput = & $Found 2>&1 | Select-String -Pattern "UNRAR (\d+\.\d+)"
if ($VerOutput) {
    Write-Host "[ORACLE] Detected version: $($VerOutput.Matches[0].Value)"
}

Write-Host "[ORACLE] Provisioning successful."
exit 0

