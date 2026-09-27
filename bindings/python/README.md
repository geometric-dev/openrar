# OpenRAR Python SDK

ctypes binding over the frozen OpenRAR C ABI (`docs/abi-freeze.md`) — no
compiled extension, the C ABI is the whole contract.

```python
from openrar import Archive, create, package_version

create("backup.rar", ["data.bin"], ["data.bin"], method=3)
with Archive("backup.rar") as a:
    for entry in a.list():
        print(entry["path"], entry["size"])
    a.extract_to_path(0, "data-out.bin")
    assert a.test(0)
```

## Conformance

The normative cross-binding case list lives in `tests/conformance_test.py`
(freeze prereq 3). It runs as the `python_conformance` ctest gate against
the freshly built library on every CI leg, and can run standalone:

    OPENRAR_LIB=/path/to/libopenrar.so pytest tests/ -q

The wheel bundles the platform shared library beside the package; the
loader performs the documented startup probes (ABI version strict equality
+ feature-mask negotiation).
