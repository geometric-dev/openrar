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
# Exit codes: 0 = the RUN concluded success. Jobs printed red on a success run
#             are continue-on-error legs (e.g. MSan (best-effort), which is red
#             on every green run by design) and are reported as NON-GATING.
#             1 = the run did not conclude success - a gating job failed, or
#                 the run was cancelled/failed and the job list shows why, or
#                 the list shows no failing job to explain a failed conclusion
#             2 = timed out, no run could be resolved, or the run's job list
#                 could not be read (NEVER reported as green - fail closed)

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
#
# Resolution is strict about which field identifies a run:
#   - a sha argument matches on headSha ONLY. When the caller passes a sha
#     (explicitly, or auto-filled from git HEAD), falling back to a headBranch
#     match would watch an older commit's run on the same branch and report its
#     result as ours - the bug this file exists to prevent.
#   - ref-only matching is the documented tag/branch mode (-Ref v1.40.5).
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
    # Assign the parse result BEFORE wrapping in @(). Windows PowerShell 5.1's
    # ConvertFrom-Json emits a top-level JSON array as ONE nested object (pwsh 7
    # enumerates it), so `@($json | ConvertFrom-Json)` produced a 1-element
    # array holding the whole run list. Downstream, Where-Object then saw
    # $_.headSha/headBranch as an ARRAY: `-like`/`-eq` returned an array of
    # booleans, and a non-empty array is truthy - so the filter passed the
    # nested list for any matching-ish pattern, @() in the caller flattened it,
    # and $target became runs[0]: the newest run. On 5.1, -Sha and -Ref both
    # silently watched an unrelated run and reported ITS verdict as ours -
    # precisely the failure this function exists to prevent (it only never bit
    # in practice because the newest run is usually the run just pushed).
    # Assignment gives both shells the same shape: $parsed = the array object,
    # @($parsed) = its elements.
    try {
        $parsed = ConvertFrom-Json $json
        $runs = @($parsed)
    } catch { return @() }
    if ($runs.Count -eq 0) { return @() }
    if ($sha -ne '') {
        # A run id (-Sha accepted a paste of `gh run view <id>` output) has no
        # headSha of its own; the dedicated Get-RunById loop handles it, so
        # skip list-based sha matching for that shape rather than reporting a
        # spurious "no match" while the id lookup is still the live path.
        if ($sha -match '^\d{6,}$') { return @() }
        $s = $runs | Where-Object { $_.headSha -like "$sha*" } | Select-Object -First 1
        if ($s) { return @($s) }
        # No sha match is final; do NOT fall through to $ref below.
        return @()
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

$watchLabel = if ($Ref -ne '' -and $Sha -ne '') { "${Ref} (${Sha})" } elseif ($Sha -ne '') { $Sha } else { $Ref }
Write-Host "waiting for CI on '$watchLabel' (timeout ${TimeoutMin}m)..." -ForegroundColor Cyan

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
$ghJobsExit = $LASTEXITCODE
$jobs = @()
try { $jobs = @(($jobsJson | ConvertFrom-Json).jobs) } catch { $jobs = @() }

# FAIL CLOSED. This tool's only job is to turn "CI went red" into a non-zero
# exit, so an inability to READ the job list must not produce "ALL GREEN".
# That was the bug: a non-zero gh exit here was ignored, $jobs stayed empty,
# the failed filter matched nothing, and the script exited 0. A gh auth
# blip, rate limit or network drop was reported as a green build. An empty
# job list is treated the same way: a completed run with zero resolved jobs
# is not information that the build is green, it is missing information.
$canAssessJobs = ($ghJobsExit -eq 0 -and $jobs.Count -gt 0)
if (-not $canAssessJobs) {
    Write-Host ""
    if ($ghJobsExit -ne 0) {
        Write-Host "Could not read the run's job list (gh exit $ghJobsExit)." -ForegroundColor Red
    } else {
        Write-Host "The run's job list is empty or unparseable." -ForegroundColor Red
    }
    Write-Host ("Run conclusion was '{0}' (id {1}); refusing to claim ALL GREEN." -f $target.conclusion, $target.databaseId) -ForegroundColor Red
    Write-Host ("Inspect it:  gh run view {0}" -f $target.databaseId) -ForegroundColor Yellow
    exit 2
}

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

# The gate is the RUN-LEVEL conclusion, not the derived job list. GitHub only
# concludes success when every required (non-continue-on-error) job succeeded,
# so a failed job on a success run is non-gating by definition - MSan
# (best-effort) is red on EVERY green run (its findings are uninstrumented-
# libc++ false positives; the job carries continue-on-error: true). Counting
# it as fatal made this script exit 1 on runs the platform itself considers
# green: it could never report ALL GREEN on any run, contradicting
# docs/versioning.md ("Exit 0 is the only green") and breaking the
# push; watch; tag chain this script exists to guard.
if ($target.conclusion -eq 'success') {
    if ($failed.Count -gt 0) {
        Write-Host ""
        Write-Host ("NON-GATING: {0} job(s) failed but the run concluded success (continue-on-error): {1}" -f $failed.Count, (($failed | ForEach-Object { $_.name }) -join ', ')) -ForegroundColor Yellow
        Write-Host ("Inspect them:  gh run view {0}" -f $target.databaseId) -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host "ALL GREEN" -ForegroundColor Green
    exit 0
}

if ($failed.Count -eq 0) {
    # The run-level conclusion is the independent cross-check: a workflow that
    # reported failure/cancelled with no failed job in the list means the list
    # does not describe what happened (e.g. a job-level failure outside the
    # normal conclusion flow, or cancellation before any job resolved). Trust
    # the run verdict over the derived one - never green.
    Write-Host ""
    Write-Host ("Run concluded '{0}' but no failing job was found in the job list - not treating as green." -f $target.conclusion) -ForegroundColor Red
    Write-Host ("Inspect it:  gh run view {0}" -f $target.databaseId) -ForegroundColor Yellow
    exit 1
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