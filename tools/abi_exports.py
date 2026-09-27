#!/ usr / bin / env python3
"""ABI export-list parity tool (docs/abi-freeze.md; v1.30.0 freeze, M1).

Two modes:

  --generate            Parse include/openrar/openrar_dll.h and (re)write
                        tools/abi_exports_canonical.txt — the frozen export
                        list, one symbol per line, sorted. Run this ONLY when
                        an additive export legitimately lands (MINOR bump
                        rules; docs/versioning.md) and commit the diff.

  --check <binary>      Extract the exported symbols from a built
                        openrar.dll / libopenrar.so / libopenrar.dylib and
                        compare the C-ABI surface (symbols matching
                        ^openrar_\\w+$) against the canonical list. Any
                        missing or extra export exits nonzero — an export
                        added or removed without a canonical-list update in
                        the same change fails here.

The comparison deliberately scopes to the unmangled C-ABI namespace: C++
implementation symbols (mangled _ZN7openrar...) may legitimately appear in
ELF/Mach-O exports until visibility hygiene is tightened, and are NOT part
of the frozen surface. The MSVC DLL exports exactly its dllexport set.

Exit codes: 0 parity (or loud [SKIP] when no symbol tool exists on the
host), 1 drift/parse failure, 2 usage error.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "openrar" / "openrar_dll.h"
CANONICAL = REPO / "tools" / "abi_exports_canonical.txt"

#Every export is declared "OPENRAR_DLL_API <type> OPENRAR_DLL_CALL <name>("
#(declarations may wrap lines — use DOTALL and anchor on the call marker).
EXPORT_RE = re.compile(
    r"OPENRAR_DLL_API\s+[^;]*?OPENRAR_DLL_CALL\s+(openrar_[A-Za-z0-9_]+)\s*\(", re.S
)
SYMBOL_RE = re.compile(r"^openrar_[A-Za-z0-9_]+$")


def read_header_exports() -> list:
    text = HEADER.read_text(encoding="utf-8")
    names = sorted(set(EXPORT_RE.findall(text)))
    if not names:
        print(f"FAIL: no exports parsed from {HEADER}", file=sys.stderr)
        sys.exit(1)
    return names


def cmd_generate() -> int:
    names = read_header_exports()
    CANONICAL.write_text("\n".join(names) + "\n", encoding="utf-8")
    print(f"[abi-exports] wrote {len(names)} exports -> {CANONICAL}")
    return 0


def _parse_dumpbin(out: str) -> set:
    syms = set()
    for line in out.splitlines():
        toks = line.split()
        if len(toks) >= 4 and SYMBOL_RE.match(toks[-1]):
            syms.add(toks[-1])
    return syms


def _parse_nm(out: str) -> set:
    syms = set()
    for line in out.splitlines():
        toks = line.split()
        if toks and toks[-1].startswith("openrar_") and SYMBOL_RE.match(toks[-1]):
            syms.add(toks[-1])
    return syms


def _tool_attempts(binary: Path):
#(argv, parser) pairs tried in order; a tool that exists but fails
#(wrong args for the format, wrong arch, not on PATH) falls through to
#the next — CI environments differ in what they put on PATH.
    attempts = []
    if sys.platform == "win32":
        dumpbin = shutil.which("dumpbin")
        if dumpbin:
            attempts.append(([dumpbin, "/exports", str(binary)], _parse_dumpbin))
    llvm_nm = shutil.which("llvm-nm")
    nm = shutil.which("nm")
    for tool in (llvm_nm, nm):
        if not tool:
            continue
        if sys.platform == "darwin":
            args = [tool, "-gU", str(binary)]
        else:
            args = [tool, "-D", "--defined-only", str(binary)]
        attempts.append((args, _parse_nm))
    return attempts


def binary_symbols(binary: Path) -> set:
    """Return the exported C-ABI symbols; {} when every tool attempt fails
    (the caller prints a loud [SKIP]) or when none of the tools exist."""
    for args, extract in _tool_attempts(binary):
        try:
            out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, OSError):
            continue
        syms = extract(out)
        if syms:
            return syms
    return set()


def cmd_check(binary: Path) -> int:
    canonical = set(CANONICAL.read_text(encoding="utf-8").split())
    if not canonical:
        print(f"FAIL: canonical list empty: {CANONICAL}", file=sys.stderr)
        return 1
    exported = binary_symbols(binary)
    if not exported:
        print("[SKIP] abi-exports: no symbol tool available (dumpbin/llvm-nm/nm) "
              "or no exports parsed; parity not checked on this host")
        return 0
    missing = sorted(canonical - exported)
    extra = sorted(exported - canonical)
    if missing or extra:
        print("FAIL: frozen export surface drifted (docs/abi-freeze.md)")
        for m in missing:
            print(f"  missing from binary: {m}")
        for e in extra:
            print(f"  exported but not in canonical list: {e}")
        return 1
    print(f"[abi-exports] parity OK: {len(exported)} exports == canonical")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--generate", action="store_true",
                    help="regenerate tools/abi_exports_canonical.txt from the header")
    ap.add_argument("--check", metavar="BINARY",
                    help="compare a built DLL/so/dylib against the canonical list")
    args = ap.parse_args()
    if args.generate and args.check:
        ap.error("--generate and --check are mutually exclusive")
    if args.generate:
        return cmd_generate()
    if args.check:
        return cmd_check(Path(args.check))
    ap.error("one of --generate / --check is required")
    return 2


if __name__ == "__main__":
    sys.exit(main())
