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

$tidyDir = Join-Path $BuildDir "tidy"
$msvcDir = Join-Path $BuildDir "msvc"
$extraDir = Join-Path $BuildDir "extra"

# 1. Clang-Tidy findings deduplication & export
$tidyFiles = @()
if (Test-Path $tidyDir) {
    $tidyFiles += Get-ChildItem -Path $tidyDir -Filter "*.txt" | Where-Object { $_.Name -notmatch '\.recursion\.txt$' }
}
if (Test-Path $extraDir) {
    $tidyFiles += Get-ChildItem -Path $extraDir -Filter "*.tidy.txt"
}

if ($tidyFiles.Count -gt 0) {
    $rxTidy = '^(?<file>[A-Za-z]:\\[^:]+):(?<line>\d+):(?<col>\d+): (?<sev>warning|error): (?<msg>.*) \[(?<check>[\w.,-]+)\]$'
    $findings = $tidyFiles | ForEach-Object {
        Get-Content $_.FullName | ForEach-Object {
            if ($_ -match $rxTidy) {
                $rawFile = $matches.file
                $normFile = $rawFile
                if ($normFile.StartsWith($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $normFile = $normFile.Substring($Root.Length).TrimStart('\', '/')
                }
                [pscustomobject]@{
                    File  = $normFile
                    Line  = [int]$matches.line
                    Check = $matches.check
                    Msg   = $matches.msg
                }
            }
        }
    } | Sort-Object File, Line, Check, Msg -Unique

    $tidyCsv = Join-Path $BuildDir "tidy_findings.csv"
    $findings | Export-Csv $tidyCsv -NoTypeInformation
    Write-Host "Exported $($findings.Count) unique clang-tidy findings to $tidyCsv"
} else {
    Write-Host "No clang-tidy logs found under $tidyDir"
}

# 2. MSVC /analyze findings deduplication & export
$msvcFiles = @()
if (Test-Path $msvcDir) {
    $msvcFiles += Get-ChildItem -Path $msvcDir -Filter "*.txt"
}
if (Test-Path $extraDir) {
    $msvcFiles += Get-ChildItem -Path $extraDir -Filter "*.msvc.txt"
}

if ($msvcFiles.Count -gt 0) {
    $rxMsvc = '^(?<file>[A-Za-z]:\\[^()]+)\((?<line>\d+)\)\s*:\s*(?<sev>warning|error)\s+(?<code>C\d+)\s*:\s*(?<msg>.*)$'
    $findingsMsvc = $msvcFiles | ForEach-Object {
        Get-Content $_.FullName | ForEach-Object {
            if ($_ -match $rxMsvc) {
                $rawFile = $matches.file
                $normFile = $rawFile
                if ($normFile.StartsWith($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $normFile = $normFile.Substring($Root.Length).TrimStart('\', '/')
                }
                [pscustomobject]@{
                    File = $normFile
                    Line = [int]$matches.line
                    Code = $matches.code
                    Msg  = $matches.msg
                }
            }
        }
    } | Sort-Object File, Line, Code, Msg -Unique

    $msvcCsv = Join-Path $BuildDir "msvc_findings.csv"
    $findingsMsvc | Export-Csv $msvcCsv -NoTypeInformation
    Write-Host "Exported $($findingsMsvc.Count) unique MSVC /analyze findings to $msvcCsv"
} else {
    Write-Host "No MSVC logs found under $msvcDir"
}
