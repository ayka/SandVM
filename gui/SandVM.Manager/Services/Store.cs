using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using SandVM.Manager.Models;

namespace SandVM.Manager.Services;

public sealed class AppSettings
{
    // Empty means "find it automatically" (see ToolPaths).
    public string SandvmPath { get; set; } = "";
    public string BiosPath { get; set; } = "";
    public string MachinesFolder { get; set; } = "";

    [JsonIgnore]
    public string EffectiveMachinesFolder => !string.IsNullOrWhiteSpace(MachinesFolder) ? MachinesFolder
        : Store.PortableRoot is { } root ? Path.Combine(root, "machines")
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "SandVM");
}

// Everything the manager remembers, in %APPDATA%\SandVM\manager.json, or in <package>\data\ for a portable
// package (portable.txt next to sandvm.exe), so a copy on a USB stick leaves nothing on the host.
public sealed class StoreData
{
    public AppSettings Settings { get; set; } = new();
    public List<VirtualMachine> Machines { get; set; } = new();
}

public static class Store
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    public const string PortableMarker = "portable.txt";

    // The package folder (parent of SandVM.Manager\) when it carries the portable marker, else null.
    public static readonly string? PortableRoot = FindPortableRoot();

    public static string DataFolder => PortableRoot is { } root
        ? Path.Combine(root, "data")
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SandVM");

    private static string? FindPortableRoot()
    {
        var parent = Path.GetDirectoryName(AppContext.BaseDirectory.TrimEnd('\\'));
        return parent is not null && File.Exists(Path.Combine(parent, PortableMarker)) ? parent : null;
    }

    public static string FilePath => Path.Combine(DataFolder, "manager.json");

    public static StoreData Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var data = JsonSerializer.Deserialize<StoreData>(File.ReadAllText(FilePath), Options) ?? new StoreData();
                if (PortableRoot is { } root) MapPaths(data, p => Path.IsPathRooted(p) ? p : Path.GetFullPath(Path.Combine(root, p)));
                return data;
            }
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            // A damaged file must not lock the user out: keep a copy and start empty.
            try { File.Copy(FilePath, FilePath + ".bozuk", overwrite: true); } catch (IOException) { }
        }
        return new StoreData();
    }

    public static void Save(StoreData data)
    {
        Directory.CreateDirectory(DataFolder);
        // Write then rename, so a crash mid-write cannot destroy the machine list.
        var tmp = FilePath + ".tmp";
        var json = JsonSerializer.Serialize(data, Options);
        if (PortableRoot is { } root)
        {
            // Paths inside the package are stored relative to it: a USB stick gets a new drive letter.
            var copy = JsonSerializer.Deserialize<StoreData>(json, Options)!;
            MapPaths(copy, p =>
            {
                var full = Path.GetFullPath(p);
                return IsUnder(full, root) ? Path.GetRelativePath(root, full) : full;
            });
            json = JsonSerializer.Serialize(copy, Options);
        }
        File.WriteAllText(tmp, json);
        File.Move(tmp, FilePath, overwrite: true);
    }

    private static bool IsUnder(string path, string root)
    {
        if (!Path.IsPathRooted(path)) return false;
        var full = Path.GetFullPath(path);
        var prefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar;
        return full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    private static void MapPaths(StoreData data, Func<string, string> map)
    {
        string Map(string p) => string.IsNullOrWhiteSpace(p) ? p : map(p);
        data.Settings.SandvmPath = Map(data.Settings.SandvmPath);
        data.Settings.BiosPath = Map(data.Settings.BiosPath);
        data.Settings.MachinesFolder = Map(data.Settings.MachinesFolder);
        foreach (var vm in data.Machines)
        {
            if (vm.DiskPath is not null) vm.DiskPath = Map(vm.DiskPath);
            if (vm.CdromPath is not null) vm.CdromPath = Map(vm.CdromPath);
        }
    }
}
