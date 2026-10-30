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
    public string EffectiveMachinesFolder => string.IsNullOrWhiteSpace(MachinesFolder)
        ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "SandVM")
        : MachinesFolder;
}

// Everything the manager remembers, in %APPDATA%\SandVM\manager.json.
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

    public static string DataFolder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SandVM");

    public static string FilePath => Path.Combine(DataFolder, "manager.json");

    public static StoreData Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<StoreData>(File.ReadAllText(FilePath), Options) ?? new StoreData();
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
        File.WriteAllText(tmp, JsonSerializer.Serialize(data, Options));
        File.Move(tmp, FilePath, overwrite: true);
    }
}
