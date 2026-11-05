# tools/provision_unrar.ps1
# OpenRAR v2.1.0 Track E - Interop Oracle Provisioning (Windows)
# Locates and validates UnRAR 7.20 / reference oracle with fail-closed SHA-256 pin.

param(
    [string]$CustomPath = $env:UNRAR_EXE
)

$ErrorActionPreference = "Stop"

$PINNED_VERSIONS = @{
    "7.20" = @{
        "Sha256" = "e785a0c0dcf79385f0483c1eb2242e3937d0447f6b21dd158906e92757b46542" # Canonical WinRAR/UnRAR 7.20 x64
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

$ExpectedHash = $PINNED_VERSIONS["7.20"].Sha256.ToLowerInvariant()
if ($Hash -ne $ExpectedHash) {
    Write-Error "[FATAL] Oracle SHA-256 mismatch! Found: $Hash, Expected: $ExpectedHash. Aborting."
    exit 1
}

# Check version output strictly 7.20
$VerOutput = & $Found 2>&1 | Select-String -Pattern "UNRAR 7\.20"
if (-not $VerOutput) {
    Write-Error "[FATAL] Oracle version is not strictly UnRAR 7.20! Aborting."
    exit 1
}
Write-Host "[ORACLE] Detected version: $($VerOutput.Matches[0].Value)"

Write-Host "[ORACLE] Provisioning successful and verified fail-closed."
exit 0

