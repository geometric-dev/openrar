#!/usr/bin/env python3
"""JS error-code mirror parity check (docs/abi-freeze.md; v1.30.0 freeze, M1).

The WASM JS wrapper mirrors the C RarError enum twice:
  - wasm/js/openrar-archive.js   CODE_MAP: numeric code -> name
  - wasm/js/openrar-archive.d.ts RarErrorCode: name union type

Both must stay in exact parity with the canonical C enum in
include/openrar/openrar_dll.h. The v1.x WASM arc's signature/free defects
are the standing cautionary tale (docs/ROADMAP.md freeze prereq 3): a code
added to the C enum without mirroring, or mirrored with a wrong numeric
value, is exactly the drift class this check makes loud.

Parity rules asserted here:
  1. every negative C code has a CODE_MAP entry with the SAME name;
  2. CODE_MAP has no extra entries (the reserved -8/-10 slots stay
     unmirrored; a new C code -16 must land in the same change);
  3. the .d.ts union names == CODE_MAP names;
  4. RAR_OK (0) and RAR_ERR_PARTIAL_OK (1) are deliberately NOT mirrored —
     they are not thrown as errors (documented exclusion; asserting it
     keeps intent explicit).

Exit codes: 0 parity, 1 drift, 2 usage.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def parse_c_enum(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    body = re.search(r"enum RarError\s*\{(.*?)\};", text, re.S)
    if not body:
        print(f"FAIL: enum RarError not found in {path}", file=sys.stderr)
        sys.exit(1)
    codes = {}
    # Only "NAME = value," enumerator lines; comments (with the reserved-slot
    # notes) never match this shape.
    for m in re.finditer(r"(RAR_[A-Z0-9_]+)\s*=\s*(-?\d+)", body.group(1)):
        name, value = m.group(1), int(m.group(2))
        if name in codes and codes[name] != value:
            print(f"FAIL: duplicate C enumerator {name}", file=sys.stderr)
            sys.exit(1)
        codes[name] = value
    if len(codes) < 15:
        print(f"FAIL: parsed only {len(codes)} C codes — parser drift", file=sys.stderr)
        sys.exit(1)
    return codes


def parse_code_map(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    body = re.search(r"const CODE_MAP\s*=\s*Object\.freeze\(\{(.*?)\}\);", text, re.S)
    if not body:
        print(f"FAIL: CODE_MAP not found in {path}", file=sys.stderr)
        sys.exit(1)
    mapping = {}
    for m in re.finditer(r"\[(-?\d+)\]\s*:\s*'([A-Z0-9_]+)'", body.group(1)):
        mapping[int(m.group(1))] = m.group(2)
    return mapping


def parse_dts_union(path: Path) -> set:
    text = path.read_text(encoding="utf-8")
    body = re.search(r"export type RarErrorCode\s*=(.*?)                ;", text, re.S)
    if not body:
        # tolerate formatting drift: capture up to the first ';' at statement end
        body = re.search(r"export type RarErrorCode\s*=(.*?);", text, re.S)
    if not body:
        print(f"FAIL: RarErrorCode union not found in {path}", file=sys.stderr)
        sys.exit(1)
    return set(re.findall(r"'([A-Z0-9_]+)'", body.group(1)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--header", default=str(REPO / "include/openrar/openrar_dll.h"))
    ap.add_argument("--js", default=str(REPO / "wasm/js/openrar-archive.js"))
    ap.add_argument("--dts", default=str(REPO / "wasm/js/openrar-archive.d.ts"))
    args = ap.parse_args()

    c_codes = parse_c_enum(Path(args.header))
    code_map = parse_code_map(Path(args.js))
    dts_names = parse_dts_union(Path(args.dts))

    fails = []

    c_negative = {v: k for k, v in c_codes.items() if v < 0}

    def js_name(c_name: str) -> str:
        # The JS mirror strips the RAR_ERR_ prefix (RAR_ERR_NOT_RAR -> NOT_RAR).
        return c_name[len("RAR_ERR_"):] if c_name.startswith("RAR_ERR_") else c_name

    # 1+2. exact numeric parity for negative codes
    for value, name in sorted(c_negative.items()):
        got = code_map.get(value)
        if got != js_name(name):
            fails.append(f"C {name} ({value}) mirrored as {got!r} in CODE_MAP "
                         f"(expected {js_name(name)!r})")
    for value, name in code_map.items():
        c_match = c_negative.get(value)
        if c_match is None or js_name(c_match) != name:
            fails.append(f"CODE_MAP has {value} -> {name!r} with no matching C code")

    # 3. union names == CODE_MAP names
    map_names = set(code_map.values())
    if dts_names != map_names:
        fails.append(
            f"RarErrorCode union != CODE_MAP names: union-only={sorted(dts_names - map_names)} "
            f"map-only={sorted(map_names - dts_names)}"
        )

    # 4. documented exclusions
    for name in ("RAR_OK", "RAR_ERR_PARTIAL_OK"):
        value = c_codes.get(name)
        if value in code_map:
            fails.append(f"{name} ({value}) must not be mirrored into CODE_MAP")

    if fails:
        print("FAIL: JS error-code mirror drifted from the C enum (docs/abi-freeze.md)")
        for f in fails:
            print(f"  - {f}")
        return 1

    print(f"[js-mirror] parity OK: {len(c_negative)} negative codes mirrored; "
          f"{len(dts_names)} union names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
