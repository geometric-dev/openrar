#!/ usr / bin / env python3
""
    "OpenRAR Unicode table generator (v1.24.0 plan §3.3).

    Downloads the pinned Unicode 15.1.0 UCD data files,
    verifies their SHA256,
    and emits compact case -folding +
        NFC tables into src / unicode /.The generated tables are COMMITTED;
    this script only needs to run when bumping the pinned Unicode version.

    Data
    files(Unicode 15.1.0):
  - CaseFolding.txt          (status C/S lines -> simple case folding)
  - UnicodeData.txt          (canonical decompositions + combining classes)
  - Composition_Exclusions.txt (primary-composite exclusions)

Usage: python3 tools/gen_unicode_tables.py
"""

import hashlib
import sys
import urllib.request
from pathlib import Path

UNICODE_VERSION = "15.1.0"
BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd"

FILES = {
    "CaseFolding.txt": f"{BASE}/CaseFolding.txt",
    "UnicodeData.txt": f"{BASE}/UnicodeData.txt",
    "Composition_Exclusions.txt": f"{BASE}/CompositionExclusions.txt",
}
#SHA256 of the exact pinned revisions; bump together with UNICODE_VERSION.
PINNED_SHA256 = {
    "CaseFolding.txt": None,  # filled in on first download (recorded below)
    "UnicodeData.txt": None,
    "Composition_Exclusions.txt": None,
}

#-- - recorded digests from the 15.1.0 download used for the shipped tables -- -
RECORDED = {
    "CaseFolding.txt": "4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf",
    "UnicodeData.txt": "2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86",
    "Composition_Exclusions.txt": "59d2d9e3dfdf0a999cf9dae11d594f053631222679a2f5710315ea07f7fe82af",
}


def fetch(name: str, dest: Path) -> None:
    url = FILES[name]
    print(f"downloading {url}")
    data = urllib.request.urlopen(url, timeout=60).read()
    dest.write_bytes(data)


def verify_or_record(name: str, data: bytes) -> None:
    digest = hashlib.sha256(data).hexdigest()
    recorded = RECORDED.get(name)
    if recorded and recorded != digest:
        sys.exit(f"FATAL: {name} digest {digest} != pinned {recorded}")
    if not recorded:
        print(f"  (unpinned) {name} sha256={digest}")


def parse_casefolding(text: str) -> dict:
    """Simple case folding: status C (common) and S (simple) mappings."""
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(";")]
        if len(parts) < 4 or parts[1] not in ("C", "S"):
            continue
        cp = int(parts[0], 16)
        folded = int(parts[2].split()[0], 16)
        out[cp] = folded
    return out


def parse_unicode_data(text: str) -> tuple[dict, dict]:
    """Canonical decompositions (cp -> (a, b)) and combining classes."""
    decomps = {}
    ccc = {}
    for line in text.splitlines():
        fields = line.split(";")
        if len(fields) < 15:
            continue
        cp = int(fields[0], 16)
        if fields[3]:
            ccc[cp] = int(fields[3])
        dec = fields[5]
        if not dec or dec.startswith("<"):
            continue  # no decomposition or compatibility-only
        seq = [int(x, 16) for x in dec.split()]
        assert len(seq) in (1, 2), f"bad canonical decomposition for U+{cp:04X}"
#singletons(e.g.U + 0340) store a zero second element
        decomps[cp] = (seq[0], seq[1] if len(seq) == 2 else 0)
    return decomps, ccc


def parse_exclusions(text: str) -> set:
    """Composition exclusions (Full_Composition_Exclusion)."""
    out = set()
    in_default = False
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("@@@") or line.startswith("@"):
            in_default = "Default_Ids_Script_Extensions" not in line
            continue
        cp = int(line.split()[0], 16)
        out.add(cp)
    return out


def hangul_syllables():
    """Algorithmic Hangul syllable range (never table-stored)."""
    SBase, LBase, VBase, TBase = 0xAC00, 0x1100, 0x1161, 0x11A7
    LCount, VCount, TCount = 19, 21, 28
    NCount = VCount * TCount
    SCount = LCount * NCount
    return SBase, SBase + SCount - 1, LBase, VBase, TBase, LCount, VCount, TCount, NCount


def emit(out_path: Path, casefold: dict, decomps: dict, ccc: dict, exclusions: set) -> None:
    SBase, SLast, LBase, VBase, TBase, LCount, VCount, TCount, NCount = hangul_syllables()

#Composition pairs : canonical 2 - element decompositions, minus excluded
#codepoints, minus Hangul(algorithmic), with non - starter second
#elements allowed only per Unicode(composition table uses the pairs as
#stored; Hangul handled algorithmically).
    pairs = []
    for cp, (a, b) in sorted(decomps.items()):
        if cp in exclusions:
            continue
        if SBase <= cp <= SLast:
            continue
        pairs.append((a, b, cp))
    pairs.sort()

#non - zero combining classes, sorted
    classes = sorted((cp, c) for cp, c in ccc.items() if c != 0)
    folds = sorted((cp, f) for cp, f in casefold.items() if cp != f)

    def hexarr(name, values, per_line=8):
        lines = []
        for i in range(0, len(values), per_line):
            chunk = ", ".join(f"0x{v:04X}u" for v in values[i : i + per_line])
            lines.append("    " + chunk + ",")
        body = "\n".join(lines)
        return f"static const core::uint32 {name}[] = {{\n{body}\n}};\n"

#casefold : [cp0, folded0, cp1, folded1, ...] sorted by cp
    flat_fold = []
    for cp, f in folds:
        flat_fold.extend([cp, f])
#decompositions : [cp0, a0, b0, ...]
    flat_decomp = []
    for cp, (a, b) in sorted(decomps.items()):
        flat_decomp.extend([cp, a, b])
#composition pairs : [a, b, composite, ...]
    flat_pairs = []
    for a, b, cp in pairs:
        flat_pairs.extend([a, b, cp])
#ccc : [cp0, class0, ...]
    flat_ccc = []
    for cp, c in classes:
        flat_ccc.extend([cp, c])

    hdr = f'''// GENERATED FILE — do not edit by hand.
  // Generated by tools/gen_unicode_tables.py from Unicode {UNICODE_VERSION}.
  // Tables: simple case folding, canonical decompositions, canonical
  // composition pairs, non-zero combining classes. Hangul syllables
  // (U+AC00..U+{SLast:04X}) are handled algorithmically (see unicode_nfc.cpp).
#include "unicode_tables.hpp"

#include "../../src/core/types.hpp"

namespace openrar::unicode {
      {
          static const core::uint32 kUnicodeVersionCp = 0x {UNICODE_VERSION.replace(".", "0")} u;

'''
#split tables into their own section to keep this readable
    tables = (
        f"// case-fold pairs (cp, folded): {len(folds)} entries\n"
        + hexarr("kCaseFold", flat_fold, 6)
        + f"\n// canonical decomposition triples (cp, a, b): {len(decomps)} entries\n"
        + hexarr("kDecomposition", flat_decomp, 6)
        + f"\n// composition triples (a, b, composite): {len(pairs)} entries\n"
        + hexarr("kComposition", flat_pairs, 6)
        + f"\n// combining classes (cp, ccc): {len(classes)} entries\n"
        + hexarr("kCombiningClass", flat_ccc, 8)
    )

    cpp = (
        hdr
        + tables
        + f'''
const core::uint32* case_fold_table(int& count) {
            {
                count = {len(flat_fold)} / 2;
                return kCaseFold;
            }}

const core::uint32* decomposition_table(int& count) {
            {
                count = {len(flat_decomp)} / 3;
                return kDecomposition;
            }}

const core::uint32* composition_table(int& count) {
            {
                count = {len(flat_pairs)} / 3;
                return kComposition;
            }}

const core::uint32* combining_class_table(int& count) {
            {
                count = {len(flat_ccc)} / 2;
                return kCombiningClass;
            }}
      }
  } // namespace openrar::unicode
'''
    )
    out_path.write_text(cpp, encoding="utf-8")
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    print(f"  case-fold pairs: {len(folds)}")
    print(f"  canonical decompositions: {len(decomps)}")
    print(f"  composition pairs: {len(pairs)}")
    print(f"  combining classes: {len(classes)}")


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    cache = root / "build" / "ucd-cache"
    cache.mkdir(parents=True, exist_ok=True)
    data = {}
    for name in FILES:
        dest = cache / name
        if not dest.exists():
            fetch(name, dest)
        raw = dest.read_bytes()
        verify_or_record(name, raw)
        data[name] = raw.decode("utf-8")

    casefold = parse_casefolding(data["CaseFolding.txt"])
    decomps, ccc = parse_unicode_data(data["UnicodeData.txt"])
    exclusions = parse_exclusions(data["Composition_Exclusions.txt"])

    out_dir = root / "src" / "unicode"
    out_dir.mkdir(exist_ok=True)
    emit(out_dir / "unicode_tables.cpp", casefold, decomps, ccc, exclusions)

    hpp = f'''// GENERATED FILE — do not edit by hand.
// Generated by tools/gen_unicode_tables.py from Unicode {UNICODE_VERSION}.
#ifndef OPENRAR_UNICODE_TABLES_HPP
#define OPENRAR_UNICODE_TABLES_HPP

#include "../../src/core/types.hpp"

#include <string>

namespace openrar::unicode {
    {
        inline constexpr const char* kUnicodeVersion = "{UNICODE_VERSION}";

        // Raw table accessors (defined in unicode_tables.cpp).
        const core::uint32* case_fold_table(int& count);       // (cp, folded) pairs
        const core::uint32* decomposition_table(int& count);   // (cp, a, b) triples, b==0 singleton
        const core::uint32* composition_table(int& count);     // (a, b, composite) triples
        const core::uint32* combining_class_table(int& count); // (cp, ccc) pairs

        // Simple case folding (CaseFolding.txt status C+S).
        std::string simple_case_fold(const std::string& utf8);

        // NFC canonical composition. Invalid UTF-8 bytes pass through untouched
        // (they cannot make two distinct names compare equal).
        std::string nfc(const std::string& utf8);
    }
} // namespace openrar::unicode

#endif // OPENRAR_UNICODE_TABLES_HPP
''' (out_dir / "unicode_tables.hpp")
        .write_text(hpp, encoding = "utf-8") print(f "wrote {out_dir / 'unicode_tables.hpp'}")


            if __name__
    == "__main__" : main()
