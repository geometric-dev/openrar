# tools/ci-watch.ps1 - wait for a CI run on a ref and report whether it went green.
#
# Push-and-pray is the failure mode this exists to remove: a push returns
# immediately, the failure (if any) surfaces minutes later, and by then the
# context is gone. This resolves the run for a ref, waits for it to finish,
# prints per-job results, and EXITS NON-ZERO if anything failed - so it can be
# chained (push; watch; tag) and cannot be forgotten.
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses a BOM-less .ps1 as ANSI,
# so a non-ASCII literal in a script file is mangled before any profile can
# help. Keep this file ASCII.
#
# Usage:
#   tools\ci-watch.ps1                          # current branch, HEAD sha
#   tools\ci-watch.ps1 -Ref master -Sha abc123 # explicit
#   tools\ci-watch.ps1 -Ref v1.40.5             # a tag
#   tools\ci-watch.ps1 -TimeoutMin 60 -Quiet    # less chatter
#
# Exit codes: 0 = all jobs green (or skipped, e.g. release-assets off-tag)
#             1 = at least one job failed
#             2 = timed out, or no run could be resolved

[CmdletBinding()]
param(
    [string] $Ref = "",
    [string] $Sha = "",
    [int]    $TimeoutMin = 45,
    [int]    $PollSec = 20,
    [switch] $Quiet,
    [switch] $NoLogExcerpt
)

$ErrorActionPreference = 'Stop'

function Write-Status([string] $msg, [string] $color = 'Gray') {
    if (-not $Quiet) { Write-Host $msg -ForegroundColor $color }
}

# gh returns exit code 8 on "no results", which is a normal race right after a
# push (the workflow has not registered yet) rather than an error.
#
# Deliberately NO fallback to "the newest run": if the caller asked for a ref or
# a sha and no run matches, watching some other run and reporting ITS result is
# the exact push-and-pray failure this script exists to remove. Absence is
# reported as absence.
function Get-RunById([string] $id) {
    $json = & gh run view $id --json databaseId,headBranch,headSha,status,conclusion,workflowName 2>$null | Out-String
    if ($LASTEXITCODE -ne 0) { return $null }
    try {
        $o = $json | ConvertFrom-Json
        return $o
    } catch { return $null }
}

function Get-Runs([string] $ref, [string] $sha) {
    $args = @('run', 'list', '--limit', '20', '--json',
              'databaseId,headBranch,headSha,status,conclusion,event,workflowName')
    $json = & gh @args 2>$null | Out-String
    if ($LASTEXITCODE -ne 0) { return @() }
    $runs = @()
    try { $runs = @($json | ConvertFrom-Json) } catch { return @() }
    if ($runs.Count -eq 0) { return @() }
    if ($sha -ne '') {
        $s = $runs | Where-Object { $_.headSha -like "$sha*" } | Select-Object -First 1
        if ($s) { return @($s) }
    }
    if ($ref -ne '') {
        # A tag shows up under headBranch too, so this covers both.
        $r = $runs | Where-Object { $_.headBranch -eq $ref } | Select-Object -First 1
        if ($r) { return @($r) }
    }
    return @()
}

if ($Ref -eq '' -and $Sha -eq '') {
    $Ref = (& git rev-parse --abbrev-ref HEAD 2>$null | Out-String).Trim()
    $Sha = (& git rev-parse HEAD 2>$null | Out-String).Trim()
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host "gh not found on PATH - cannot verify CI." -ForegroundColor Red
    exit 2
}

$deadline = (Get-Date).AddMinutes($TimeoutMin)
$target = $null
$shownQueued = $false

Write-Host "waiting for CI on '${Ref}'${Sha} (timeout ${TimeoutMin}m)..." -ForegroundColor Cyan

while ((Get-Date) -lt $deadline) {
    # A bare numeric argument that resolves as a run id is treated as one:
    # people naturally paste `gh run view <id>` output into a watcher.
    if ($Sha -match '^\d{6,}$' -and $null -eq $target) {
        $target = Get-RunById $Sha
    }
    if ($null -eq $target) {
        # Wrap at the call site: a function returning a single element unwraps
        # it to a scalar, so .Count would be empty without the @().
        $runs = @(Get-Runs $Ref $Sha)
        if ($runs.Count -gt 0) { $target = $runs[0] }
    }
    if ($null -ne $target) {
        if (-not $shownQueued) {
            Write-Status ("  run {0}: {1} ({2})" -f $target.databaseId, $target.status, $target.workflowName) 'DarkGray'
            $shownQueued = $true
        }
        if ($target.status -eq 'completed') { break }
    }
    Start-Sleep -Seconds $PollSec
}

if ($null -eq $target) {
    Write-Host "No CI run matched. If you just pushed, it may not have" -ForegroundColor Red
    Write-Host "registered yet - re-run this, or check the Actions tab." -ForegroundColor Red
    Write-Host "(This script will NOT silently watch a different run.)" -ForegroundColor DarkGray
    exit 2
}
if ($target.status -ne 'completed') {
    Write-Host ("Timed out after ${TimeoutMin}m; run is still '{0}'." -f $target.status) -ForegroundColor Yellow
    Write-Host ("Watch it with:  gh run watch {0}" -f $target.databaseId) -ForegroundColor Yellow
    exit 2
}

# Per-job breakdown.
$jobsJson = & gh run view $target.databaseId --json jobs 2>$null | Out-String
$jobs = @()
try { $jobs = @(($jobsJson | ConvertFrom-Json).jobs) } catch { $jobs = @() }

Write-Host ""
Write-Host "run $($target.databaseId) - $($target.conclusion)" -ForegroundColor Cyan
foreach ($j in $jobs) {
    $c = $j.conclusion
    $col = 'Gray'
    if ($c -eq 'success') { $col = 'Green' }
    elseif ($c -eq 'skipped' -or $c -eq '') { $col = 'DarkGray' }
    else { $col = 'Red' }
    Write-Host ("  {0,-48} {1}" -f $j.name, $c) -ForegroundColor $col
}

# A skipped job is only interesting when it should have run. release-assets is
# gated on a version tag, so it is skipped on branch pushes by design.
$failed = @($jobs | Where-Object { $_.conclusion -ne 'success' -and $_.conclusion -ne 'skipped' -and $_.conclusion -ne '' })

if ($failed.Count -eq 0) {
    Write-Host ""
    Write-Host "ALL GREEN" -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host ("FAILED: {0} job(s): {1}" -f $failed.Count, (($failed | ForEach-Object { $_.name }) -join ', ')) -ForegroundColor Red

if (-not $NoLogExcerpt) {
    $log = & gh run view $target.databaseId --log-failed 2>$null | Out-String
    $lines = @($log -split "`n") |
        Where-Object { $_ -match 'FAIL\]|Assertion failed|Assertion `|error:|Error:|Tests failed|The following tests FAILED|\*\*\*Failed|FAILED' } |
        Select-Object -First 25
    if ($lines.Count -gt 0) {
        Write-Host ""
        Write-Host "--- failure excerpt ---" -ForegroundColor Red
        foreach ($l in $lines) { Write-Host $l.Trim() -ForegroundColor DarkYellow }
    }
    Write-Host ""
    Write-Host ("Full log:  gh run view {0} --log-failed" -f $target.databaseId) -ForegroundColor DarkGray
}
exit 1