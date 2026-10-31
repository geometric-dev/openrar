#!/usr/bin/env python3
"""
tools/packaging/launchpad_release.py
Launchpad PPA and Debian source packaging manager for OpenRAR.

Verifies debian/ packaging metadata integrity, synchronizes version stamps,
generates distribution-specific changelogs (noble, jammy, plucky), builds
source packages, and coordinates Launchpad PPA publication.

Usage:
    python tools/packaging/launchpad_release.py --verify-only
    python tools/packaging/launchpad_release.py --build-source --distros noble,jammy
    python tools/packaging/launchpad_release.py --upload --ppa ppa:geometric-dev/openrar
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REQUIRED_DEBIAN_FILES = [
    "debian/control",
    "debian/rules",
    "debian/changelog",
    "debian/copyright",
    "debian/source/format",
    "debian/source/options",
    "debian/openrar.install",
    "debian/libopenrar1.install",
    "debian/libopenrar-dev.install",
    "debian/openrar.manpages",
    "debian/openrar.1",
    "debian/watch",
]

def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent

def extract_cmake_version(repo_root: Path) -> str:
    cmakelists = repo_root / "CMakeLists.txt"
    content = cmakelists.read_text(encoding="utf-8")
    m = re.search(r"project\(openrar VERSION ([0-9]+\.[0-9]+\.[0-9]+)", content)
    if not m:
        raise ValueError(f"Could not find project VERSION in {cmakelists}")
    return m.group(1)

def verify_version_synchrony(repo_root: Path, target_version: str) -> list[str]:
    errors = []
    
    # 1. CMakeLists.txt
    cmake_ver = extract_cmake_version(repo_root)
    if cmake_ver != target_version:
        errors.append(f"CMakeLists.txt version ({cmake_ver}) != target ({target_version})")

    # 2. Python pyproject.toml
    pyproject = repo_root / "bindings" / "python" / "pyproject.toml"
    if pyproject.exists():
        py_content = pyproject.read_text(encoding="utf-8")
        m = re.search(r'version\s*=\s*"([^"]+)"', py_content)
        if not m or m.group(1) != target_version:
            errors.append(f"bindings/python/pyproject.toml version ({m.group(1) if m else 'missing'}) != target ({target_version})")

    # 3. C# .csproj
    csproj = repo_root / "bindings" / "csharp" / "OpenRAR" / "OpenRAR.NET.csproj"
    if csproj.exists():
        cs_content = csproj.read_text(encoding="utf-8")
        m = re.search(r'<Version>([^<]+)</Version>', cs_content)
        if not m or m.group(1) != target_version:
            errors.append(f"bindings/csharp/OpenRAR/OpenRAR.NET.csproj version ({m.group(1) if m else 'missing'}) != target ({target_version})")

    # 4. WASM package.json
    pkg_json = repo_root / "wasm" / "js" / "package.json"
    if pkg_json.exists():
        pkg_content = pkg_json.read_text(encoding="utf-8")
        m = re.search(r'"version":\s*"([^"]+)"', pkg_content)
        if not m or m.group(1) != target_version:
            errors.append(f"wasm/js/package.json version ({m.group(1) if m else 'missing'}) != target ({target_version})")

    # 5. debian/changelog
    changelog = repo_root / "debian" / "changelog"
    if changelog.exists():
        cl_first = changelog.read_text(encoding="utf-8").splitlines()[0]
        m = re.search(r'openrar \(([0-9]+\.[0-9]+\.[0-9]+)', cl_first)
        if not m or m.group(1) != target_version:
            errors.append(f"debian/changelog top entry version ({m.group(1) if m else 'missing'}) != target ({target_version})")

    return errors

def verify_debian_tree(repo_root: Path) -> list[str]:
    errors = []
    for rel_path in REQUIRED_DEBIAN_FILES:
        target = repo_root / rel_path
        if not target.exists():
            errors.append(f"Missing required Debian packaging file: {rel_path}")
        elif target.stat().st_size == 0:
            errors.append(f"Debian packaging file is empty: {rel_path}")

    # Check debian/rules tab formatting
    rules_path = repo_root / "debian" / "rules"
    if rules_path.exists():
        raw = rules_path.read_bytes()
        for idx, line in enumerate(raw.splitlines(), start=1):
            if line.startswith(b" ") and not line.strip().startswith(b"#"):
                errors.append(f"debian/rules line {idx} starts with spaces instead of tabs")

    # Check debian/control binary packages
    control_path = repo_root / "debian" / "control"
    if control_path.exists():
        control_text = control_path.read_text(encoding="utf-8")
        expected_pkgs = ["Package: openrar", "Package: libopenrar1", "Package: libopenrar-dev"]
        for pkg in expected_pkgs:
            if pkg not in control_text:
                errors.append(f"debian/control missing expected binary package declaration: '{pkg}'")

    # Check debian/copyright DEP-5 header
    copyright_path = repo_root / "debian" / "copyright"
    if copyright_path.exists():
        c_text = copyright_path.read_text(encoding="utf-8")
        if "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/" not in c_text:
            errors.append("debian/copyright missing standard DEP-5 Format specification URL")

    # Check debian/source/format
    format_path = repo_root / "debian" / "source" / "format"
    if format_path.exists():
        fmt_text = format_path.read_text(encoding="utf-8").strip()
        if fmt_text not in ("3.0 (quilt)", "3.0 (native)"):
            errors.append(f"debian/source/format '{fmt_text}' is not standard ('3.0 (quilt)' or '3.0 (native)')")

    return errors

def format_rfc2822_date(dt: datetime = None) -> str:
    if dt is None:
        dt = datetime.now(timezone.utc)
    return dt.strftime("%a, %d %b %Y %H:%M:%S +0000")

def generate_series_changelog(repo_root: Path, version: str, series: str, out_path: Path):
    debian_version = f"{version}-0ubuntu2~{series}1"
    now_str = format_rfc2822_date()
    content = f"""openrar ({debian_version}) {series}; urgency=medium

  * Automated Launchpad PPA build for Ubuntu {series}.
  * Upstream OpenRAR release v{version}.
  * Multi-binary split: openrar, libopenrar1, libopenrar-dev.
  * Native C++17 RAR 5.0 engine with frozen C ABI (libopenrar.so.1).

 -- OpenRAR Project <geometric-dev@users.noreply.github.com>  {now_str}
"""
    out_path.write_text(content, encoding="utf-8")

def build_source_package(repo_root: Path, version: str, series: str, output_dir: Path, key_id: str = None) -> Path:
    staging_dir = output_dir / f"openrar-{version}"
    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)

    orig_tarball = output_dir / f"openrar_{version}.orig.tar.gz"
    if not orig_tarball.exists():
        print(f"Creating orig tarball: {orig_tarball.name}")
        subprocess.check_call(
            ["git", "archive", "--format=tar.gz", f"--prefix=openrar-{version}/", "-o", str(orig_tarball), "HEAD"],
            cwd=repo_root
        )

    # Extract orig tarball
    subprocess.check_call(["tar", "-xzf", str(orig_tarball), "-C", str(output_dir)])

    # Copy debian/ folder
    shutil.copytree(repo_root / "debian", staging_dir / "debian", dirs_exist_ok=True)

    # Write customized changelog for this Ubuntu series
    generate_series_changelog(repo_root, version, series, staging_dir / "debian" / "changelog")

    # Run dpkg-buildpackage -S
    cmd = ["dpkg-buildpackage", "-S", "-sa", "-d"]
    if key_id:
        cmd.extend(["-k" + key_id])
    else:
        cmd.extend(["-us", "-uc"])

    print(f"Building source package for {series}...")
    subprocess.check_call(cmd, cwd=staging_dir)

    changes_file = output_dir / f"openrar_{version}-0ubuntu1~{series}1_source.changes"
    return changes_file

def main():
    parser = argparse.ArgumentParser(description="OpenRAR Launchpad & Debian Packaging Manager")
    parser.add_argument("--verify-only", action="store_true", help="Run Debian packaging and version verification gates")
    parser.add_argument("--build-source", action="store_true", help="Build Debian source package")
    parser.add_argument("--distros", default="resolute,noble,jammy", help="Comma-separated Ubuntu release series (e.g. resolute,noble,jammy)")
    parser.add_argument("--output-dir", default="build/debian-source", help="Directory for source package artifacts")
    parser.add_argument("--sign-key", default=None, help="GPG Key ID for signing source package")
    parser.add_argument("--upload", action="store_true", help="Upload built source packages to Launchpad PPA via dput")
    parser.add_argument("--ppa", default="ppa:geometric-dev/openrar", help="Launchpad PPA target (default: ppa:geometric-dev/openrar)")
    args = parser.parse_args()

    repo_root = get_repo_root()
    version = extract_cmake_version(repo_root)
    print(f"OpenRAR Launchpad Packaging Tool [Target Version: {version}]")

    # 1. Verification
    print("Verifying Debian tree and version lockstep...")
    tree_errors = verify_debian_tree(repo_root)
    sync_errors = verify_version_synchrony(repo_root, version)
    all_errors = tree_errors + sync_errors

    if all_errors:
        print(f"FAIL: {len(all_errors)} verification errors found:", file=sys.stderr)
        for err in all_errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    print("SUCCESS: Debian packaging tree and version synchrony verified.")

    if args.verify_only:
        sys.exit(0)

    # 2. Build Source
    distros = [d.strip() for d in args.distros.split(",") if d.strip()]
    out_dir = repo_root / args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    built_changes = []

    if args.build_source:
        for series in distros:
            changes = build_source_package(repo_root, version, series, out_dir, args.sign_key)
            built_changes.append(changes)
            print(f"Generated: {changes}")

    # 3. Upload
    if args.upload:
        if not built_changes:
            # Look for existing changes files in output directory
            built_changes = list(out_dir.glob("*.changes"))
        if not built_changes:
            print("No .changes files found to upload.", file=sys.stderr)
            sys.exit(1)
        for cf in built_changes:
            print(f"Uploading {cf.name} to {args.ppa} via dput...")
            subprocess.check_call(["dput", args.ppa, str(cf)])
        print("Upload to Launchpad PPA completed successfully.")

if __name__ == "__main__":
    main()
