# OpenRAR Documentation Hub

Welcome to the **OpenRAR Documentation Hub**. This directory contains the complete technical architecture, wire format specifications, formal security models, integration guides, and historical engineering ledgers for the OpenRAR project.

---

## 🧭 Navigation Guide: Where Do I Start?

Depending on your objective, follow one of the curated reading tracks below:

| Persona / Objective | Recommended Starting Point | Key Documents |
|---|---|---|
| **Casual Reader / Project Explorer** | High-level architecture & future vision | [v2.0 Architecture Charter](v2.0-charter.md) · [Invariants](invariants.md) · [Master Roadmap](ROADMAP.md) |
| **Format & Codec Engineer** | Wire-level RAR 5.0 layout & compression | [RAR 5.0 Format Specification](spec/00-overview.md) · [Invariants](invariants.md) · [Streaming Architecture](streaming-considerations.md) |
| **Application Developer (C / C++ / C# / Python)** | Native DLL and multi-language bindings | [Frozen C ABI Specification](abi-freeze.md) · [DLL Integration Guide](dll-integration-spec.md) · [DLL Enhancement Plan](dll-enhancement-plan.md) |
| **Web & JavaScript Developer** | Browser and Node.js WebAssembly runtime | [WASM Archive Specification](wasm-archive-spec.md) · [WASM Examples](wasm-examples.md) · [WASM Limitations](wasm-limitations.md) |
| **Security Auditor & Sysadmin** | Sandboxing, attack surface & containment | [Security Architecture](SECURITY_ARCHITECTURE.md) · [Extraction Contract](EXTRACTION_CONTRACT.md) · [SBOM](SBOM.md) |
| **OpenRAR Contributor** | Verification gates, rules & quality substrate | [Verification Legs](verification-legs.md) · [Versioning Policy](versioning.md) · [Verification Ledger](v1.44-verification-ledger.md) |

---

## 🏛️ 1. Living Architectural Specifications & Normative Contracts

These documents define the active, living laws of the OpenRAR codebase. Every commit and release must satisfy these contracts.

| Specification | Scope & Purpose | Status |
|---|---|---|
| **[Invariants](invariants.md)** | Non-negotiable system invariants: chain semantics (G-1), RAM ceiling (G-2), zero-alloc decode (G-3), byte identity (G-4), and error taxonomy. | 🔒 Frozen |
| **[Security Architecture](SECURITY_ARCHITECTURE.md)** | Sandboxed worker architecture (Windows AppContainer, Linux seccomp-BPF), syscall brokers, capability models, and threat analysis. | 🛡️ Active |
| **[Extraction Contract](EXTRACTION_CONTRACT.md)** | Atomic extraction guarantees, journal manifests, directory traversal containment, rollback behavior, and symlink security. | 🛡️ Active |
| **[Frozen C ABI](abi-freeze.md)** | Machine-checked C ABI layout, struct offsets, export tables, and additive-only binary compatibility rules. | 🔒 Frozen (v1.30) |
| **[Versioning Policy](versioning.md)** | Semantic Versioning rules, ABI event criteria, and breaking-change policies. | 📜 Active |
| **[Verification Legs](verification-legs.md)** | 24-stage CI verification matrix, sanitizer taxonomy (ASan, UBSan, TSan, MSan), and fuzzing gates. | 🧪 Active |
| **[Verification Ledger](v1.44-verification-ledger.md)** | Machine-checked assert ledger and test count reconciliation baseline. | 🧪 Active |
| **[Format Migration Model (FMM)](FMM.md)** | Clean-room transcode specifications from ZIP, TAR, and GZIP into RAR 5.0 streams. | 📦 Active |
| **[Software Bill of Materials (SBOM)](SBOM.md)** | CycloneDX software inventory, cryptosec attestations, and supply chain provenance. | 📋 Active |

---

## 📦 2. RAR 5.0 Wire Format Specification Suite (`spec/`)

OpenRAR maintains a comprehensive, clean-room technical specification of the RAR 5.0 container and codec format:

| Chapter | Title | Topic Covered |
|---|---|---|
| **[00](spec/00-overview.md)** | [Format Overview](spec/00-overview.md) | Header framing, variable-length integers (VINT), CRC32, encryption flags |
| **[01](spec/01-headers.md)** | [Header Types](spec/01-headers.md) | Main, File, Service (`CMT`, `QO`, `STM`), and End-of-Archive blocks |
| **[02](spec/02-compression-store.md)** | [Method 0: Store](spec/02-compression-store.md) | Passthrough uncompressed framing and byte-stream integrity |
| **[03](spec/03-compression-m1-m5.md)** | [Methods 1–5: LZ & Huffman](spec/03-compression-m1-m5.md) | Sliding window LZ77, prefix trees, circular distance buffers, slot tables |
| **[04](spec/04-filters.md)** | [Pre/Post-Processing Filters](spec/04-filters.md) | DELTA byte transform, E8/E9 x86 jump call translation, ARM filters |
| **[05](spec/05-recovery.md)** | [Recovery Records](spec/05-recovery.md) | Cauchy Reed-Solomon GF(2¹⁶) parity, shard layouts, multi-volume `.rev` repair |
| **[06](spec/06-encoding.md)** | [String & Metadata Encoding](spec/06-encoding.md) | UTF-8 path normalization, high-precision FILETIME/UNIX times, NTFS ACLs |
| **[07](spec/07-services.md)** | [Service Headers](spec/07-services.md) | Quick-Open (`QO`) header caching, locator records, archive comments |
| **[08](spec/08-encryption.md)** | [Cryptographic Security](spec/08-encryption.md) | AES-256 CBC, PBKDF2 HMAC-SHA256 key derivation, header encryption (`-hp`) |
| **[09](spec/09-multivolume.md)** | [Multi-Volume Archives](spec/09-multivolume.md) | Split slice naming (`.partNN.rar`), cross-volume entry spanning, verification |
| **[10](spec/10-sfx.md)** | [Self-Extracting Archives](spec/10-sfx.md) | Native Windows PE stub integration, SFX config scripting, sandboxed execution |
| **[11](spec/11-foreign-formats.md)** | [Foreign Container Transcoding](spec/11-foreign-formats.md) | Clean-room ZIP, TAR (ustar/pax), and GZIP migration into RAR 5.0 streams |

---

## 🔌 3. Subsystem Technical Guides & Bindings

Guides for integrating OpenRAR into applications, web environments, and native platforms:

- **[C DLL Integration Guide](dll-integration-spec.md):** Native C API reference, error-code semantics, memory ownership contracts, and threading rules.
- **[C DLL Enhancement Plan](dll-enhancement-plan.md):** Multi-threaded archive creation, async callbacks, and extraction streaming.
- **[WASM Archive Specification](wasm-archive-spec.md):** In-memory archive creation, inspection, and extraction inside browser and Node.js runtimes.
- **[WASM Code Examples](wasm-examples.md):** Idiomatic TypeScript/JavaScript code recipes for Web Workers, streams, and single-file extracts.
- **[WASM Deployment Guide](wasm-deployment.md):** Packaging, NPM distribution, CDN hosting, and cross-origin isolation (COOP/COEP).
- **[WASM Technical Limitations](wasm-limitations.md):** Browser memory ceilings (2 GiB Emscripten cap), single-thread fallback behavior, and performance profiles.
- **[Streaming Decode Considerations](streaming-considerations.md):** Architecture of zero-allocation pipe streaming, circular window decoupling, and two-phase parallel decompression.

---

## 🎯 4. Project Strategy & Governance

- **[Master Roadmap (`ROADMAP.md`)](ROADMAP.md):** The authoritative project roadmap detailing the completed Classic Arc (v1.0–v1.42), the completed Modern Arc (v1.43–v1.52), and active governance scorecards.
- **[OpenRAR v2.0 Architecture Charter](v2.0-charter.md):** Executive strategy and architectural roadmap for v2.0 (closing the ratio gap via cost-aware DAG parsing, AVX-512/NEON SIMD kernels, and zero-allocation streaming).

---

## 📁 5. Directory Structure & Archives

The `docs/` tree is partitioned into focused, logically separated domains:

```text
docs/
├── README.md                     # This document (Master Documentation Hub)
├── ROADMAP.md                    # Canonical Master Roadmap & Milestone Scorecard
├── v2.0-charter.md               # OpenRAR v2.0 Architecture Strategy & Charter
│
├── [Living Specifications]
│   ├── EXTRACTION_CONTRACT.md    # Atomic extraction, containment & rollback guarantees
│   ├── SECURITY_ARCHITECTURE.md  # Sandbox worker architecture & capability containment
│   ├── abi-freeze.md             # Frozen C ABI specification & binary compatibility rules
│   ├── invariants.md             # Core system invariants (G-1..G-8, I-1..I-6)
│   ├── versioning.md             # SemVer policy, ABI stability, and breaking rules
│   ├── verification-legs.md      # 24-stage CI verification matrix & sanitizer rules
│   ├── v1.44-verification-ledger.md # Machine-checked assert ledger
│   ├── FMM.md                    # Format Migration Model (ZIP/TAR/GZIP -> RAR5)
│   └── SBOM.md                   # Software Bill of Materials & security attestations
│
├── [Subsystem & Binding Specs]
│   ├── dll-integration-spec.md   # C DLL API specification & struct layouts
│   ├── dll-enhancement-plan.md   # C DLL features, streaming, and progress callbacks
│   ├── wasm-archive-spec.md      # WebAssembly in-memory archive API contract
│   ├── wasm-deployment.md        # WASM NPM distribution & web worker hosting
│   ├── wasm-examples.md          # Browser & Node.js code recipes
│   ├── wasm-limitations.md       # Browser memory ceilings & threading models
│   └── streaming-considerations.md # Streaming decode pipeline architecture
│
├── spec/                         # 12-Chapter RAR 5.0 Format Specification Suite
│   ├── 00-overview.md            # Framing, VINT, CRC, and encryption flags
│   ├── ...                       # Headers, compression, filters, recovery, services
│   └── 11-foreign-formats.md     # ZIP, TAR, and GZIP clean-room migration
│
├── milestones/                   # Chronological Milestone Engineering Archive (v1.23–v1.51)
│   ├── README.md                 # Milestone catalog & sprint index
│   ├── v1.23..v1.51 plans        # Pre-analyses, design reviews, implementation plans
│   └── v1.23..v1.51 walkthroughs # Completed milestone walkthroughs & verification records
│
└── reviews/                      # Architecture Audits & Governance Logs
    ├── README.md                 # Audits & ADR catalog
    ├── review-phase2.1..2.4.md   # Phase 2 4-part architectural code audit
    ├── monthly-review-202610.md  # October 2026 comprehensive governance review
    ├── question-log.md           # Architecture Decision Records (ADRs)
    └── followup-cli-tests-flake.md # Windows CI timing flake post-mortem
```
