#!/usr/bin/env python3
"""tools/bump_version.py
Single-source atomic version bumper and synchronizer for OpenRAR.

Updates all version locations across the codebase in lockstep:
  1. CMakeLists.txt (project(openrar VERSION X.Y.Z ...))
  2. wasm/js/package.json ("version": "X.Y.Z")
  3. bindings/python/pyproject.toml (version = "X.Y.Z")
  4. bindings/python/openrar/__init__.py (__version__ = "X.Y.Z")
  5. bindings/csharp/OpenRAR/OpenRAR.NET.csproj (<Version>X.Y.Z</Version>)
  6. debian/changelog (prepends Debian changelog entry if version differs)
  7. debian/openrar.1 & docs/man/openrar.1 (.TH OPENRAR 1 ... "OpenRAR X.Y.Z")

Usage:
  python tools/bump_version.py --check
  python tools/bump_version.py 1.54.0 [--dry-run] [--no-lint]
"""

import argparse
import email.utils
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def read_text_exact(path: Path) -> str:
    """Read without newline translation.

    Path.read_text() applies universal-newline decoding and Path.write_text()
    re-encodes with os.linesep, so on Windows a bump silently rewrote every
    file with CRLF and dirtied the whole tree for a no-op version change.
    """
    with path.open("r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text_exact(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(content)


def dominant_eol(content: str) -> str:
    return "\r\n" if "\r\n" in content else "\n"


def read_versions(root: Path) -> dict[str, str | None]:
    versions: dict[str, str | None] = {}

    # 1. CMakeLists.txt
    cmake_path = root / "CMakeLists.txt"
    if cmake_path.exists():
        content = cmake_path.read_text(encoding="utf-8")
        m = re.search(r"project\(openrar\s+VERSION\s+(\d+\.\d+\.\d+)", content)
        versions["CMakeLists.txt"] = m.group(1) if m else None
    else:
        versions["CMakeLists.txt"] = None

    # 2. wasm/js/package.json
    pkg_path = root / "wasm" / "js" / "package.json"
    if pkg_path.exists():
        content = pkg_path.read_text(encoding="utf-8")
        m = re.search(r'"version":\s*"([^"]+)"', content)
        versions["wasm/js/package.json"] = m.group(1) if m else None
    else:
        versions["wasm/js/package.json"] = None

    # 3. bindings/python/pyproject.toml
    pyproject_path = root / "bindings" / "python" / "pyproject.toml"
    if pyproject_path.exists():
        content = pyproject_path.read_text(encoding="utf-8")
        m = re.search(r'(?m)^version\s*=\s*"([^"]+)"', content)
        versions["bindings/python/pyproject.toml"] = m.group(1) if m else None
    else:
        versions["bindings/python/pyproject.toml"] = None

    # 4. bindings/python/openrar/__init__.py
    pyinit_path = root / "bindings" / "python" / "openrar" / "__init__.py"
    if pyinit_path.exists():
        content = pyinit_path.read_text(encoding="utf-8")
        m = re.search(r'(?m)^__version__\s*=\s*"([^"]+)"', content)
        versions["bindings/python/openrar/__init__.py"] = m.group(1) if m else None
    else:
        versions["bindings/python/openrar/__init__.py"] = None

    # 5. bindings/csharp/OpenRAR/OpenRAR.NET.csproj
    csproj_path = root / "bindings" / "csharp" / "OpenRAR" / "OpenRAR.NET.csproj"
    if csproj_path.exists():
        content = csproj_path.read_text(encoding="utf-8")
        m = re.search(r"<Version>([^<]+)</Version>", content)
        versions["bindings/csharp/OpenRAR/OpenRAR.NET.csproj"] = m.group(1) if m else None
    else:
        versions["bindings/csharp/OpenRAR/OpenRAR.NET.csproj"] = None

    # 6. debian/changelog
    changelog_path = root / "debian" / "changelog"
    if changelog_path.exists():
        content = changelog_path.read_text(encoding="utf-8")
        first_line = content.splitlines()[0] if content else ""
        m = re.search(r"openrar\s+\((\d+\.\d+\.\d+)", first_line)
        versions["debian/changelog"] = m.group(1) if m else None
    else:
        versions["debian/changelog"] = None

    # 7. debian/openrar.1
    man1_path = root / "debian" / "openrar.1"
    if man1_path.exists():
        content = man1_path.read_text(encoding="utf-8")
        m = re.search(r'\.TH OPENRAR 1 "[^"]*" "OpenRAR ([^"]*)"', content)
        versions["debian/openrar.1"] = m.group(1) if m else None
    else:
        versions["debian/openrar.1"] = None

    # 8. docs/man/openrar.1
    man2_path = root / "docs" / "man" / "openrar.1"
    if man2_path.exists():
        content = man2_path.read_text(encoding="utf-8")
        m = re.search(r'\.TH OPENRAR 1 "[^"]*" "OpenRAR ([^"]*)"', content)
        versions["docs/man/openrar.1"] = m.group(1) if m else None
    else:
        versions["docs/man/openrar.1"] = None

    return versions


def check_consistency(root: Path) -> int:
    versions = read_versions(root)
    print("OpenRAR Version Consistency Check:")
    print("-" * 60)
    for path, ver in versions.items():
        status = ver if ver else "MISSING / UNPARSEABLE"
        print(f"  {path:42}: {status}")
    print("-" * 60)

    unique_versions = {v for v in versions.values() if v is not None}
    if None in versions.values():
        print("ERROR: One or more version locations could not be parsed.")
        return 1

    if len(unique_versions) > 1:
        print(f"ERROR: Version mismatch detected across locations: {unique_versions}")
        return 1

    current = next(iter(unique_versions)) if unique_versions else "unknown"
    print(f"SUCCESS: All {len(versions)} locations synchronized at version {current}.")
    return 0


def bump_version(root: Path, new_ver: str, dry_run: bool = False, run_lint: bool = True) -> int:
    if not SEMVER_RE.match(new_ver):
        print(f"ERROR: Invalid semver format '{new_ver}'. Must be MAJOR.MINOR.PATCH (e.g. 1.54.0).")
        return 1

    print(f"Bumping OpenRAR version -> {new_ver} (dry_run={dry_run})")

    now = datetime.now(timezone.utc)
    month_year = now.strftime("%B %Y")
    rfc2822_date = email.utils.format_datetime(now)

    # Two-phase (validate everything, then write everything) so a malformed
    # location cannot leave the tree half-bumped: the previous one-pass version
    # wrote CMakeLists.txt before discovering that, say, package.json no longer
    # matched, producing exactly the version drift this tool exists to prevent.
    edits: list[tuple[str, Path, str]] = []

    def stage(label: str, path: Path, pattern: str, replacement: str, count: int = 1) -> bool:
        c = read_text_exact(path)
        c_new, n = re.subn(pattern, replacement, c, count=count)
        if n == 0:
            print(f"ERROR: Failed to update {label} (pattern not found).")
            return False
        edits.append((label, path, c_new))
        return True

    # 1. CMakeLists.txt
    cmake_path = root / "CMakeLists.txt"
    if cmake_path.exists() and not stage(
        "CMakeLists.txt", cmake_path, r"project\(openrar\s+VERSION\s+\d+\.\d+\.\d+",
        f"project(openrar VERSION {new_ver}"
    ):
        return 1

    # 2. wasm/js/package.json
    pkg_path = root / "wasm" / "js" / "package.json"
    if pkg_path.exists() and not stage(
        "wasm/js/package.json", pkg_path, r'"version":\s*"[^"]+"', f'"version": "{new_ver}"'
    ):
        return 1

    # 3. bindings/python/pyproject.toml
    pyproject_path = root / "bindings" / "python" / "pyproject.toml"
    if pyproject_path.exists() and not stage(
        "bindings/python/pyproject.toml", pyproject_path, r'(?m)^version\s*=\s*"[^"]+"',
        f'version = "{new_ver}"'
    ):
        return 1

    # 4. bindings/python/openrar/__init__.py
    pyinit_path = root / "bindings" / "python" / "openrar" / "__init__.py"
    if pyinit_path.exists() and not stage(
        "bindings/python/openrar/__init__.py", pyinit_path, r'(?m)^__version__\s*=\s*"[^"]+"',
        f'__version__ = "{new_ver}"'
    ):
        return 1

    # 5. bindings/csharp/OpenRAR/OpenRAR.NET.csproj
    csproj_path = root / "bindings" / "csharp" / "OpenRAR" / "OpenRAR.NET.csproj"
    if csproj_path.exists() and not stage(
        "bindings/csharp/OpenRAR/OpenRAR.NET.csproj", csproj_path, r"<Version>[^<]+</Version>",
        f"<Version>{new_ver}</Version>"
    ):
        return 1

    # 6. debian/changelog (prepend a stanza unless the top entry already matches)
    changelog_path = root / "debian" / "changelog"
    if changelog_path.exists():
        c = read_text_exact(changelog_path)
        top_line = c.splitlines()[0] if c else ""
        m = re.search(r"openrar\s+\((\d+\.\d+\.\d+)", top_line)
        if not m or m.group(1) != new_ver:
            eol = dominant_eol(c)
            lines = [
                f"openrar ({new_ver}-0ubuntu1) noble; urgency=medium",
                "",
                f"  * Release v{new_ver}.",
                "",
                f" -- OpenRAR Project <geometric-dev@users.noreply.github.com>  {rfc2822_date}",
                "",
            ]
            stanza = eol.join(lines) + eol
            edits.append(("debian/changelog", changelog_path, stanza + c))

    # 7. debian/openrar.1 & 8. docs/man/openrar.1
    for man_rel in ["debian/openrar.1", "docs/man/openrar.1"]:
        man_path = root / man_rel
        if man_path.exists() and not stage(
            man_rel, man_path, r'^\.TH OPENRAR 1 "[^"]*" "OpenRAR [^"]*"',
            f'.TH OPENRAR 1 "{month_year}" "OpenRAR {new_ver}"', count=1
        ):
            return 1

    for label, path, content in edits:
        if dry_run:
            print(f"  [~] would update {label}")
        else:
            write_text_exact(path, content)
            print(f"  [x] {label}")

    if dry_run:
        print("\nDry-run complete. No files were written.")
        return 0

    print("\nVerifying consistency post-bump:")
    rc = check_consistency(root)
    if rc != 0:
        return rc

    if run_lint:
        anchor_lint_path = root / "tools" / "anchor_lint.py"
        if anchor_lint_path.exists():
            print("\nRunning tools/anchor_lint.py...")
            res = subprocess.run([sys.executable, str(anchor_lint_path)], cwd=str(root))
            if res.returncode != 0:
                print("WARNING: anchor_lint reported violations after bump.")
                return res.returncode

    print(f"\nSuccessfully bumped OpenRAR to v{new_ver} across all targets!")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="OpenRAR Atomic Version Bumper")
    parser.add_argument("version", nargs="?", help="New semantic version (e.g. 1.54.0)")
    parser.add_argument("--check", "-c", action="store_true", help="Check version consistency across all files")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Simulate changes without writing files")
    parser.add_argument("--no-lint", action="store_true", help="Skip running anchor_lint.py after bump")

    args = parser.parse_args()
    root = get_repo_root()

    if args.check or not args.version:
        return check_consistency(root)

    return bump_version(root, args.version, dry_run=args.dry_run, run_lint=not args.no_lint)


if __name__ == "__main__":
    sys.exit(main())
