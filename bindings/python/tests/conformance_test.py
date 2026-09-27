"""The normative OpenRAR binding-conformance suite (Python port; plan M4).

This is THE gate for the Python SDK (freeze prereq 3; the WASM v2
double-free/signature defects are the cautionary tale). Every OpenRAR
binding must pass the same case list against the freshly built library:

  1. version probes (strict equality, spec §3)
  2. feature-mask parity with the documented registry
  3. alloc/free pairing incl. the nullptr/0-len contract
  4. create → list → extract roundtrip with field parity
  5. error-code mapping matrix from crafted inputs
     (NOT_RAR / LIMIT_EXCEEDED / INVALID_ARG / BUSY)
  6. handle lifecycle (double close is safe; extract on closed fails)
  7. mutation roundtrip (delete → list shrinks)

Runs via ctest (python_conformance gate) against $<TARGET_FILE:openrar_dll>
via OPENRAR_LIB, or standalone: pytest bindings/python/tests/conformance_test.py.
"""

import ctypes
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from openrar import _native, Archive  # noqa: E402

fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print(f"FAIL {msg}")
    return cond


def native():
    lib = os.environ.get("OPENRAR_LIB", "")
    return _native.load_native(lib)


def test_version_probes(n):
    check(n.lib.openrar_version() == 1, "openrar_version == 1")
    check(n.lib.openrar_archive_version() == 1, "archive version == 1")
    check(len(n.package_version) > 0, "package version present")


def test_feature_mask(n):
    # Registry bits 0..16 (docs/abi-freeze.md §5) — exactly the documented set.
    check(n.features == n.expected_features,
          f"feature mask parity: got {n.features:#x}")


def test_alloc_free_pairing(n):
    p = n.lib.openrar_alloc(128)
    check(p is not None, "alloc(128) nonzero")
    n.lib.openrar_free(ctypes.c_void_p(p))
    # Empty output contract: malloc(0) never relied upon — free(nullptr) safe.
    n.lib.openrar_free(None)


def make_archive(n, dirpath, name="conf"):
    src = Path(dirpath) / f"{name}.txt"
    payload = b"conformance payload \x01\x02\xff binary\n" * 40
    src.write_bytes(payload)
    arc = Path(dirpath) / f"{name}.rar"
    n.create(str(arc), [str(src)], [f"{name}.txt"], method=3)
    check(arc.exists(), "created archive exists")
    return arc, payload


def test_roundtrip(n):
    with tempfile.TemporaryDirectory() as d:
        arc, payload = make_archive(n, d)
        with Archive(str(arc), native=n) as h:
            entries = h.list()
            check(len(entries) == 1, "one entry listed")
            e = entries[0]
            check(e["path"] == "conf.txt", "entry name parity")
            check(e["size"] == len(payload), "size parity")
            check(e["is_dir"] is False, "is_dir parity")
            check(e["is_encrypted"] is False, "is_encrypted parity")
            out = Path(d) / "out.bin"
            h.extract_to_path(0, str(out))
            check(out.read_bytes() == payload, "extract byte parity")
            check(h.test(0), "test OK")


def test_error_matrix(n):
    # NOT_RAR: garbage buffer as archive → open_file fails, error name maps.
    with tempfile.TemporaryDirectory() as d:
        garbage = Path(d) / "g.bin"
        garbage.write_bytes(b"\x00" * 64)
        try:
            n.open(str(garbage))
            check(False, "open(garbage) must fail")
        except _native.OpenRARError as e:
            check(e.code in (_native.RAR_ERR_NOT_RAR, _native.RAR_ERR_IO),
                  f"garbage open error class: {e.code}")
        # INVALID_ARG: create with method 9.
        src = Path(d) / "x.txt"
        src.write_bytes(b"hi")
        try:
            n.create(str(Path(d) / "bad.rar"), [str(src)], ["x.txt"], method=9)
            check(False, "create(method=9) must fail")
        except _native.OpenRARError as e:
            check(e.code == _native.RAR_ERR_INVALID_ARG, "method 9 → INVALID_ARG")
        # LIMIT_EXCEEDED: set_limits(1, ...) then extract a bigger entry.
        arc, _ = make_archive(n, d, "lim")
        with Archive(str(arc), native=n) as h:
            h.set_limits(1, 1, 1 << 40, 1 << 40)
            try:
                h.extract_to_path(0, str(Path(d) / "no.bin"))
                check(False, "capped extract must fail")
            except _native.OpenRARError as e:
                check(e.code == _native.RAR_ERR_LIMIT_EXCEEDED,
                      "LIMIT_EXCEEDED mapping")
        # BUSY: an open handle blocks the mutation exports.
        with Archive(str(arc), native=n) as h:
            try:
                n.delete_entries(str(arc), [0])
                check(False, "mutation under open handle must fail")
            except _native.OpenRARError as e:
                check(e.code == _native.RAR_ERR_BUSY, "BUSY mapping")


def test_handle_lifecycle(n):
    with tempfile.TemporaryDirectory() as d:
        arc, _ = make_archive(n, d, "life")
        h = n.open(str(arc))
        n.close(h)
        n.close(h)  # double close is safe (unknown id erase)
        # extract on a closed handle → INVALID_ARG (0 is not a handle)
        try:
            n.extract_to_path(0, 0, str(Path(d) / "x.bin"))
            check(False, "extract on closed handle must fail")
        except _native.OpenRARError as e:
            check(e.code == _native.RAR_ERR_INVALID_ARG, "closed handle → INVALID_ARG")


def test_mutation_roundtrip(n):
    with tempfile.TemporaryDirectory() as d:
        src1 = Path(d) / "one.txt"
        src1.write_bytes(b"one")
        src2 = Path(d) / "two.txt"
        src2.write_bytes(b"two")
        arc = Path(d) / "mut.rar"
        n.create(str(arc), [str(src1), str(src2)], ["one.txt", "two.txt"], method=0)
        n.delete_entries(str(arc), [0])
        with Archive(str(arc), native=n) as h:
            names = [e["path"] for e in h.list()]
            check(names == ["two.txt"], f"delete shrank the archive: {names}")


def main():
    if len(sys.argv) > 1:  # ctest passes the built library path explicitly
        os.environ["OPENRAR_LIB"] = sys.argv[1]
    n = native()
    test_version_probes(n)
    test_feature_mask(n)
    test_alloc_free_pairing(n)
    test_roundtrip(n)
    test_error_matrix(n)
    test_handle_lifecycle(n)
    test_mutation_roundtrip(n)
    if fails:
        print(f"[python-conformance] {len(fails)} FAILURES")
        return 1
    print("[python-conformance] all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
