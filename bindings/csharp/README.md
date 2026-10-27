# OpenRAR .NET SDK

C# / .NET SDK over the frozen OpenRAR C ABI (`docs/abi-freeze.md`) via P/Invoke.

## Usage

```csharp
using OpenRAR;

// Create archive
Archive.Create("backup.rar", new[] { "data.bin" }, method: 3);

// Read and extract
using (var archive = Archive.Open("backup.rar"))
{
    foreach (var entry in archive.Entries)
    {
        Console.WriteLine($"{entry.Path} ({entry.UnpackedSize} bytes)");
    }
    archive.Extract(0, "data-out.bin");
    bool valid = archive.Test(0);
}
```

## Conformance

Validated against the normative cross-binding test suite in `OpenRAR.Tests` mirroring the Python and WASM conformance contracts.
