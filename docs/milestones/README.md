# OpenRAR Milestone Engineering Archive

This directory preserves the historical engineering artifacts—pre-analyses, architectural design reviews, implementation plans, probe logs, and audit walkthroughs—generated during each milestone of OpenRAR.

For active project roadmaps and living specifications, refer back to the [OpenRAR Documentation Hub](../README.md) and the [Master Roadmap](../ROADMAP.md).

---

## 📅 Milestone Engineering Catalog

| Release | Focus / Milestone Title | Historical Artifacts |
|---|---|---|
| **v1.23** | SFX Directive Engine & Process Security | [Design Review](v1.23-sfx-design-review.md) · [Implementation Plan](v1.23-sfx-implementation-plan.md) |
| **v1.24** | Atomic Extraction & Syscall Path Containment | [Implementation Plan](v1.24-implementation-plan.md) |
| **v1.25** | Memory-Mapped Read Engine | [Design Review](v1.25-mmap-design-review.md) · [Implementation Plan](v1.25-implementation-plan.md) · [Walkthrough](v1.25-mmap-walkthrough.md) |
| **v1.26** | Content-Defined Chunking (CDC) Solid Ordering | [Pre-Analysis](v1.26-pre-analysis.md) · [Implementation Plan](v1.26-implementation-plan.md) |
| **v1.27** | POSIX xattrs & Mark of the Web (Zone.Identifier) | [Pre-Analysis](v1.27-pre-analysis.md) · [Implementation Plan](v1.27-implementation-plan.md) |
| **v1.28** | Interactive Dual-Progress TUI & Bench Engine | [Pre-Analysis](v1.28-pre-analysis.md) · [Implementation Plan](v1.28-implementation-plan.md) |
| **v1.29** | Format Migration Engine (`cv`: ZIP/TAR/GZIP -> RAR5) | [Pre-Analysis](v1.29-pre-analysis.md) · [Implementation Plan](v1.29-implementation-plan.md) |
| **v1.30** | Sandboxed Worker Process & Frozen C ABI | [Pre-Analysis](v1.30-pre-analysis.md) · [Implementation Plan](v1.30-implementation-plan.md) |
| **v1.31** | Token Code 257 & Slot Lifecycle | [Pre-Analysis](v1.31-pre-analysis.md) · [Implementation Plan](v1.31-implementation-plan.md) |
| **v1.32** | Optimal Parse Investigation (Parked) | [Pre-Analysis](v1.32-pre-analysis.md) · [Implementation Plan](v1.32-implementation-plan.md) |
| **v1.33** | Hash Chain Pruning Analysis | [Pre-Analysis](v1.33-pre-analysis.md) |
| **v1.34** | Measurement Protocol & Determinism Verification | [Pre-Analysis](v1.34.0-pre-analysis.md) · [Implementation Plan](v1.34.0-implementation-plan.md) |
| **v1.35** | Reed-Solomon Multi-Physical Recovery Engine | [Pre-Analysis](v1.35.0-pre-analysis.md) · [Implementation Plan](v1.35.0-implementation-plan.md) · [RR Probe](v1.35.0-rr-probe.md) |
| **v1.36** | Transcoder Symlink / Hardlink Security | [Pre-Analysis](v1.36.0-pre-analysis.md) |
| **v1.37** | Two-Phase Parallel Streaming Decompressor | [Pre-Analysis](v1.37.0-pre-analysis.md) · [Design Wall](v1.37.0-design-wall.md) · [Plan](v1.37.0-implementation-plan.md) · [2-Phase Pre-Analysis](v1.37.0-two-phase-pre-analysis.md) · [2-Phase Plan](v1.37.0-two-phase-implementation-plan.md) |
| **v1.38** | Sliding Window Read Buffer Optimization | [Pre-Analysis](v1.38.0-pre-analysis.md) · [Implementation Plan](v1.38.0-implementation-plan.md) |
| **v1.39** | Extraction Durability & Write Pipelines | [Pre-Analysis](v1.39.0-pre-analysis.md) · [Remediation Plan](v1.39.1-remediation-plan.md) |
| **v1.40** | Corpus Benchmark Suite & Hardware Baselines | [Benchmark Bundle](v1.40.0-bench-bundle.md) |
| **v1.41** | Security Review & Fuzzing Expansion | [Code Review v1.41.1](v1.41.1-code-review.md) |
| **v1.42** | Classic Arc Stabilization & Release Tag | [Release Notes](v1.42.0-release-notes.md) |
| **v1.43** | 5-Axis Ratio Attribution & Census Profiling | [Pre-Analysis](v1.43.0-pre-analysis.md) |
| **v1.44** | Verification Substrate & Modular CMake Architecture | [Substrate Plan](v1.44-verification-substrate-plan.md) |
| **v1.46** | Dynamic Window Clamping (`-md=auto`) & OPT-D01 | [Pre-Analysis](v1.46.0-pre-analysis.md) · [Design Review](v1.46.0-design-review.md) |
| **v1.47** | Multi-Sanitizer Hardening & macOS Seatbelt Sandbox | [Pre-Analysis](v1.47.0-pre-analysis.md) · [Design Review](v1.47.0-design-review.md) · [Plan](v1.47.0-implementation-plan.md) · [Walkthrough](v1.47.0-walkthrough.md) |
| **v1.48** | Transcoder GZIP Concatenation & In-Memory Pipeline | [Pre-Analysis](v1.48.0-pre-analysis.md) · [Implementation Plan](v1.48.0-implementation-plan.md) |
| **v1.49** | WebAssembly Streaming Archive Pipeline | [Pre-Analysis](v1.49.0-pre-analysis.md) · [Implementation Plan](v1.49.0-implementation-plan.md) · [Walkthrough](v1.49.0-walkthrough.md) |
| **v1.50** | Parallel CDC Candidate-then-Stitch Deduplication | [Pre-Analysis](v1.50.0-pre-analysis.md) · [Design Review](v1.50.0-design-review.md) · [Plan](v1.50.0-implementation-plan.md) · [Walkthrough](v1.50.0-walkthrough.md) |
| **v1.51** | Multi-Volume Sandboxing & Encrypted Parsing Scope | [Pre-Analysis](v1.51.0-pre-analysis.md) · [Design Review](v1.51.0-design-review.md) · [Plan](v1.51.0-implementation-plan.md) · [Walkthrough](v1.51.0-walkthrough.md) |
| **v1.52** | Competitive Multi-Engine Audit & v2.0 Charter | Delivered in [PERFORMANCE.md](../../PERFORMANCE.md) and [v2.0-charter.md](../v2.0-charter.md) |
| **v1.56** | Cost-Aware Forward-Arrival DAG Parser & Symbol Arbiter | [Implementation Plan](v1.56.0-implementation-plan.md) |
| **v1.57** | Forward-Arrival Throughput Optimization & Fast Literal Bypass | [Implementation Plan](v1.57.0-implementation-plan.md) · [Walkthrough](v1.57.0-walkthrough.md) |
| **v1.58** | Modern SIMD Vector Pipelines & Hardware-Accelerated BLAKE2sp | [Implementation Plan](v1.58.0-implementation-plan.md) · [Walkthrough](v1.58.0-walkthrough.md) |
| **v1.59** | Distance-Costed Hash-Chain Pruning & Rep-Code Horizon Expansion | [Implementation Plan](v1.59.0-implementation-plan.md) · [Walkthrough](v1.59.0-walkthrough.md) |
| **v1.60** | Parallel CDC Multi-Core Fingerprinting & Chunking Engine | [Implementation Plan](v1.60.0-implementation-plan.md) · [Walkthrough](v1.60.0-walkthrough.md) |
