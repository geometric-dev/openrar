follow-up: cli_tests cdc_packed_rr_repair is flaky on windows-2022 x64

STATUS: OPEN. Cause still unproven. One real robustness gap closed, diagnostics
now in place - the next occurrence should name the cause instead of producing a
bare assert.

## Symptom

Observed on the v1.40.5 tag run (2026-10-12): the windows-2022 msvc (Release)
(x64) leg failed `cli_tests` at tests/unit/cli_tests.cpp:1685, inside
`cdc_packed_rr_repair`:

    openrar a -cdc -md1m -rr5 -q <arc> <base> <u1> <v1>
    assert(res == 0)   // the ARCHIVE CREATION itself returned non-zero

Not a regression: the identical commit passed on the master push run, and the
tag run passed on re-run (attempt 2, all 12 jobs green). So it is intermittent,
roughly 1 in 2 for this job, and it has only ever been seen on the Windows x64
leg.

## Why it still matters

`release-assets` gates on the whole matrix, so one flaky assert means no
artifacts publish until someone re-runs the job by hand. That happened on the
v1.40.5 tag and needed a manual `gh run rerun --failed`.

Note the failure is in archive CREATION with `-rr5` on `-cdc` input, not in the
repair path the test is named for. Nothing in 1.40.x changed the recovery
writer or the CDC planner, which is consistent with an old, unreached edge.

## What was ruled out

- **Environment-variable leakage** from earlier tests in the same process.
  `cli_tests` sets only `OPENRAR_CDC_INDEX_CAP` and a cancel hook, and both
  restore the variable before returning; `cdc_index_cap_falls_back_reported`
  runs five lines before the RR test and does reset. Checked, clean.
- **The command itself.** A standalone loop of the exact invocation, with the
  exact deterministic inputs rebuilt from `make_noise_bytes` (seeds 80/81/82,
  256 KiB, corruption at sz/3), ran 60 times with zero failures on this host.
  Same code, same data, no repro.

## Leading hypothesis (unproven)

An external agent - Defender, the Search indexer, a backup client - holds a
just-created temp file for a few milliseconds, and `CreateFileW` inside that
window fails with `ERROR_SHARING_VIOLATION`. On a fresh CI VM, freshly written
files are scanned hard; that fits an intermittent, Windows-only, fresh-runner
failure with no local reproduction.

`FileStream::open` had **no retry at all** on that path, so any such blip
failed a whole archive write. That gap is now fixed (see below).

## Changes made (commit 6184cb9)

1. `FileStream::open` retries `ERROR_SHARING_VIOLATION` and
   `ERROR_LOCK_VIOLATION` only, on the create/open-always dispositions, for a
   bounded ~375 ms (25 attempts, 15 ms apart). Genuine permission/path errors
   still fail immediately with their real `GetLastError`; read-only opens are
   deliberately not retried.
2. `io_tests` gained a Windows-only regression test for that contract: a real
   exclusive lock must be ridden out, and a lock held past the budget must
   still fail promptly. Verified non-vacuous - removing the retry makes it fail
   on exactly that assertion.
3. `cdc_packed_rr_repair` no longer redirects both child invocations to
   DEVNULL. They now capture to a log and print the child's stdout+stderr on
   failure, so the next occurrence is diagnosable.

## Next step

Wait for the next windows x64 failure and read the captured child output. The
exit code plus the CLI's own message should identify the cause directly - a
sharing-violation message confirms hypothesis 1 and closes this; anything else
means a genuine logic edge in the CDC/RR path and the next step is to narrow
it with that output in hand. Until then this stays open rather than "fixed".