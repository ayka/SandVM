using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace SandVM.Manager.Services;

public static class DiskImage
{
    private const uint FsctlSetSparse = 0x000900C4;

    // Creates a raw disk image of `bytes` that takes no space until the guest writes to it. Without the
    // sparse flag NTFS would zero-fill everything up to the first write far into the file.
    public static void CreateSparse(string path, long bytes)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var fs = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
        // Not fatal (e.g. FAT32/exFAT): the image still works, it just is not sparse.
        DeviceIoControl(fs.SafeFileHandle, FsctlSetSparse, IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero);
        fs.SetLength(bytes);
    }

    // Space the image really takes on disk (sparse files are smaller than their length).
    public static long AllocatedBytes(string path)
    {
        uint low = GetCompressedFileSizeW(path, out uint high);
        if (low == uint.MaxValue)
        {
            int err = Marshal.GetLastWin32Error();
            if (err != 0) throw new Win32Exception(err);
        }
        return ((long)high << 32) | low;
    }

    public static string FormatBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return unit == 0 ? $"{bytes} B" : $"{value:0.#} {units[unit]}";
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr inBuffer, uint inSize,
                                               IntPtr outBuffer, uint outSize, out uint returned, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetCompressedFileSizeW(string fileName, out uint fileSizeHigh);
}
