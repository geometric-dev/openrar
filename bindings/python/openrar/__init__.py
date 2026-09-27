"""OpenRAR Python SDK — ctypes binding over the frozen C ABI (v1.30.0).

The binding exposes the file-mode handle family of the frozen surface
(docs/abi-freeze.md): open → list → extract-to-path / test, creation, and
mutation. Additive capability is negotiated at load time via
openrar_abi_features(); errors raise OpenRARError with the documented
RarError codes.
"""

from . import _native
from ._native import (  # noqa: F401
    ArchiveEntry,
    Native,
    OpenRARError,
    RAR_ERR_ABORTED,
    RAR_ERR_BAD_PASSWORD,
    RAR_ERR_BUSY,
    RAR_ERR_CRC_MISMATCH,
    RAR_ERR_ENCRYPTED,
    RAR_ERR_INVALID_ARG,
    RAR_ERR_IO,
    RAR_ERR_LIMIT_EXCEEDED,
    RAR_ERR_MISSING_VOLUME,
    RAR_ERR_NOMEM,
    RAR_ERR_NOT_RAR,
    RAR_ERR_TRUNCATED,
    RAR_ERR_UNSUPPORTED_FEATURE,
    RAR_OK,
    load_native,
)

__version__ = "1.30.0"


class Archive:
    """A file-mode handle (scan-once). Context-manager close semantics."""

    def __init__(self, path: str, password: str = "", native: Native = None):
        self._n = native or load_native()
        self._h = self._n.open(path, password)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def list(self):
        return self._n.list(self._h)

    def extract_to_path(self, index: int, dest: str):
        self._n.extract_to_path(self._h, index, dest)

    def test(self, index: int) -> bool:
        return self._n.test(self._h, index)

    def set_limits(self, member: int, total: int, header_count: int, header_bytes: int):
        self._n.set_limits(self._h, member, total, header_count, header_bytes)

    def close(self):
        if getattr(self, "_h", 0):
            self._n.close(self._h)
            self._h = 0


def create(arc_path: str, src_paths, arc_names, method: int = 3, dict_size: int = 0):
    """Create a RAR 5.0 archive from files on disk (method 0..5)."""
    load_native().create(arc_path, src_paths, arc_names, method, dict_size)


def package_version() -> str:
    return load_native().package_version


def abi_features() -> int:
    return load_native().features
