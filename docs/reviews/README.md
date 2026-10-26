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

### 4. Technical Investigations & Post-Mortems
- **[CLI Test Timing Flake Post-Mortem](followup-cli-tests-flake.md):** Root-cause analysis and resolution for sporadic CI test timeouts under Windows hosted runners.
- **[Optimal Parse Descoping Analysis](optimal-parse-descoped-analysis.md):** Mathematical and empirical justification for parking the v1.32 full Dynamic Programming optimal parser (resolving Anti-Finding A-4).
- **[Optimal Parse Descoping Plan](optimal-parse-descoped-plan.md):** Action plan for clean removal and replacement with bounded greedy match finding.
