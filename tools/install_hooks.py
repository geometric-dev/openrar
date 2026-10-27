#!/usr/bin/env python3
"""tools/install_hooks.py
Installs or uninstalls local Git pre-commit hooks for OpenRAR.

Configures git to run `tools/git_hooks/pre-commit` prior to every commit,
enforcing:
  - Version consistency (tools/bump_version.py --check)
  - Anchor lint & verification ledger parity (tools/anchor_lint.py)
  - Clang-format conformance on staged C/C++ files

Usage:
  python tools/install_hooks.py
  python tools/install_hooks.py --uninstall
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def make_executable(path: Path) -> None:
    if not path.exists():
        return
    try:
        current = path.stat().st_mode
        path.chmod(current | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass


def install_hooks(repo_root: Path) -> int:
    hooks_src_dir = repo_root / "tools" / "git_hooks"
    pre_commit_src = hooks_src_dir / "pre-commit"

    if not pre_commit_src.exists():
        print(f"ERROR: Source hook not found: {pre_commit_src}")
        return 1

    make_executable(pre_commit_src)

    # 1. Configure git core.hooksPath
    res = subprocess.run(["git", "config", "core.hooksPath", "tools/git_hooks"], cwd=str(repo_root))
    if res.returncode != 0:
        print("WARNING: 'git config core.hooksPath tools/git_hooks' failed.")

    # 2. Also copy to .git/hooks/pre-commit as fallback
    git_dir = repo_root / ".git"
    if git_dir.is_dir():
        hooks_dst_dir = git_dir / "hooks"
        hooks_dst_dir.mkdir(parents=True, exist_ok=True)
        pre_commit_dst = hooks_dst_dir / "pre-commit"
        shutil.copy2(pre_commit_src, pre_commit_dst)
        make_executable(pre_commit_dst)
        print(f"  [x] Copied to {pre_commit_dst}")

    print("  [x] Configured git core.hooksPath -> tools/git_hooks")
    print("\nSUCCESS: OpenRAR Git pre-commit hooks installed successfully.")
    print("Pre-commit checks will now run automatically on each 'git commit'.")
    return 0


def uninstall_hooks(repo_root: Path) -> int:
    subprocess.run(["git", "config", "--unset", "core.hooksPath"], cwd=str(repo_root))
    git_dir = repo_root / ".git"
    if git_dir.is_dir():
        pre_commit_dst = git_dir / "hooks" / "pre-commit"
        if pre_commit_dst.exists():
            pre_commit_dst.unlink()
            print(f"  [x] Removed {pre_commit_dst}")

    print("  [x] Unset git core.hooksPath")
    print("\nSUCCESS: OpenRAR Git hooks uninstalled.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Install OpenRAR local Git hooks")
    parser.add_argument("--uninstall", action="store_true", help="Remove installed Git hooks")
    args = parser.parse_args()

    repo_root = get_repo_root()
    if args.uninstall:
        return uninstall_hooks(repo_root)
    return install_hooks(repo_root)


if __name__ == "__main__":
    sys.exit(main())
