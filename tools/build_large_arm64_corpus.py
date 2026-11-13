#!/usr/bin/env python3
"""Rebuild the large-ARM64 measurement corpora (synthetic + real binaries).

The committed C5 fixtures are 2 KB, so they cannot show whether the dispatch
win scales. This script reproduces the large-image measurements recorded in
tests/fixtures/c5/README.md:

  * a freestanding C program compiled for aarch64-pc-windows-msvc with clang
    and linked with lld-link (/nodefaultlib), at several function counts;
  * real third-party ARM64 binaries downloaded from pinned URLs and verified
    against pinned SHA-256 hashes.

Every corpus member is written as corpus/<label>/pe_aarch64.exe so archives
made from different samples stay comparable (RAR5 stores the member name).
Generated images are byte-stable per toolchain (/timestamp:0) but depend on
the compiler version, so they go to manifest.generated.sha256 and are not
enforced across hosts; the downloaded archives and their extracted members are
pinned in manifest.sha256.

Usage:

    python tools/build_large_arm64_corpus.py --out build/large_arm64

Then measure a sample (diagnostic, or gated in CI for libcrypto):

    python tools/measure_c5.py \\
        --baseline main=.../openrar.exe --candidate branch=.../openrar.exe \\
        --corpus build/large_arm64/corpus/curl --report-only

Exit status is non-zero if a download, hash check, compile or link fails.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "build" / "large_arm64"
MOD_FUNCS = 256
USER_AGENT = "openrar-c5-measurement/1.0"

# Pinned real samples. archive_sha pins the download; member_sha pins the file
# that becomes corpus/<label>/pe_aarch64.exe.
SOURCES = (
    {
        "label": "7zip",
        "url": "https://www.7-zip.org/a/7z2501-arm64.exe",
        "archive_sha": "6365c7c44e217b9c1009e065daf9f9aa37454e64315b4aaa263f7f8f060755dc",
        "member": None,
        "member_sha": "6365c7c44e217b9c1009e065daf9f9aa37454e64315b4aaa263f7f8f060755dc",
    },
    {
        "label": "curl",
        "url": "https://curl.se/windows/dl-8.23.0_8/curl-8.23.0_8-win64a-mingw.zip",
        "archive_sha": "9ffddd2a85843d3b4b590d24d6becc4f25a70c77781d7d09235ba069b5c4589e",
        "member": "curl.exe",
        "member_glob": "*/bin/curl.exe",
        "member_sha": "38e26717fd1a3b04a92be13bac9442a0c8c0566b7bba1e8d413e225c6592335b",
    },
    {
        "label": "pydll",
        "url": "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-arm64.zip",
        "archive_sha": "cba392bd5c7aed68b26d1b523822cc3d80802c085481dfde79262ebfc7179d26",
        "member": "python313.dll",
        "member_glob": "python313.dll",
        "member_sha": "b4a36b1486e5cb0e23c7aded651e3998aa476089699622779c328fb31350fc8c",
    },
    {
        "label": "pyexe",
        "url": "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-arm64.zip",
        "archive_sha": "cba392bd5c7aed68b26d1b523822cc3d80802c085481dfde79262ebfc7179d26",
        "member": "python.exe",
        "member_glob": "python.exe",
        "member_sha": "0339dc88887f1849f8fd811d1b329ce0fa7503c77ec7a2b3b68113c99320aec6",
    },
    {
        "label": "libcrypto",
        "url": "https://www.python.org/ftp/python/3.13.9/python-3.13.9-embed-arm64.zip",
        "archive_sha": "cba392bd5c7aed68b26d1b523822cc3d80802c085481dfde79262ebfc7179d26",
        "member": "libcrypto-3-arm64.dll",
        "member_glob": "libcrypto-3-arm64.dll",
        "member_sha": "8387f314ecffbb3db2e7c5409ad60c60f93a111f2ad3e62432d5d341d6e51140",
    },
)


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expect_arm64_pe(path: pathlib.Path) -> None:
    data = path.read_bytes()
    if data[:2] != b"MZ" or len(data) < 128:
        raise SystemExit(f"{path} is not a PE image")
    off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[off:off + 4] != b"PE\x00\x00":
        raise SystemExit(f"{path} has no PE header")
    machine = struct.unpack_from("<H", data, off + 4)[0]
    if machine != 0xAA64:
        raise SystemExit(f"{path}: machine 0x{machine:04X}, expected 0xAA64")


# --- synthetic large images ------------------------------------------------


def consts(k: int, m: int) -> list[int]:
    x = (k * 2654435761 + m * 40503 + 12345) & 0xFFFFFFFF
    out = []
    for _ in range(6):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(x)
    return out


def func(m: int, k: int, count: int) -> str:
    c = consts(k, m)
    nxt_k = (k + 1) % count
    s1 = 3 + (c[0] % 9)
    s2 = 5 + (c[1] % 11)
    cases = []
    for j in range(16):
        op = "+" if j % 2 else "^"
        sh = ">>" if j % 3 else "<<"
        val = (c[3] ^ (j * 0x9E3779B9)) & 0xFFFFFFFF
        cases.append(f"        case {j}: acc = (acc {op} 0x{val:08x}u) {sh} {1 + (j % 5)}; break;")
    body = "\n".join(cases)
    return f"""
__attribute__((noinline)) u32 c5_f{m}_{k}(u32 a, const u32 *t, u32 n) {{
    u32 i;
    u32 acc = a ^ 0x{c[0]:08x}u;
    for (i = 0; i < n; ++i) {{
        acc = (acc * 0x{(c[1] | 1):08x}u) + (t[(i + {k}) & 4095] >> {s1});
        if (acc & 0x{(c[2] | 1):08x}u) {{
            acc ^= t[(acc >> {s2}) & 4095];
        }} else {{
            acc += (acc >> {s1}) ^ 0x{c[3]:08x}u;
        }}
        switch (acc & 15u) {{
{body}
        default: acc = (acc >> 2) ^ 0x{c[4]:08x}u; break;
        }}
    }}
    return acc ^ c5_f{m}_{nxt_k}(a + {k}, t + (i & 63), n >> 1);
}}
"""


def module(m: int, nfuncs: int) -> str:
    head = ["typedef unsigned int u32;", "typedef unsigned long long u64;"]
    if m == 0:
        head.append("u32 c5_table[4096];")
    for k in range(nfuncs):
        head.append(f"__attribute__((noinline)) u32 c5_f{m}_{k}(u32 a, const u32 *t, u32 n);")
    return "\n".join(head) + "\n" + "".join(func(m, k, nfuncs) for k in range(nfuncs))


def main_module(nmods: int) -> str:
    decls = "\n".join(f"u32 c5_f{m}_0(u32 a, const u32 *t, u32 n);" for m in range(nmods))
    calls = "\n".join(
        f"    seed ^= c5_f{m}_0(seed, c5_table, {64 + m * 3});" for m in range(nmods)
    )
    return f"""typedef unsigned int u32;
extern u32 c5_table[4096];
{decls}

unsigned int c5_main(void) {{
    u32 seed = 0x9e3779b9u;
{calls}
    return seed;
}}
"""


def run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(" ".join(cmd), file=sys.stderr)
        print(proc.stdout + proc.stderr, file=sys.stderr)
        raise SystemExit(1)


def tool_version(cmd: list[str]) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    text = (proc.stdout + proc.stderr).strip().splitlines()
    return text[0] if text else "unknown"


def build_synthetic(nfuncs: int, out: pathlib.Path, clang: str, lld: str) -> pathlib.Path:
    src = out / "synthetic_src"
    obj = out / "synthetic_obj"
    src.mkdir(parents=True, exist_ok=True)
    obj.mkdir(parents=True, exist_ok=True)
    nmods = (nfuncs + MOD_FUNCS - 1) // MOD_FUNCS
    files = []
    for m in range(nmods):
        p = src / f"m{m}.c"
        p.write_text(module(m, min(MOD_FUNCS, nfuncs - m * MOD_FUNCS)), encoding="utf-8", newline="\n")
        files.append(p)
    mp = src / "main.c"
    mp.write_text(main_module(nmods), encoding="utf-8", newline="\n")
    files.append(mp)

    def compile_one(p: pathlib.Path) -> pathlib.Path:
        o = obj / (p.stem + ".o")
        run([clang, "--target=aarch64-pc-windows-msvc", "-O2", "-c",
             "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
             "-fno-asynchronous-unwind-tables", str(p), "-o", str(o)])
        return o

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(compile_one, files))
    image = out / f"pe_{nfuncs}.exe"
    # /timestamp:0 removes the link-time clock from the PE header, which is the
    # only source of byte variation between identical builds.
    run([lld, "/machine:arm64", "/entry:c5_main", "/subsystem:console",
         "/nodefaultlib", "/opt:noref", "/timestamp:0", f"/out:{image}"]
        + [str(o) for o in objs])
    return image


# --- real samples -----------------------------------------------------------


def download(url: str, dest: pathlib.Path, attempts: int = 3) -> None:
    """Fetch url into dest, retrying transient failures with backoff.

    A partial file is removed before each retry so a truncated download can
    never be mistaken for a cached archive by a later run.
    """
    last = None
    for attempt in range(attempts):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=120) as resp, dest.open("wb") as f:
                shutil.copyfileobj(resp, f)
            return
        except (urllib.error.URLError, OSError) as exc:
            last = exc
            dest.unlink(missing_ok=True)
            if attempt + 1 < attempts:
                time.sleep(2 ** attempt)
    raise SystemExit(f"{url}: download failed after {attempts} attempts: {last}")


def fetch(source: dict, downloads: pathlib.Path, offline: bool) -> pathlib.Path:
    name = source["url"].rsplit("/", 1)[-1]
    path = downloads / name
    if path.is_file() and sha256(path) == source["archive_sha"]:
        return path
    if offline:
        raise SystemExit(f"{path} is missing or has the wrong hash and --offline was given")
    path.parent.mkdir(parents=True, exist_ok=True)
    download(source["url"], path)
    got = sha256(path)
    if got != source["archive_sha"]:
        path.unlink()
        raise SystemExit(f"{source['url']}: sha256 {got}, expected {source['archive_sha']}")
    return path


def extract_member(source: dict, archive: pathlib.Path, dest: pathlib.Path) -> None:
    if source["member"] is None:
        shutil.copyfile(archive, dest)
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    glob = source.get("member_glob", source["member"])
    with zipfile.ZipFile(archive) as z:
        names = [n for n in z.namelist()
                 if fnmatch.fnmatch(n, glob) and n.rsplit("/", 1)[-1] == source["member"]]
        if not names:
            raise SystemExit(f"{archive.name}: no member matching {glob}")
        with z.open(names[0]) as f, dest.open("wb") as o:
            shutil.copyfileobj(f, o)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--synthetic", default="128,256,512,1024,2048",
                    help="comma-separated function counts")
    ap.add_argument("--skip-synthetic", action="store_true")
    ap.add_argument("--skip-real", action="store_true")
    ap.add_argument("--only", default="",
                    help="fetch only these real sample labels (comma-separated)")
    ap.add_argument("--offline", action="store_true", help="never download; require pinned files")
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--lld-link", default="lld-link")
    args = ap.parse_args()

    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    corpus = out / "corpus"
    corpus.mkdir(exist_ok=True)
    manifest = {"synthetic": [], "real": []}

    if not args.skip_synthetic:
        counts = [int(x) for x in args.synthetic.split(",") if x.strip()]
        bad = [n for n in counts if n <= 0]
        if bad:
            raise SystemExit(f"function counts must be positive: {bad}")
        manifest["clang"] = tool_version([args.clang, "--version"])
        manifest["lld_link"] = tool_version([args.lld_link, "--version"])
        for n in counts:
            image = build_synthetic(n, out, args.clang, args.lld_link)
            dest = corpus / f"gen_{n}" / "pe_aarch64.exe"
            dest.parent.mkdir(exist_ok=True)
            shutil.copyfile(image, dest)
            expect_arm64_pe(dest)
            manifest["synthetic"].append(
                {"label": f"gen_{n}", "functions": n, "bytes": dest.stat().st_size,
                 "sha256": sha256(dest)})
            print(f"  gen_{n:<5} {dest.stat().st_size:>9,} B  sha256 {sha256(dest)[:16]}…")

    if not args.skip_real:
        downloads = out / "downloads"
        downloads.mkdir(exist_ok=True)
        sources = SOURCES
        if args.only:
            wanted = [s.strip() for s in args.only.split(",") if s.strip()]
            known = {s["label"] for s in SOURCES}
            unknown = [s for s in wanted if s not in known]
            if unknown:
                raise SystemExit(f"unknown --only labels: {unknown}")
            sources = [s for s in SOURCES if s["label"] in wanted]
        for source in sources:
            archive = fetch(source, downloads, args.offline)
            dest = corpus / source["label"] / "pe_aarch64.exe"
            dest.parent.mkdir(exist_ok=True)
            extract_member(source, archive, dest)
            got = sha256(dest)
            if got != source["member_sha"]:
                raise SystemExit(f"{source['label']}: member sha256 {got}, "
                                 f"expected {source['member_sha']}")
            expect_arm64_pe(dest)
            manifest["real"].append(
                {"label": source["label"], "url": source["url"],
                 "archive_sha256": source["archive_sha"], "member": source["member"],
                 "bytes": dest.stat().st_size, "sha256": got})
            print(f"  {source['label']:<7} {dest.stat().st_size:>9,} B  sha256 {got[:16]}…")

    # manifest.sha256 lists only samples whose bytes are reproducible on any
    # host (the pinned downloads). Generated images are byte-stable per
    # toolchain (/timestamp:0) but legitimately differ across clang/lld
    # versions, so they get a separate, informational manifest.
    pinned = [f"{entry['sha256']}  corpus/{entry['label']}/pe_aarch64.exe"
              for entry in manifest["real"]]
    generated = [f"{entry['sha256']}  corpus/{entry['label']}/pe_aarch64.exe"
                 for entry in manifest["synthetic"]]
    # A filtered run (--only, or a subset of samples) must not erase hashes
    # it never verified: refuse to drop lines from an existing pinned manifest.
    existing = out / "manifest.sha256"
    if not args.skip_real and existing.is_file():
        dropped = (set(existing.read_text(encoding="utf-8").splitlines())
                   - set(pinned))
        if dropped:
            raise SystemExit(
                f"{existing} lists {len(dropped)} sample(s) this run did not "
                "verify; use a fresh --out instead of overwriting it")

    # Only rewrite a manifest whose class was actually rebuilt; a
    # --skip-real run must not truncate the pinned list.
    if pinned or not args.skip_real:
        (out / "manifest.sha256").write_text("\n".join(pinned) + ("\n" if pinned else ""),
                                             encoding="utf-8", newline="\n")
    if generated or not args.skip_synthetic:
        (out / "manifest.generated.sha256").write_text(
            "\n".join(generated) + ("\n" if generated else ""), encoding="utf-8", newline="\n")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                       encoding="utf-8", newline="\n")
    print(f"wrote {len(pinned) + len(generated)} corpus members to {corpus}")
    print(f"pinned manifest: {len(pinned)} real samples; generated (informational): "
          f"{len(generated)}")
    print("measure one with: python tools/measure_c5.py --baseline main=... "
          "--candidate branch=... --corpus <dir> --report-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
