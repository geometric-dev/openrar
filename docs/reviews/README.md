# OpenRAR Architecture Audits, Reviews & ADRs

This directory archives historical architectural reviews, codebase audits, post-mortems, and Architecture Decision Record (ADR) logs for OpenRAR.

For active project roadmaps and living specifications, refer back to the [OpenRAR Documentation Hub](../README.md).

---

## 📑 Contents

### 1. Phase 2 Architectural Code Audit
The Phase 2 audit series evaluated the entire OpenRAR codebase across four systematic deep-dive rounds:

- **[Phase 2 Master Review Plan](review-master-plan.md):** The scoping charter, methodologies, verification requirements, and triage criteria for the comprehensive Phase 2 codebase audit.
- **[Phase 2.1 Audit Report](review-phase2.1.md):** Deep-dive review of core parsing, archive reading, and sliding-window compression pipelines.
- **[Phase 2.2 Audit Report](review-phase2.2.md):** Review of multi-volume handling, memory-mapped I/O, and encryption primitives.
- **[Phase 2.3 Audit Report](review-phase2.3.md):** Review of Reed-Solomon recovery volumes, Cauchy erasure correction, and SFX script handling.
- **[Phase 2.4 Audit Report](review-phase2.4.md):** Review of foreign format transcoding (ZIP, TAR, GZIP), sandboxed worker isolation, and C ABI surfaces.

### 2. Comprehensive Governance & Monthly Audits
- **[October 2026 Codebase & Governance Audit](monthly-review-202610.md):** Repository-wide review evaluating implementation status, test coverage, static analysis, sanitizer health, and Roadmap adherence.

### 3. Architectural Decision Records (ADRs)
- **[Architectural Question Log](question-log.md):** Systematic log of key architectural design decisions, trade-offs, and resolutions adopted throughout OpenRAR development.

### 4. Code Quality & Static Analysis
- **[November 2026 Code Quality Review](code-quality-review-202611.md):** Whole-tree review with a fresh Clang-Tidy extended-suite run (209 diagnostics triaged, none actionable), MSVC `/analyze` C6262/arithmetic triage, and a P0–P4 prioritised remediation list with fix outlines.
- **[Architect Challenge — Code Quality Review 2026-11](code-quality-review-202611-architect-challenge.md):** Adversarial principal-architect challenge of the remediation plan; three fix outlines revised (assert-only narrowing, line-keyed baseline, shared scratch buffer) and two missing verification gates identified.
- **[Architect Walkthrough — Code Quality Review 2026-11](code-quality-review-202611-architect-walkthrough.md):** Post-implementation sign-off for the remediation (header-parser fuzz harness, MSVC ASan leg, 144-site widening sweep, three re-enabled checks). Verdict: 🟢 sign-off — two defects found and fixed during the walkthrough (fuzz campaigns were coverage-blind at 65 counters; the new nightly step discarded its corpus), plus two more that only CI could see (the leg pointed at `windows-latest`, which no longer has the 2022 generator; a Windows-only gate run cannot see Linux-only code). R-1 (run the ASan leg) and R-2 (the `allocation_tests` split) are closed with receipts in the walkthrough, and R-6/R-7/R-8 close the fuzzer's OOM input and the three legs that were red (TSan, linux-arm64, and a ratio guard measured against the wrong corpus — none of them a code regression). CI run 43504475946 is green on every leg.

### 5. Technical Investigations & Post-Mortems
- **[CLI Test Timing Flake Post-Mortem](followup-cli-tests-flake.md):** Root-cause analysis and resolution for sporadic CI test timeouts under Windows hosted runners.
- **[Optimal Parse Descoping Analysis](optimal-parse-descoped-analysis.md):** Mathematical and empirical justification for parking the v1.32 full Dynamic Programming optimal parser (resolving Anti-Finding A-4).
- **[Optimal Parse Descoping Plan](optimal-parse-descoped-plan.md):** Action plan for clean removal and replacement with bounded greedy match finding.
