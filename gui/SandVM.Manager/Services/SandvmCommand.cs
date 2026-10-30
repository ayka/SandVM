using SandVM.Manager.Models;

namespace SandVM.Manager.Services;

// Turns a VirtualMachine into sandvm.exe arguments (see the usage text in src/cli/main.cpp).
public static class SandvmCommand
{
    public static List<string> Build(VirtualMachine vm, ToolPaths paths, string? controlName, string? screenshotPath)
    {
        var args = new List<string>();
        // Global options come before the command.
        if (!string.IsNullOrWhiteSpace(vm.SandboxBox)) args.AddRange(new[] { "--sandbox", vm.SandboxBox.Trim() });
        if (vm.Verbose) args.Add("-v");

        args.Add("run");
        args.AddRange(new[] { "--bios", paths.Bios ?? ToolPaths.BiosFile });
        args.AddRange(new[] { "--ram", vm.RamMiB.ToString() });
        args.AddRange(new[] { "--cpus", vm.Cpus.ToString() });
        // More than one CPU needs the local APIC anyway (sandvm implies it).
        if (vm.Apic || vm.Cpus > 1) args.Add("--apic");
        if (!string.IsNullOrEmpty(vm.DiskPath)) args.AddRange(new[] { "--disk", vm.DiskPath });
        if (!string.IsNullOrEmpty(vm.CdromPath)) args.AddRange(new[] { "--cdrom", vm.CdromPath });
        if (vm.Network)
        {
            args.Add("--net");
            if (TryParseForwards(vm.PortForwards, out var forwards, out _))
            {
                foreach (var (host, guest) in forwards) args.AddRange(new[] { "--hostfwd", $"tcp:{host}:{guest}" });
            }
        }
        if (vm.Display) args.Add("--display");
        if (screenshotPath is not null) args.AddRange(new[] { "--screenshot", screenshotPath });
        if (controlName is not null) args.AddRange(new[] { "--control", controlName });
        return args;
    }

    // For the "copy command line" button: what a user would type in PowerShell.
    public static string ToDisplayString(string exe, IEnumerable<string> args) =>
        string.Join(" ", new[] { "& " + Quote(exe) }.Concat(args.Select(Quote)));

    private static string Quote(string s) =>
        s.Length > 0 && s.All(c => char.IsLetterOrDigit(c) || "-_.:\\/".Contains(c)) ? s : "'" + s.Replace("'", "''") + "'";

    // "2222:22, 8080:80" -> [(2222, 22), (8080, 80)]. Empty input is valid (no forwards).
    public static bool TryParseForwards(string text, out List<(int Host, int Guest)> forwards, out string error)
    {
        forwards = new();
        error = "";
        foreach (var raw in text.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var parts = raw.Split(':');
            if (parts.Length != 2 || !int.TryParse(parts[0], out int host) || !int.TryParse(parts[1], out int guest) ||
                host is < 1 or > 65535 || guest is < 1 or > 65535)
            {
                error = $"'{raw}' geçersiz. Biçim: ana_port:misafir_port (örnek 2222:22)";
                return false;
            }
            if (forwards.Any(f => f.Host == host))
            {
                error = $"Ana bilgisayar portu {host} iki kez kullanılmış.";
                return false;
            }
            forwards.Add((host, guest));
        }
        return true;
    }
}
