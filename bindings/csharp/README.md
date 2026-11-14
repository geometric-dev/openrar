# OpenRAR .NET SDK

P/Invoke binding over the frozen OpenRAR C ABI (`docs/abi-freeze.md`).
Targets `netstandard2.0` — .NET Framework 4.6.1+, .NET Core 2.0+ and .NET 5+.

## Native library

The package contains **managed code only**; the native library must be loadable
as `openrar`:

| Platform | File | Where |
|---|---|---|
| Windows | `openrar.dll` | beside the application, or on `PATH` |
| Linux | `libopenrar.so` | beside the application, or in the loader path |
| macOS | `libopenrar.dylib` | beside the application, or in the loader path |

The release zips (`openrar-<version>-*.zip`) carry the library next to the CLI,
so copying that folder beside your binary is all that is needed. Windows zips
ship both `openrar_x64.dll` (side-by-side name) and the canonical
`openrar.dll` that `DllImport("openrar")` resolves.

## Startup probe

```csharp
using OpenRAR;

Native.Probe();                            // throws on ABI mismatch
Console.WriteLine(Native.PackageVersion);  // e.g. "2.4.0"
Console.WriteLine($"features=0x{Native.Features:x}");
```

`Probe()` runs the spec §3 startup checks: strict equality against the C ABI
version and the `openrar_abi_features()` negotiation. Call it once at startup.

## Read, verify and extract

```csharp
using System;
using System.IO;
using OpenRAR;

Native.Probe();

using var archive = new Archive(@"C:\backups\data.rar");

// bytes per member, bytes total, header count, header bytes
archive.SetLimits(member: 256UL << 20, total: 4UL << 30,
                  headerCount: 100_000, headerBytes: 8UL << 20);

var entries = archive.List();
for (uint i = 0; i < entries.Length; i++)
{
    var entry = entries[i];
    Console.WriteLine($"[{i}] {entry.Path} {entry.Size} bytes method={entry.Method} crc32={entry.Crc32:x8}");

    if (!archive.Test(i))                       // CRC32 / BLAKE2sp verification
        throw new InvalidOperationException($"entry {i} failed verification");

    archive.ExtractToPath(i, Path.Combine(outDir, $"{i:D4}.bin"));
}
```

`Archive` implements `IDisposable`; `using` closes the handle. `OpenRARException`
carries `Code` (the documented RarError value) and `Name` (`"CRC_MISMATCH"`,
`"ENCRYPTED"`, …) plus the library's thread-local message.

## What the binding does not expose

Creation and mutation live in the C ABI (`openrar_archive_create_file` and the
mutation family) and are exercised from `OpenRAR.Tests` through the internal
`Native.CreateFile_` helper — there is no public wrapper yet. Use the CLI for
those (`openrar a`, `openrar u`, `openrar d`).

## Conformance

Validated against the normative cross-binding suite in `OpenRAR.Tests`, which
mirrors the Python and WASM conformance contracts.
