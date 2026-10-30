using System.IO;

namespace SandVM.Manager.Services;

// Finds sandvm.exe and the SeaBIOS image. Works both from a packaged folder (everything next to the
// manager) and from the source tree (gui/SandVM.Manager/bin/... with build/ and firmware/ further up).
public sealed record ToolPaths(string? Sandvm, string? Bios)
{
    public const string SandvmExe = "sandvm.exe";
    public const string BiosFile = "bios-256k.bin";

    public bool IsComplete => Sandvm is not null && Bios is not null;

    public static ToolPaths Resolve(AppSettings settings)
    {
        string? sandvm = Existing(settings.SandvmPath) ?? Search(SandvmExe, Path.Combine("build", SandvmExe));
        string? bios = Existing(settings.BiosPath)
                       ?? (sandvm is null ? null : NearExe(sandvm))
                       ?? Search(Path.Combine("firmware", BiosFile), BiosFile);
        return new ToolPaths(sandvm, bios);
    }

    private static string? Existing(string path) =>
        !string.IsNullOrWhiteSpace(path) && File.Exists(path) ? Path.GetFullPath(path) : null;

    // sandvm.exe in X\ or X\build\ -> X\firmware\bios-256k.bin.
    private static string? NearExe(string sandvm)
    {
        var dir = Path.GetDirectoryName(sandvm);
        for (int i = 0; i < 2 && dir is not null; i++, dir = Path.GetDirectoryName(dir))
        {
            var candidate = Path.Combine(dir, "firmware", BiosFile);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static string? Search(params string[] relatives)
    {
        var dir = AppContext.BaseDirectory;
        for (int depth = 0; depth < 8 && dir is not null; depth++, dir = Path.GetDirectoryName(dir.TrimEnd('\\')))
        {
            foreach (var rel in relatives)
            {
                var candidate = Path.Combine(dir, rel);
                if (File.Exists(candidate)) return Path.GetFullPath(candidate);
            }
        }
        return null;
    }
}
