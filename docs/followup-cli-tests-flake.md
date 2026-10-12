follow-up: cli_tests cdc_packed_rr_repair is flaky on windows-2022 x64

Observed on the v1.40.5 tag run (2026-10-12): the windows-2022 msvc (Release)
(x64) leg failed `cli_tests` at tests/unit/cli_tests.cpp:1685, inside
`cdc_packed_rr_repair`:

    openrar a -cdc -md1m -rr5 -q <arc> <base> <u1> <v1>
    assert(res == 0)   // the ARCHIVE CREATION itself returned non-zero

Not a regression: the identical commit passed on the master push run, and the
tag run passed on re-run (attempt 2, all 12 jobs green). So it is
intermittent, roughly 1 in 2 for this job, and it has only ever been seen on
the Windows x64 leg.

Two things make it worth chasing rather than re-running:

1. It blocks releases. The `release-assets` job gates on the whole matrix, so
   a single flaky assert means no artifacts publish until someone re-runs the
   job by hand. That happened here and needed a manual `gh run rerun --failed`.
2. The failure is in archive CREATION with `-rr5` on `-cdc` input, not in the
   repair path the test is named for. The inputs include generated noise
   (`make_noise_bytes`) plus a partially-corrupted second file, so the RR
   record is being built over data where a volume/recovery computation may
   race or hit an edge case. Nothing in this release changed the recovery
   writer or the CDC planner, which is consistent with an old, unreached edge.

Next step: run `cli_tests` (or just that test) in a loop on windows-2022 x64
and capture the CLI's stderr and exit code when it fails - the test currently
swallows both into DEVNULL, so the actual cause is invisible. That is the
first thing to fix: make the test report the child's output on failure.