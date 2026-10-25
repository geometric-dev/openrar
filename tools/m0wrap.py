#!/usr/bin/env python3
"M0 one-shot: wrap flat main() test suites into runner case tables. dry/apply."
import re
import sys
import pathlib

ROOT = pathlib.Path("tests/unit")
RUNNER_H = '#include "../support/test_runner.hpp"\n'
ROUTING = re.compile(
    r"\s*(#\s*ifdef\s+_MSC_VER|#\s*endif|#\s*if\b|#\s*else"
    r"|_CrtSetReport\w+\([^)]*\)|_set_abort_behavior\([^)]*\)|"
    r"OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR\(\);|_set_error_mode\([^)]*\);)")
BANNER = re.compile(r'\s*(std::cout\s*<<\s*".*")(?:\s*<<\s*std::(?:endl|flush))?\s*;\s*$')
CALL = re.compile(r"\s*(test_\w+|\w+)\(\);\s*$")
RETURN = re.compile(r"\s*return\s+(\d+)\s*;\s*$")
RETURN_FAILS = re.compile(r"\s*return\s+fails\s*==\s*0\s*\?\s*0\s*:\s*1\s*;\s*$")
FINALCHECK = re.compile(r'\s*if\s*\(\s*fails\s*==\s*0\s*\)\s*std::printf\("([^"]*)"\);')
HARMLESS = re.compile(r"\s*(setvbuf\([^)]*\);|std::cout\.flush\(\);|"
                      r"std::cout\s*<<\s*std::(?:flush|endl)\s*;)\s*$")


def find_main(txt):
    m = re.search(r"int main\s*\(([^)]*)\)\s*\{", txt)
    if not m:
        return None
    op = txt.index("{", m.start())
    depth = 0
    for j in range(op, len(txt)):
        if txt[j] == "{":
            depth += 1
        elif txt[j] == "}":
            depth -= 1
            if depth == 0:
                return m.start(), op, j
    return None


def classify(body):
    cat, seq = {}, []
    for ln in body.splitlines():
        key = ("routing" if ROUTING.match(ln) else
               "banner" if BANNER.match(ln) else
               "call" if CALL.match(ln) else
               "return" if RETURN.match(ln) else
               "harmless" if HARMLESS.match(ln) else
               "blank" if not ln.strip() else
               "comment" if re.match(r"\s*(//|/\*|\*)", ln) else "other")
        if key in ("comment", "other") and "std::cout" in ln:
            key = "banner"
        cat.setdefault(key, []).append(ln)
        seq.append((key, ln))
    return cat, seq


def guard(banner_line, final=False):
    m = BANNER.match(banner_line)
    if not m:
        return None
    cond = ("rc == 0 && !openrar::test::last_run_was_filtered()" if final
            else "!openrar::test::last_run_was_filtered()")
    return f"if ({cond}) {m.group(1)};"


def insert_runner_include(text):
    lines = text.splitlines()
    # Walk the TOP include run: include/blank/comment lines from the top until
    # the first real content line (using-namespace, namespace-open, code).
    last_inc = -1
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("#include"):
            last_inc = i
        elif s and not (s.startswith("//") or s.startswith("/*") or s.startswith("*")
                        or s.startswith("using") or s.startswith("namespace")):
            break
    if last_inc < 0:
        raise ValueError("no include block")
    lines.insert(last_inc + 1, RUNNER_H.rstrip("\n"))
    return "\n".join(lines)


def wrap_calllist(txt, sig_start, sig_text, op, cl, path):
    cat, seq = classify(txt[op + 1:cl])
    # Counter-suite conversions: keep lines but re-key them for the emit path.
    TAIL_FAIL = ('if (fails) {',
                 'std::fprintf(stderr, "%d failure(s)\\n", fails);',
                 'return 1;',
                 '}')
    for i, (k, _, ) in enumerate(seq):
        s = seq[i][1]
        if s.strip() == 'if (fails) {':
            seq[i] = ("tailfail", s)
        elif s.strip() == 'std::fprintf(stderr, "%d failure(s)\\n", fails);':
            seq[i] = ("tailfail", s)
        elif s.strip() == 'return 1;' and i > 0 and seq[i - 1][1].strip().startswith('std::fprintf'):
            seq[i] = ("tailfail", s)
        elif RETURN_FAILS.match(s):
            seq[i] = ("return_fails", s)
        elif FINALCHECK.match(s):
            seq[i] = ("finalcheck", s)
        elif re.match(r'\s*std::fprintf\(stderr,\s*"(\w[\w.: -]*OK\\?n?)"', s):
            seq[i] = ("okprint", s)
    seq = [s for s in seq if s[0] not in ("tailfail", "return_fails")]
    others = [l for l in cat.get("other", []) if
              l.strip() not in ('if (fails) {', 'std::fprintf(stderr, "%d failure(s)\\n", fails);',
                                'return 1;', '}', 'return fails == 0 ? 0 : 1;')
              and not FINALCHECK.match(l)
              and not re.match(r'\s*std::fprintf\(stderr,\s*"(\w[\w.: -]*OK\\n)"', l)]
    bad_ret = []
    if others or bad_ret:
        return None, f"REJECT other={others[:2]!r} badret={bad_ret}"
    calls = [CALL.match(l).group(1) for l in cat["call"]]
    if len(set(calls)) != len(calls):
        return None, "REJECT dup-calls"
    first_call = next((i for i, (k, _) in enumerate(seq) if k == "call"), None)
    prelude = [l for k, l in seq[:first_call or 0] if k == "banner"]
    finals = [l for k, l in seq[(first_call + 1) if first_call is not None else 0:]
              if k in ("banner", "finalcheck")]
    if len(finals) > 1 or len(prelude) > 2:
        return None, f"REJECT banners pre={len(prelude)} fin={len(finals)}"
    setvbuf = [l for l in cat.get("harmless", []) if "setvbuf" in l]
    argv_files = {"extraction_atomic_tests.cpp", "sfx_e2e_tests.cpp"}
    if path in argv_files:
        return None, "MANUAL argv suite"
    out = [TABLE_HDR := "static const openrar::test::Case k_cases[] = {"]
    for c in calls:
        out.append(f'    {{"{c}", {c}, false}},')
    out[-1] = out[-1].rstrip(",") + "};"
    main = ["int main(int argc, char* argv[]) {",
            "    openrar::test::route_crt_asserts_to_stderr();"]
    main += [s for s in setvbuf]
    new = []
    for l in prelude:
        g = guard(l)
        new.append(g if g else l)
    main += new
    main += ["    int rc = openrar::test::openrar_test_main(k_cases, " +
             str(len(calls)) + ", argc, argv);"]
    counter_mode = re.search(r"\bint fails\s*=|!\w+\s*\bfails\b", txt) is not None
    if counter_mode:
        main += ["    if (fails != 0) { std::fprintf(stderr, \"%d failure(s)\\n\", fails); rc = 1; }"]
    for l in finals:
        if FINALCHECK.match(l):
            m = FINALCHECK.match(l)
            main.append(f'    if (rc == 0 && !openrar::test::last_run_was_filtered()) '
                        f'return printf_rc_guard("{m.group(1)}"[unused]);')
            # safer: plain guarded printf via C++ iostream-free wrapper
            main.pop()
            main.append(f'    if (rc == 0 && !openrar::test::last_run_was_filtered())')
            main.append(f'        std::printf("{m.group(1)}");')
        else:
            g = guard(l, final=True)
            main.append(g if g else l)
    okprints = [l for k, l in seq if k == "okprint"]
    for l in okprints:
        m = re.match(r'\s*std::fprintf\(stderr,\s*"([^"]*)"', l)
        if m:
            main.append(f'    if (rc == 0 && !openrar::test::last_run_was_filtered())')
            main.append(f'        std::fprintf(stderr, "{m.group(1)}");')
    main.append("    return rc;")
    main.append("}")
    body = "\n".join(out) + "\n\n" + "\n".join(main) + "\n"
    return txt[:sig_start] + body, f"OK calls={len(calls)}"


def wrap_inline(txt, sig_text, op, cl, path):
    if "argc" in sig_text or "argv" in sig_text:
        return None, "MANUAL argv suite"
    body = txt[op + 1:cl]
    out = body.splitlines()
    guards = []
    counter_fix = False
    i = 0
    while i < len(out):
        l = out[i]
        # exact 4-line tailfail group: if (fails) { fprintf; return 1; }
        if (l.strip() == "if (fails) {" and i + 3 < len(out)
                and out[i + 1].strip() == 'std::fprintf(stderr, "%d failure(s)\\n", fails);'
                and out[i + 2].strip() == "return 1;"
                and out[i + 3].strip() == "}"):
            out[i:i + 4] = []
            counter_fix = True
            continue
        g = guard(l)  # inside a case fn: filter-guard only, no rc reference
        if g:
            out[i] = "    " + g
            guards.append(l.strip()[:44])
            i += 1
            continue
        if RETURN_FAILS.match(l):
            out[i] = (RETURN_FAILS.sub('if (fails != 0) throw std::logic_error('
                                       'std::to_string(fails) + " failed checks");', l))
            counter_fix = True
            i += 1
            continue
        mfc = FINALCHECK.match(l)
        if mfc:
            indent = l[:len(l) - len(l.lstrip())]
            out[i] = (f'{indent}if (fails != 0) throw std::logic_error('
                      f'std::to_string(fails) + " failed checks");')
            out.insert(i + 1, f'{indent}if (!openrar::test::last_run_was_filtered())')
            out.insert(i + 2, f'{indent}    std::printf("{mfc.group(1)}");')
            counter_fix = True
            i += 3
            continue
        if re.match(r"\s*return\s+fails", l):
            out[i] = (re.sub(r"return\s+fails\s*\?\s*1\s*:\s*0\s*;",
                             'if (fails != 0) throw std::logic_error(std::to_string('
                             'fails) + " failed checks");', l))
            counter_fix = not (out[i] == l)
            i += 1
            continue
        if re.match(r"\s*return\s+0\s*;", l):
            out[i] = re.sub(r"return\s+0\s*;", "return;", l)
            i += 1
            continue
        if re.match(r"\s*return\s+1\s*;", l):
            out[i] = re.sub(r"return\s+1\s*;", 'throw std::runtime_error("return-1 path reached");', l)
            counter_fix = True
            i += 1
            continue
        if RETURN.match(l) and RETURN.match(l).group(1) not in ("0",):
            return None, f"REJECT return->{RETURN.match(l).group(1)}"
        i += 1
    suite = "run_suite_body"
    table = ["static const openrar::test::Case k_cases[] = {",
             f'    {{"{suite}", {suite}, false}},',
             "};",
             "",
             "int main(int argc, char* argv[]) {",
             "    openrar::test::route_crt_asserts_to_stderr();",
             "    return openrar::test::openrar_test_main(k_cases, 1, argc, argv);",
             "}"]
    wrapped = (f"static void {suite}() {{" + "\n" + "\n".join(out) + "\n}\n\n"
               + "\n".join(table) + "\n")
    return txt[:sig_start] + wrapped, f"OK banners_guarded={guards} counter={counter_fix}"


def process(path, dry):
    p = ROOT / path
    txt = p.read_text(encoding="utf-8", errors="strict")
    if "test_runner.hpp" in txt:
        print(f"== {path}: ALREADY-WRAPPED")
        return
    r = find_main(txt)
    if not r:
        print(f"== {path}: NOMAIN")
        return
    main_sig, op, cl = r
    sig_text = txt[main_sig:op]
    cat, seq = classify(txt[op + 1:cl])
    calls = len(cat.get("call", []))
    kind = ("CALLLIST" if calls >= 3 else "INLINE") if not cat.get("other") else "SUSPECT"
    print(f"== {path}: {kind} calls={calls} banners={len(cat.get('banner', []))} "
          f"other={len(cat.get('other', []))} argc={'argc' in sig_text}")
    for o in cat.get("other", [])[:4]:
        print(f"   OTHER: {o.strip()[:86]}")
    if dry:
        for b in cat.get("banner", [])[:3]:
            print(f"   BANNER: {b.strip()[:80]}")
        return
    new, note = wrap_calllist(txt, main_sig, sig_text, op, cl, path)
    if new is None:
        new, note = wrap_inline(txt, main_sig, sig_text, op, cl, path)
    if new is None:
        print(f"   {note}")
        return
    p.write_text(insert_runner_include(new), encoding="utf-8")
    print(f"   APPLIED [{kind}] {note}")


if __name__ == "__main__":
    dry = "--apply" not in sys.argv
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    targets = files or sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("*.cpp"))
    for f in targets:
        try:
            process(f, dry)
        except Exception as e:
            print(f"== {f}: ERROR {type(e).__name__}: {e}")
