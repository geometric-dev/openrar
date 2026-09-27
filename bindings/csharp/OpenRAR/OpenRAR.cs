// OpenRAR.NET — P/Invoke binding over the frozen OpenRAR C ABI (v1.30.0).
// The C ABI (docs/abi-freeze.md) is the whole contract; this wrapper mirrors
// the file-mode handle family and the documented startup probes.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace OpenRAR
{
    /// <summary>Raised for a nonzero RarError return. Code is the numeric
    /// value, Name the documented error name, Detail the thread-local
    /// message.</summary>
    public class OpenRARException : Exception
    {
        public int Code { get; }
        public string Name { get; }

        public OpenRARException(int code, string detail)
            : base($"{Name_(code)}{(string.IsNullOrEmpty(detail) ? "" : ": " + detail)}")
        {
            Code = code;
            Name = Name_(code);
        }

        private static string Name_(int code) => code switch
        {
            0 => "RAR_OK",
            1 => "RAR_ERR_PARTIAL_OK",
            -1 => "NOT_RAR",
            -2 => "UNSUPPORTED_FEATURE",
            -3 => "TRUNCATED",
            -4 => "CRC_MISMATCH",
            -5 => "NOMEM",
            -6 => "IO",
            -7 => "BAD_PASSWORD",
            -9 => "INVALID_ARG",
            -11 => "ABORTED",
            -12 => "ENCRYPTED",
            -13 => "MISSING_VOLUME",
            -14 => "BUSY",
            -15 => "LIMIT_EXCEEDED",
            _ => $"UNKNOWN_{code}",
        };
    }

    /// <summary>The frozen 64-byte openrar_archive_entry_t.</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct ArchiveEntry
    {
        public uint PathOffset;
        public uint PathLen;
        public uint IsDir;
        public uint Method;
        public uint IsEncrypted;
        public uint Crc32;
        public ulong Size;
        public ulong PackedSize;
        public ulong Mtime;
        public ulong Pad0;
        public ulong Pad1;
    }

    /// <summary>A file-mode handle (scan-once). IDisposable.</summary>
    public sealed class Archive : IDisposable
    {
        private uint _handle;

        public Archive(string path, string password = null)
        {
            _handle = Native.OpenFile(path, password);
            if (_handle == 0)
                throw new OpenRARException(Native.RarErrIo, Native.LastError());
        }

        public EntryInfo[] List()
        {
            uint count = 0;
            IntPtr entries = IntPtr.Zero, paths = IntPtr.Zero;
            UIntPtr pathsSize = UIntPtr.Zero;
            Native.Check(Native.HandleList(_handle, ref count, ref entries, ref paths, ref pathsSize));
            try
            {
                var outEntries = new EntryInfo[count];
                byte[] raw = new byte[count * 64];
                Marshal.Copy(entries, raw, 0, raw.Length);
                byte[] pathBlob = new byte[(long)pathsSize];
                if (paths != IntPtr.Zero) Marshal.Copy(paths, pathBlob, 0, pathBlob.Length);
                for (uint i = 0; i < count; i++)
                {
                    var e = BytesToEntry(raw, i * 64);
                    string name = Encoding.UTF8.GetString(
                        pathBlob, (int)e.PathOffset, (int)e.PathLen);
                    outEntries[i] = new EntryInfo
                    {
                        Path = name,
                        IsDir = e.IsDir != 0,
                        Method = e.Method,
                        IsEncrypted = e.IsEncrypted != 0,
                        Crc32 = e.Crc32,
                        Size = e.Size,
                        PackedSize = e.PackedSize,
                        Mtime = e.Mtime,
                    };
                }
                return outEntries;
            }
            finally
            {
                Native.ListFree(entries, paths, pathsSize);
            }
        }

        public void ExtractToPath(uint index, string dest) =>
            Native.Check(Native.HandleExtractToPath(_handle, index, dest, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero));

        public bool Test(uint index) =>
            Native.HandleTest(_handle, index, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero) == 0;

        public void SetLimits(ulong member, ulong total, ulong headerCount, ulong headerBytes) =>
            Native.Check(Native.HandleSetLimits(_handle, member, total, headerCount, headerBytes));

        public void Dispose()
        {
            if (_handle != 0)
            {
                Native.Close(_handle);
                _handle = 0;
            }
            GC.SuppressFinalize(this);
        }

        ~Archive() => Dispose();

        private static ArchiveEntry BytesToEntry(byte[] raw, int off)
        {
            var e = new ArchiveEntry();
            e.PathOffset = BitConverter.ToUInt32(raw, off);
            e.PathLen = BitConverter.ToUInt32(raw, off + 4);
            e.IsDir = BitConverter.ToUInt32(raw, off + 8);
            e.Method = BitConverter.ToUInt32(raw, off + 12);
            e.IsEncrypted = BitConverter.ToUInt32(raw, off + 16);
            e.Crc32 = BitConverter.ToUInt32(raw, off + 20);
            e.Size = BitConverter.ToUInt64(raw, off + 24);
            e.PackedSize = BitConverter.ToUInt64(raw, off + 32);
            e.Mtime = BitConverter.ToUInt64(raw, off + 40);
            return e;
        }
    }

    public class EntryInfo
    {
        public string Path;
        public bool IsDir;
        public uint Method;
        public bool IsEncrypted;
        public uint Crc32;
        public ulong Size;
        public ulong PackedSize;
        public ulong Mtime;
    }

    public static class Native
    {
        public const int DllApiVersion = 1;
        public const int RarErrInvalidArg = -9;
        public const int RarErrIo = -6;

        private const string Lib = "openrar";

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_version();
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern ulong openrar_abi_features();
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr openrar_package_version_string();
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_get_error(StringBuilder buf, int len);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint openrar_archive_open_file(
            string path, string password, IntPtr progress, IntPtr cancel, IntPtr user);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern void openrar_archive_close(uint handle);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_handle_list(
            uint handle, ref uint count, ref IntPtr entries, ref IntPtr paths, ref UIntPtr pathsSize);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern void openrar_archive_list_free(
            IntPtr entries, IntPtr paths, UIntPtr pathsSize);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_handle_extract_to_path(
            uint handle, uint index, string dest, IntPtr progress, IntPtr cancel, IntPtr user);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_handle_test(
            uint handle, uint index, IntPtr progress, IntPtr cancel, IntPtr user);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_handle_set_limits(
            uint handle, ulong member, ulong total, ulong headerCount, ulong headerBytes);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int openrar_archive_create_file(
            string arcPath, string[] srcPaths, string[] arcNames, uint fileCount,
            int method, ulong dictSize);

        internal static int HandleList(uint h, ref uint c, ref IntPtr e, ref IntPtr p, ref UIntPtr s) =>
            openrar_archive_handle_list(h, ref c, ref e, ref p, ref s);
        internal static void ListFree(IntPtr e, IntPtr p, UIntPtr s) => openrar_archive_list_free(e, p, s);
        internal static int HandleExtractToPath(uint h, uint i, string d, IntPtr pr, IntPtr ca, IntPtr us) =>
            openrar_archive_handle_extract_to_path(h, i, d, pr, ca, us);
        internal static int HandleTest(uint h, uint i, IntPtr pr, IntPtr ca, IntPtr us) =>
            openrar_archive_handle_test(h, i, pr, ca, us);
        internal static int HandleSetLimits(uint h, ulong m, ulong t, ulong hc, ulong hb) =>
            openrar_archive_handle_set_limits(h, m, t, hc, hb);
        internal static uint OpenFile(string path, string password) =>
            openrar_archive_open_file(path, password ?? "", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        internal static void Close(uint h) => openrar_archive_close(h);
        internal static void CreateFile_(string arc, string[] src, string[] names, int method, ulong dict) =>
            Check(openrar_archive_create_file(arc, src, names, (uint)src.Length, method, dict));

        /// <summary>Spec §3 startup probes: strict-equality version check +
        /// the feature mask. Throws OpenRARException on mismatch.</summary>
        public static void Probe()
        {
            int v = openrar_version();
            if (v != DllApiVersion)
                throw new OpenRARException(-2, $"ABI version mismatch: host={DllApiVersion} library={v}");
            PackageVersion = Marshal.PtrToStringAnsi(openrar_package_version_string()) ?? "";
            Features = openrar_abi_features();
        }

        public static string PackageVersion { get; private set; } = "";
        public static ulong Features { get; private set; }

        internal static void Check(int rc)
        {
            if (rc != 0) throw new OpenRARException(rc, LastError());
        }

        internal static string LastError()
        {
            var buf = new StringBuilder(1024);
            int n = openrar_archive_get_error(buf, 1024);
            return n > 0 ? buf.ToString(0, n) : "";
        }
    }
}
