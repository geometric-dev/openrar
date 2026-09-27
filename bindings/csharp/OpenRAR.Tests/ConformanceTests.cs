// OpenRAR.NET conformance suite (v1.30 M4) — the normative cross-binding
// case list (see bindings/python/tests/conformance_test.py for the case
// contract). Runs under xUnit in CI against the freshly built library;
// OPENRAR_TEST_LIB points at the built shared library when it is not on the
// default probe path.

using System;
using System.IO;
using System.Runtime.InteropServices;
using Xunit;

namespace OpenRAR.Tests
{
    public class ConformanceTests
    {
        private static string TempDir()
        {
            var d = Path.Combine(Path.GetTempPath(), "openrar_cs_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(d);
            return d;
        }

        private (string arc, byte[] payload) MakeArchive(string dir)
        {
            var src = Path.Combine(dir, "conf.txt");
            var payload = new byte[2048];
            for (int i = 0; i < payload.Length; i++) payload[i] = (byte)(i * 31 + 7);
            File.WriteAllBytes(src, payload);
            var arc = Path.Combine(dir, "conf.rar");
            Native.CreateFile_(arc, new[] { src }, new[] { "conf.txt" }, 3, 0);
            Assert.True(File.Exists(arc));
            return (arc, payload);
        }

        [Fact]
        public void VersionProbes()
        {
            Native.Probe();
            Assert.False(string.IsNullOrEmpty(Native.PackageVersion));
        }

        [Fact]
        public void RoundtripParity()
        {
            var dir = TempDir();
            try
            {
                var (arc, payload) = MakeArchive(dir);
                using (var a = new Archive(arc))
                {
                    var entries = a.List();
                    Assert.Single(entries);
                    Assert.Equal("conf.txt", entries[0].Path);
                    Assert.Equal((ulong)payload.Length, entries[0].Size);
                    Assert.False(entries[0].IsDir);
                    var outp = Path.Combine(dir, "out.bin");
                    a.ExtractToPath(0, outp);
                    Assert.Equal(payload, File.ReadAllBytes(outp));
                    Assert.True(a.Test(0));
                }
            }
            finally { Directory.Delete(dir, true); }
        }

        [Fact]
        public void ErrorMatrix()
        {
            var dir = TempDir();
            try
            {
                var garbage = Path.Combine(dir, "g.bin");
                File.WriteAllBytes(garbage, new byte[64]);
                var ex = Assert.Throws<OpenRARException>(() => new Archive(garbage));
                Assert.True(ex.Code < 0);

                var src = Path.Combine(dir, "x.txt");
                File.WriteAllBytes(src, new byte[] { 1, 2, 3 });
                Assert.Throws<OpenRARException>(() =>
                    Native.CreateFile_(Path.Combine(dir, "bad.rar"), new[] { src },
                                       new[] { "x.txt" }, 9, 0));

                var (arc, _) = MakeArchive(dir);
                using (var a = new Archive(arc))
                {
                    a.SetLimits(1, 1, ulong.MaxValue, ulong.MaxValue);
                    Assert.Throws<OpenRARException>(() =>
                        a.ExtractToPath(0, Path.Combine(dir, "no.bin")));
                }
            }
            finally { Directory.Delete(dir, true); }
        }

        [Fact]
        public void HandleLifecycle()
        {
            var dir = TempDir();
            try
            {
                var (arc, _) = MakeArchive(dir);
                var h = new Archive(arc);
                h.Dispose();
                h.Dispose(); // double close is safe
                Assert.Throws<OpenRARException>(() => h.ExtractToPath(0, Path.Combine(dir, "x.bin")));
            }
            finally { Directory.Delete(dir, true); }
        }
    }
}
