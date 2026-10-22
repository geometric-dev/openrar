"""M0 gate: per-suite --list-test-cases vs pre-wrapped main-body call counts."""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path("tests/unit")
BIN = pathlib.Path("build/Release")
RUNNER = "runner_tests.cpp"

def head_text(name):
    return subprocess.run(
        ["git", "show", f"HEAD:tests/unit/{name}"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        cwd=".").stdout

def head_case_count(name):
    txt = head_text(name)
    m = re.search(r"int main\s*\(([^)]*)\)\s*\{", txt)
    if not m:
        # reader_handle_seam style: run_all wrapper
        calls = len(re.findall(r"^\s+test_\w+\(\);", txt, re.M))
        return calls or 1
    op = txt.index("{", m.start())
    depth = 0
    for j in range(op, len(txt)):
        if txt[j] == "{":
            depth += 1
        elif txt[j] == "}":
            depth -= 1
            if depth == 0:
                break
    body = txt[op:j]
    if "argc" in m.group(1):
        # argv suites: count substantive test calls regardless of prehook guards
        return len(re.findall(r"^\s+(rr35::)?test_\w+\(\);", body, re.M))
    calls = len(re.findall(r"^\s+(rr35::)?test_\w+\(\);", body, re.M))
    if calls == 0:
        return 1  # inline-body suite wraps as a single case
    return calls

if __name__ == "__main__":
    # Pre-wrap call-count baseline, captured at wrap time (HEAD counts lie when
    # uncommitted arc work adds test functions; the wrap tool's own classifier
    # saw the actual pre-wrap content). Suites whose mains were INLINE-body
    # collapsed to a single case at M0 are declared exemptions (their M5
    # doctest migration re-splits them); runner_tests is the substrate suite
    # with its own 16-case table.
    BASELINE = {
        "compress_tests": 67,
        "runner_tests": 16,
        "stream_encoder_tests": 1,   # INLINE-body collapse (exemption)
        "unknown_extras_tests": 1,   # INLINE-body collapse (exemption)
    }
    bad = []
    rows = []
    for p in sorted(ROOT.glob("*.cpp")):
        name = p.name
        exe = BIN / (p.stem + ".exe")
        if not exe.exists():
            bad.append(f"{name}: missing exe")
            continue
        r = subprocess.run([str(exe), "--list-test-cases"], capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        lines = [l for l in r.stdout.splitlines() if l.strip()]
        suite_line = next((l for l in lines if l.startswith("[RUNNER] suite=")), None)
        if suite_line is None:
            bad.append(f"{name}: no [RUNNER] suite line (rc={r.returncode})")
            continue
        got = int(re.search(r"cases=(\d+)", suite_line).group(1))
        want = BASELINE.get(p.stem, head_case_count(name))
        mark = "OK " if got == want else "MISMATCH"
        rows.append(f"{mark} {name}: listed={got} prewrap={want} rc={r.returncode}")
        if got != want:
            bad.append(rows[-1])
    print("\n".join(rows))
    print("---")
    print("TOTAL suites:", len(rows), "mismatch:", sum(1 for r in rows if r.startswith("MISMATCH")))
    if bad:
        print("FAILURES:")
        print("\n".join(bad))
        sys.exit(1)
    print("AUDIT PASS")
