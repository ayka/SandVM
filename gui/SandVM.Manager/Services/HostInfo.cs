using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace SandVM.Manager.Services;

public static class HostInfo
{
    // sandvm's own limits: 15 vCPUs (4-bit IOAPIC ID after the last CPU) and 63 GiB of RAM.
    public const int MaxCpus = 15;
    public const int MaxRamMiB = 64512;

    public static int LogicalProcessors => Environment.ProcessorCount;

    public static long TotalRamMiB
    {
        get
        {
            var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
            return GlobalMemoryStatusEx(ref status) ? (long)(status.TotalPhys / (1024 * 1024)) : 4096;
        }
    }

    // Guest RAM is committed up front, so leave the host some room.
    public static int SuggestedMaxRamMiB => (int)Math.Clamp(TotalRamMiB - 2048, 512, MaxRamMiB);

    public sealed record HypervisorStatus(bool Ok, string Text);

    // Runs "sandvm caps": exit code 0 and "hypervisor present : yes" mean WHPX is usable.
    public static async Task<HypervisorStatus> CheckHypervisorAsync(string sandvm)
    {
        try
        {
            var psi = new ProcessStartInfo(sandvm, "caps")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = Path.GetDirectoryName(sandvm)!,
            };
            using var p = Process.Start(psi)!;
            var output = p.StandardOutput.ReadToEndAsync();
            var error = p.StandardError.ReadToEndAsync();
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await p.WaitForExitAsync(cts.Token);
            var text = (await output) + (await error);
            if (p.ExitCode == 0 && text.Contains("hypervisor present : yes"))
                return new HypervisorStatus(true, "Hipervizör hazır");
            return new HypervisorStatus(false,
                "Hipervizör kullanılamıyor: Windows özelliklerinden 'Windows Hypervisor Platform'u açıp yeniden başlatın.");
        }
        catch (OperationCanceledException)
        {
            return new HypervisorStatus(false, "Hipervizör denetimi zaman aşımına uğradı.");
        }
        catch (Exception ex)
        {
            return new HypervisorStatus(false, "sandvm.exe çalıştırılamadı: " + ex.Message);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length;
        public uint MemoryLoad;
        public ulong TotalPhys;
        public ulong AvailPhys;
        public ulong TotalPageFile;
        public ulong AvailPageFile;
        public ulong TotalVirtual;
        public ulong AvailVirtual;
        public ulong AvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);
}
