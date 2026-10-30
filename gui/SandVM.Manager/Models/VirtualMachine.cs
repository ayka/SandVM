using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;
using System.Windows.Media;

namespace SandVM.Manager.Models;

public enum GuestOs { Linux, Windows, ReactOS, Other }

public enum VmState { Stopped, Starting, Running, ShuttingDown }

// One virtual machine as the user configured it, plus its runtime state (not saved).
public sealed class VirtualMachine : INotifyPropertyChanged
{
    private string _name = "";
    private GuestOs _os = GuestOs.Linux;
    private int _ramMiB = 2048;
    private int _cpus = 2;
    private string? _diskPath;
    private string? _cdromPath;
    private bool _network = true;
    private string _portForwards = "";
    private bool _display = true;
    private bool _apic = true;
    private bool _verbose;
    private string _sandboxBox = "";
    private VmState _state = VmState.Stopped;

    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get => _name; set { if (Set(ref _name, value)) Raise(nameof(Summary)); } }
    public GuestOs Os { get => _os; set { if (Set(ref _os, value)) Raise(nameof(Summary), nameof(OsName), nameof(BadgeLetter), nameof(BadgeBrush)); } }
    public int RamMiB { get => _ramMiB; set { if (Set(ref _ramMiB, value)) Raise(nameof(Summary), nameof(RamText)); } }
    public int Cpus { get => _cpus; set { if (Set(ref _cpus, value)) Raise(nameof(Summary)); } }
    public string? DiskPath { get => _diskPath; set { if (Set(ref _diskPath, value)) Raise(nameof(DiskText)); } }
    // True when the manager created the disk image, so deleting the VM may delete it too.
    public bool OwnsDisk { get; set; }
    public string? CdromPath { get => _cdromPath; set { if (Set(ref _cdromPath, value)) Raise(nameof(CdromText)); } }
    public bool Network { get => _network; set { if (Set(ref _network, value)) Raise(nameof(NetworkText)); } }
    // "2222:22, 8080:80": host port : guest port, TCP, bound to 127.0.0.1.
    public string PortForwards { get => _portForwards; set { if (Set(ref _portForwards, value)) Raise(nameof(NetworkText)); } }
    public bool Display { get => _display; set { if (Set(ref _display, value)) Raise(nameof(DisplayText)); } }
    public bool Apic { get => _apic; set => Set(ref _apic, value); }
    public bool Verbose { get => _verbose; set => Set(ref _verbose, value); }
    public string SandboxBox { get => _sandboxBox; set { if (Set(ref _sandboxBox, value)) Raise(nameof(SandboxText)); } }
    public DateTime Created { get; set; } = DateTime.Now;
    public DateTime? LastStarted { get; set; }

    [JsonIgnore]
    public VmState State
    {
        get => _state;
        set { if (Set(ref _state, value)) Raise(nameof(StateText), nameof(StateBrush), nameof(IsRunning), nameof(IsStopped)); }
    }

    [JsonIgnore] public bool IsRunning => State != VmState.Stopped;
    [JsonIgnore] public bool IsStopped => State == VmState.Stopped;

    [JsonIgnore]
    public string OsName => Os switch
    {
        GuestOs.Linux => "Linux",
        GuestOs.Windows => "Windows",
        GuestOs.ReactOS => "ReactOS",
        _ => "Diğer",
    };

    [JsonIgnore] public string RamText => FormatRam(RamMiB);
    [JsonIgnore] public string Summary => $"{OsName} · {RamText} · {Cpus} CPU";
    [JsonIgnore] public string DiskText => string.IsNullOrEmpty(DiskPath) ? "Yok" : DiskPath;
    [JsonIgnore] public string CdromText => string.IsNullOrEmpty(CdromPath) ? "Boş" : CdromPath;
    [JsonIgnore] public string DisplayText => Display ? "Pencerede göster" : "Yok (yalnızca seri konsol)";
    [JsonIgnore] public string SandboxText => string.IsNullOrWhiteSpace(SandboxBox) ? "Kullanılmıyor" : SandboxBox;

    [JsonIgnore]
    public string NetworkText => !Network
        ? "Kapalı"
        : string.IsNullOrWhiteSpace(PortForwards) ? "NAT (10.0.2.15)" : $"NAT (10.0.2.15), yönlendirme: {PortForwards}";

    [JsonIgnore]
    public string StateText => State switch
    {
        VmState.Starting => "Başlatılıyor",
        VmState.Running => "Çalışıyor",
        VmState.ShuttingDown => "Kapanıyor",
        _ => "Kapalı",
    };

    [JsonIgnore]
    public Brush StateBrush => State switch
    {
        VmState.Running => Palette.Success,
        VmState.Starting or VmState.ShuttingDown => Palette.Warning,
        _ => Palette.Muted,
    };

    [JsonIgnore]
    public string BadgeLetter => Os switch
    {
        GuestOs.Linux => "L",
        GuestOs.Windows => "W",
        GuestOs.ReactOS => "R",
        _ => "?",
    };

    [JsonIgnore]
    public Brush BadgeBrush => Os switch
    {
        GuestOs.Linux => Palette.Linux,
        GuestOs.Windows => Palette.Windows,
        GuestOs.ReactOS => Palette.ReactOs,
        _ => Palette.Muted,
    };

    public static string FormatRam(int mib) => mib >= 1024 ? $"{mib / 1024.0:0.#} GB" : $"{mib} MB";

    // Settings only: MemberwiseClone would also copy the PropertyChanged subscribers.
    public VirtualMachine Clone()
    {
        var copy = new VirtualMachine { Id = Id, Created = Created, LastStarted = LastStarted };
        copy.CopySettingsFrom(this);
        return copy;
    }

    // Copies the saved settings (not the runtime state) from an edited copy.
    public void CopySettingsFrom(VirtualMachine other)
    {
        Name = other.Name;
        Os = other.Os;
        RamMiB = other.RamMiB;
        Cpus = other.Cpus;
        DiskPath = other.DiskPath;
        OwnsDisk = other.OwnsDisk;
        CdromPath = other.CdromPath;
        Network = other.Network;
        PortForwards = other.PortForwards;
        Display = other.Display;
        Apic = other.Apic;
        Verbose = other.Verbose;
        SandboxBox = other.SandboxBox;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool Set<T>(ref T field, T value, [CallerMemberName] string name = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        Raise(name);
        return true;
    }

    private void Raise(params string[] names)
    {
        foreach (var name in names) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}

public static class Palette
{
    public static readonly Brush Success = Frozen(0x16, 0xA3, 0x4A);
    public static readonly Brush Warning = Frozen(0xD9, 0x77, 0x06);
    public static readonly Brush Muted = Frozen(0x9C, 0xA3, 0xAF);
    public static readonly Brush Linux = Frozen(0xE9, 0x54, 0x20);
    public static readonly Brush Windows = Frozen(0x00, 0x78, 0xD4);
    public static readonly Brush ReactOs = Frozen(0x2F, 0x85, 0x5A);

    private static Brush Frozen(byte r, byte g, byte b)
    {
        var brush = new SolidColorBrush(Color.FromRgb(r, g, b));
        brush.Freeze();
        return brush;
    }
}
