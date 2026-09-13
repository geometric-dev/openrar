<!-- Title: conventional-commit subject, e.g. fix(format): reject non-minimal vint -->

## What & why

<!-- One logical change per PR. Root cause / rationale if a fix. -->

## Verification

<!-- Paste the footer you ran, e.g. "Gate: build + ctest + interop passed." -->

- [ ] Local gate green: `powershell -File tools/gate.ps1` (or POSIX equivalent)
- [ ] Relevant test layers run (see CONTRIBUTING.md "Testing" table)
- [ ] Commit message follows CONTRIBUTING.md (type(scope): subject, Gate: footer)
- [ ] Any finding IDs (H/M/L/I/Q/R) resolve to a tracked doc
- [ ] No scratch/build artifacts added

## Clean-room confirmation (format/compress/crypto changes)

- [ ] No UnRAR/WinRAR source consulted; work is spec-driven (`docs/spec/`)
