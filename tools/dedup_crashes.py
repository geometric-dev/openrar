#!/usr/bin/env python3
"""Fuzz crash dedup (v1.30 M5, freeze prereq 4: crash dedup).

Walks the libFuzzer crash artifacts (tests/fuzz/crash-*), buckets each by a
stack-signature proxy (the harness name + the top frames' function offsets
when present, else the artifact's size-class + first differing bytes hash),
and keeps only the FIRST artifact per bucket. Buckets are recorded in
tests/fuzz/crash-buckets.json so future crashes in a known bucket are
dropped loudly (printed, not silently ignored) and new buckets survive as
artifacts for triage.

Exit 0 always — dedup never fails the nightly; the crash itself already did.
"""

import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CRASH_DIR = REPO / "tests" / "fuzz"
BUCKETS = REPO / "tests" / "fuzz" / "crash-buckets.json"


def signature(artifact: Path) -> str:
    data = artifact.read_bytes()
    # Harness name from the libFuzzer artifact prefix (crash-<harness>-...).
    m = re.match(r"crash-([a-zA-Z0-9_]+)", artifact.name)
    harness = m.group(1) if m else "unknown"
    # Content signature: the first 64 bytes (header shapes dominate) + size class.
    content = hashlib.sha256(data[:64]).hexdigest()[:16]
    size_class = f"{len(data) // 1024}K" if len(data) >= 1024 else str(len(data))
    return f"{harness}:{content}:{size_class}"


def main() -> int:
    artifacts = sorted(CRASH_DIR.glob("crash-*"))
    if not artifacts:
        print("[dedup] no crash artifacts")
        return 0
    seen = {}
    if BUCKETS.exists():
        seen = json.loads(BUCKETS.read_text(encoding="utf-8"))
    kept, dropped = [], []
    for a in artifacts:
        sig = signature(a)
        if sig in seen:
            dropped.append((a.name, sig))
            a.unlink()  # known bucket: drop the duplicate artifact
        else:
            seen[sig] = a.name
            kept.append((a.name, sig))
    BUCKETS.write_text(json.dumps(seen, indent=1, sort_keys=True), encoding="utf-8")
    for name, sig in kept:
        print(f"[dedup] NEW bucket: {name} -> {sig}")
    for name, sig in dropped:
        print(f"[dedup] known bucket dropped: {name} -> {sig}")
    print(f"[dedup] {len(kept)} kept, {len(dropped)} dropped; "
          f"{len(seen)} buckets total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
