"""openrar._native — ctypes binding over the frozen OpenRAR C ABI.

The C ABI (include/openrar/openrar_dll.h, docs/abi-freeze.md) is the whole
contract: ctypes needs no compiled extension, and the loader performs the
documented startup probes (openrar_version strict equality +
openrar_abi_features() negotiation, docs/dll-integration-spec.md §3).

Library discovery order (the wheel bundles the shared library beside this
package; source builds find it via OPENRAR_LIB or the build tree):
  1. OPENRAR_LIB environment variable (absolute path to the library)
  2. beside this package (the wheel layout)
  3. platform system search paths
"""

import ctypes
import os
import sys
from pathlib import Path

__all__ = ["RarError", "OpenRARError", "ArchiveEntry", "Native", "load_native"]

# ── RarError mirror (parity-gated: tools/check_js_error_mirror.py pins the JS
#    mirror; the conformance suite pins THIS one against the C enum) ─────────
RAR_OK = 0
RAR_ERR_PARTIAL_OK = 1
RAR_ERR_NOT_RAR = -1
RAR_ERR_UNSUPPORTED_FEATURE = -2
RAR_ERR_TRUNCATED = -3
RAR_ERR_CRC_MISMATCH = -4
RAR_ERR_NOMEM = -5
RAR_ERR_IO = -6
RAR_ERR_BAD_PASSWORD = -7
RAR_ERR_INVALID_ARG = -9
RAR_ERR_ABORTED = -11
RAR_ERR_ENCRYPTED = -12
RAR_ERR_MISSING_VOLUME = -13
RAR_ERR_BUSY = -14
RAR_ERR_LIMIT_EXCEEDED = -15

# Error-name mirror (CODE_MAP parity, minus OK/PARTIAL_OK which are never
# raised as errors).
_ERRORS = {
    RAR_ERR_NOT_RAR: "NOT_RAR",
    RAR_ERR_UNSUPPORTED_FEATURE: "UNSUPPORTED_FEATURE",
    RAR_ERR_TRUNCATED: "TRUNCATED",
    RAR_ERR_CRC_MISMATCH: "CRC_MISMATCH",
    RAR_ERR_NOMEM: "NOMEM",
    RAR_ERR_IO: "IO",
    RAR_ERR_BAD_PASSWORD: "BAD_PASSWORD",
    RAR_ERR_INVALID_ARG: "INVALID_ARG",
    RAR_ERR_ABORTED: "ABORTED",
    RAR_ERR_ENCRYPTED: "ENCRYPTED",
    RAR_ERR_MISSING_VOLUME: "MISSING_VOLUME",
    RAR_ERR_BUSY: "BUSY",
    RAR_ERR_LIMIT_EXCEEDED: "LIMIT_EXCEEDED",
}

_DLL_API_VERSION = 1


class OpenRARError(Exception):
    """Raised for a nonzero RarError return. .code is the numeric value,
    .name the documented error name, .detail the thread-local message."""

    def __init__(self, code: int, detail: str = ""):
        self.code = code
        self.name = _ERRORS.get(code, f"UNKNOWN_{code}")
        self.detail = detail or ""
        super().__init__(f"{self.name}: {self.detail}" if self.detail else self.name)


class ArchiveEntry(ctypes.Structure):
    """The frozen 64-byte openrar_archive_entry_t (docs/abi-freeze.md §3)."""

    _fields_ = [
        ("path_offset", ctypes.c_uint32),
        ("path_len", ctypes.c_uint32),
        ("is_dir", ctypes.c_uint32),
        ("method", ctypes.c_uint32),
        ("is_encrypted", ctypes.c_uint32),
        ("crc32", ctypes.c_uint32),
        ("size", ctypes.c_uint64),
        ("packed_size", ctypes.c_uint64),
        ("mtime", ctypes.c_uint64),
        ("_pad", ctypes.c_uint64 * 2),
    ]


assert ctypes.sizeof(ArchiveEntry) == 64, "entry layout drifted from abi-freeze.md"


def _candidate_paths() -> list:
    here = Path(__file__).resolve().parent
    names = {
        "win32": ["openrar.dll"],
        "darwin": ["libopenrar.dylib"],
    }.get(sys.platform, ["libopenrar.so"])
    out = []
    env = os.environ.get("OPENRAR_LIB")
    if env:
        out.append(Path(env))
    for n in names:
        out.append(here / n)
        out.append(here.parent / n)
    return out


class Native:
    """Loaded C ABI + the startup probes. Construct once per process."""

    def __init__(self, path: str = ""):
        candidates = [Path(path)] if path else _candidate_paths()
        self.lib = None
        tried = []
        for c in candidates:
            if c.exists():
                try:
                    self.lib = ctypes.CDLL(str(c))
                    self.path = str(c)
                    break
                except OSError as e:  # pragma: no cover - load failure details
                    tried.append(f"{c}: {e}")
            else:
                tried.append(str(c))
        if self.lib is None:
            raise OpenRARError(RAR_ERR_NOT_RAR, "OpenRAR shared library not found: " + "; ".join(tried))
        self._declare()
        self._probe()

    def _declare(self):
        lib = self.lib
        lib.openrar_version.restype = ctypes.c_int
        lib.openrar_archive_version.restype = ctypes.c_int
        lib.openrar_package_version_string.restype = ctypes.c_char_p
        lib.openrar_abi_features.restype = ctypes.c_uint64
        lib.openrar_alloc.restype = ctypes.c_void_p
        lib.openrar_alloc.argtypes = [ctypes.c_size_t]
        lib.openrar_free.restype = None
        lib.openrar_free.argtypes = [ctypes.c_void_p]
        lib.openrar_archive_get_error.restype = ctypes.c_int
        lib.openrar_archive_get_error.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.openrar_archive_open_file.restype = ctypes.c_uint32
        lib.openrar_archive_open_file.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        lib.openrar_archive_close.restype = None
        lib.close = lib.openrar_archive_close
        lib.close.argtypes = [ctypes.c_uint32]
        lib.openrar_archive_handle_list.restype = ctypes.c_int
        lib.openrar_archive_handle_list.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.openrar_archive_handle_extract_to_path.restype = ctypes.c_int
        lib.openrar_archive_handle_extract_to_path.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        lib.openrar_archive_handle_test.restype = ctypes.c_int
        lib.openrar_archive_handle_test.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        lib.openrar_archive_handle_set_limits.restype = ctypes.c_int
        lib.openrar_archive_handle_set_limits.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
        ]
        lib.openrar_archive_create_file.restype = ctypes.c_int
        lib.openrar_archive_create_file.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.c_uint64,
        ]
        lib.openrar_archive_delete_entries_file.restype = ctypes.c_int
        lib.openrar_archive_delete_entries_file.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
        ]
        lib.openrar_last_error.restype = ctypes.c_int
        lib.openrar_last_error.argtypes = [ctypes.c_char_p, ctypes.c_int]

    def _probe(self):
        # docs/dll-integration-spec.md §3: strict-equality probes at startup.
        v = self.lib.openrar_version()
        if v != _DLL_API_VERSION:
            raise OpenRARError(RAR_ERR_UNSUPPORTED_FEATURE,
                               f"ABI version mismatch: host={_DLL_API_VERSION} library={v}")
        av = self.lib.openrar_archive_version()
        if av != 1:
            raise OpenRARError(RAR_ERR_UNSUPPORTED_FEATURE, f"archive version {av}")
        self.package_version = self.lib.openrar_package_version_string().decode("utf-8")
        self.features = self.lib.openrar_abi_features()
        # Registered registry bits (docs/abi-freeze.md §5): 0..16 all set.
        self.expected_features = (1 << 17) - 1

    # ── helpers ───────────────────────────────────────────────────────────
    def last_error(self) -> str:
        buf = ctypes.create_string_buffer(1024)
        n = self.lib.openrar_archive_get_error(buf, 1024)
        return buf.value.decode("utf-8", "replace") if n > 0 else ""

    def check(self, rc: int, ok_codes=(RAR_OK,)):
        if rc not in ok_codes:
            raise OpenRARError(rc, self.last_error())

    def open(self, path: str, password: str = "") -> int:
        h = self.lib.openrar_archive_open_file(
            os.fspath(path).encode("utf-8"),
            password.encode("utf-8") if password else None,
            None, None, None,
        )
        if h == 0:
            raise OpenRARError(RAR_ERR_IO, self.last_error())
        return h

    def list(self, handle: int):
        count = ctypes.c_uint32(0)
        entries = ctypes.c_void_p()
        paths = ctypes.c_void_p()
        paths_size = ctypes.c_size_t(0)
        rc = self.lib.openrar_archive_handle_list(
            handle, ctypes.byref(count), ctypes.byref(entries), ctypes.byref(paths),
            ctypes.byref(paths_size),
        )
        self.check(rc)
        try:
            raw = ctypes.string_at(entries, count.value * ctypes.sizeof(ArchiveEntry))
            blob = ctypes.string_at(paths, paths_size.value) if paths.value else b""
            out = []
            for i in range(count.value):
                e = ArchiveEntry.from_buffer_copy(raw, i * ctypes.sizeof(ArchiveEntry))
                path = blob[e.path_offset:e.path_offset + e.path_len].decode("utf-8", "replace")
                out.append({
                    "path": path,
                    "is_dir": bool(e.is_dir),
                    "method": e.method,
                    "is_encrypted": bool(e.is_encrypted),
                    "crc32": e.crc32,
                    "size": e.size,
                    "packed_size": e.packed_size,
                    "mtime": e.mtime,
                })
            return out
        finally:
            self.lib.openrar_archive_list_free(entries, paths, paths_size)

    def extract_to_path(self, handle: int, index: int, dest: str):
        self.check(self.lib.openrar_archive_handle_extract_to_path(
            handle, index, os.fspath(dest).encode("utf-8"), None, None, None))

    def test(self, handle: int, index: int) -> bool:
        return self.lib.openrar_archive_handle_test(handle, index, None, None, None) == RAR_OK

    def set_limits(self, handle: int, member: int, total: int, hdr_count: int, hdr_bytes: int):
        self.check(self.lib.openrar_archive_handle_set_limits(
            handle, member, total, hdr_count, hdr_bytes))

    def close(self, handle: int):
        self.lib.openrar_archive_close(handle)

    def create(self, arc_path: str, src_paths, arc_names, method: int = 3, dict_size: int = 0):
        n = len(src_paths)
        src_arr = (ctypes.c_char_p * n)(*(os.fspath(p).encode("utf-8") for p in src_paths))
        name_arr = (ctypes.c_char_p * n)(*(nm.encode("utf-8") for nm in arc_names))
        self.check(self.lib.openrar_archive_create_file(
            os.fspath(arc_path).encode("utf-8"), src_arr, name_arr, n, method, dict_size))

    def delete_entries(self, arc_path: str, indices):
        n = len(indices)
        arr = (ctypes.c_uint32 * n)(*indices)
        self.check(self.lib.openrar_archive_delete_entries_file(
            os.fspath(arc_path).encode("utf-8"), arr, n))


_cached = None


def load_native(path: str = "") -> Native:
    global _cached
    if _cached is None or path:
        _cached = Native(path)
    return _cached
