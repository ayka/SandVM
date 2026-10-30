using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using SandVM.Manager.Models;
using SandVM.Manager.Services;

namespace SandVM.Manager.Views;

// Creates a new machine (isNew) or edits a copy of an existing one. Result holds the outcome; for a new
// machine with "new disk" the image file has already been created when the dialog closes with OK.
public partial class VmEditorWindow : Window
{
    private readonly bool _isNew;
    private readonly string _machinesFolder;
    private readonly Func<string, VirtualMachine?, bool> _nameTaken;
    private readonly VirtualMachine? _original;
    private bool _loading = true;
    private bool _nameEdited;

    public VirtualMachine? Result { get; private set; }

    public VmEditorWindow(VirtualMachine? source, bool isNew, string machinesFolder, Func<string, VirtualMachine?, bool> nameTaken)
    {
        InitializeComponent();
        _isNew = isNew;
        _machinesFolder = machinesFolder;
        _nameTaken = nameTaken;
        _original = isNew ? null : source;
        Title = isNew ? "Yeni sanal makine" : $"Ayarlar: {source?.Name}";
        OkButton.Content = isNew ? "Oluştur" : "Kaydet";

        RamSlider.Maximum = Math.Max(1024, Math.Min(HostInfo.MaxRamMiB, (HostInfo.TotalRamMiB - 1024) / 256 * 256));
        CpuSlider.Maximum = Math.Max(1, Math.Min(HostInfo.MaxCpus, HostInfo.LogicalProcessors));

        var vm = source?.Clone() ?? new VirtualMachine { Name = "" };
        NameBox.Text = vm.Name;
        _nameEdited = !string.IsNullOrEmpty(vm.Name);
        OsBox.SelectedIndex = (int)vm.Os;
        IsoBox.Text = vm.CdromPath ?? "";
        RamSlider.Value = Math.Clamp(vm.RamMiB, (int)RamSlider.Minimum, (int)RamSlider.Maximum);
        CpuSlider.Value = Math.Clamp(vm.Cpus, 1, (int)CpuSlider.Maximum);
        DiskSizeSlider.Value = DefaultDiskGb(vm.Os);
        NetworkCheck.IsChecked = vm.Network;
        ForwardBox.Text = vm.PortForwards;
        DisplayCheck.IsChecked = vm.Display;
        ApicCheck.IsChecked = vm.Apic;
        VerboseCheck.IsChecked = vm.Verbose;
        SandboxBox.Text = vm.SandboxBox;

        if (!string.IsNullOrEmpty(vm.DiskPath))
        {
            DiskBox.Text = vm.DiskPath;
            ExistingDiskRadio.IsChecked = true;
        }
        else if (isNew)
        {
            NewDiskRadio.IsChecked = true;
            if (source is null) ApplyOsDefaults(vm.Os);
        }
        else
        {
            NoDiskRadio.IsChecked = true;
        }

        NameBox.TextChanged += (_, _) =>
        {
            if (!_loading) _nameEdited = true;
            UpdateNewDiskPath();
        };
        _loading = false;
        UpdateLabels();
        UpdateNewDiskPath();
        OnDiskModeChanged(this, new RoutedEventArgs());
        OnNetworkChanged(this, new RoutedEventArgs());
        Loaded += (_, _) => NameBox.Focus();
    }

    private GuestOs SelectedOs => (GuestOs)Math.Max(0, OsBox.SelectedIndex);

    private static int DefaultDiskGb(GuestOs os) => os switch
    {
        GuestOs.Linux => 30,
        GuestOs.Windows => 40,
        GuestOs.ReactOS => 8,
        _ => 10,
    };

    // Sensible starting points; the user can change all of them.
    private void ApplyOsDefaults(GuestOs os)
    {
        int ram = os switch
        {
            GuestOs.Linux => 4096,
            GuestOs.Windows => 2048,
            _ => 1024,
        };
        int cpus = os switch
        {
            GuestOs.Linux => 4,
            GuestOs.Windows => 2,
            _ => 1,
        };
        RamSlider.Value = Math.Min(ram, HostInfo.SuggestedMaxRamMiB);
        CpuSlider.Value = Math.Min(cpus, CpuSlider.Maximum);
        DiskSizeSlider.Value = DefaultDiskGb(os);
    }

    private void OnOsChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loading && _isNew) ApplyOsDefaults(SelectedOs);
    }

    private void OnRamChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateLabels();
    private void OnCpuChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateLabels();
    private void OnDiskSizeChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateLabels();

    private void UpdateLabels()
    {
        if (RamText is null || CpuText is null || DiskSizeText is null) return;  // during InitializeComponent
        int ram = (int)RamSlider.Value;
        RamText.Text = VirtualMachine.FormatRam(ram);
        RamHint.Text = ram > HostInfo.SuggestedMaxRamMiB
            ? $"Uyarı: ana bilgisayarda yalnızca {VirtualMachine.FormatRam((int)HostInfo.TotalRamMiB)} bellek var; Windows yavaşlayabilir."
            : $"Ana bilgisayarın belleği: {VirtualMachine.FormatRam((int)HostInfo.TotalRamMiB)}. Bellek makine çalışırken ayrılır.";
        CpuText.Text = $"{(int)CpuSlider.Value} çekirdek";
        DiskSizeText.Text = $"{(int)DiskSizeSlider.Value} GB";
    }

    private static string SafeFolderName(string name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var safe = new string(name.Trim().Select(c => invalid.Contains(c) ? '_' : c).ToArray()).TrimEnd('.', ' ');
        return safe.Length == 0 ? "Sanal makine" : safe;
    }

    private string NewDiskPath => Path.Combine(_machinesFolder, SafeFolderName(NameBox.Text), "disk.img");

    private void UpdateNewDiskPath()
    {
        if (NewDiskPathText is not null) NewDiskPathText.Text = "Konum: " + NewDiskPath;
    }

    private void OnDiskModeChanged(object sender, RoutedEventArgs e)
    {
        if (NewDiskPanel is null || ExistingDiskPanel is null) return;
        NewDiskPanel.IsEnabled = NewDiskRadio.IsChecked == true;
        ExistingDiskPanel.IsEnabled = ExistingDiskRadio.IsChecked == true;
    }

    private void OnNetworkChanged(object sender, RoutedEventArgs e)
    {
        if (ForwardPanel is not null) ForwardPanel.IsEnabled = NetworkCheck.IsChecked == true;
    }

    private void OnBrowseIso(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Disk kalıbı (*.iso)|*.iso|Tüm dosyalar|*.*", Title = "Kurulum ISO'sunu seçin" };
        if (dialog.ShowDialog(this) == true) IsoBox.Text = dialog.FileName;
    }

    private void OnClearIso(object sender, RoutedEventArgs e) => IsoBox.Text = "";

    private void OnBrowseDisk(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Ham disk kalıbı (*.img;*.raw)|*.img;*.raw|Tüm dosyalar|*.*", Title = "Disk dosyasını seçin" };
        if (dialog.ShowDialog(this) != true) return;
        DiskBox.Text = dialog.FileName;
        ExistingDiskRadio.IsChecked = true;
    }

    // Picking an ISO suggests a name and OS type from its file name, unless the user already typed one.
    private void OnIsoChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading || !_isNew) return;
        var file = Path.GetFileNameWithoutExtension(IsoBox.Text.Trim());
        if (string.IsNullOrEmpty(file)) return;
        var lower = file.ToLowerInvariant();
        GuestOs? os = lower switch
        {
            _ when lower.Contains("reactos") => GuestOs.ReactOS,
            _ when Regex.IsMatch(lower, @"win(dows)?[ _-]?(7|8|10|11|xp|vista)|^win") => GuestOs.Windows,
            _ when Regex.IsMatch(lower, "ubuntu|debian|fedora|mint|alpine|arch|manjaro|suse|linux|kali|elementary|zorin|pop") => GuestOs.Linux,
            _ => null,
        };
        if (os is { } guessed && guessed != SelectedOs) OsBox.SelectedIndex = (int)guessed;
        if (!_nameEdited)
        {
            // "lubuntu-26.04.1-desktop-amd64" -> "Lubuntu 26.04.1"
            var parts = Regex.Split(file, "[-_ ]+").Where(p => !Regex.IsMatch(p, "^(amd64|x86_64|i[3-6]86|desktop|live|server|dvd|cd|iso|x64|x86)$", RegexOptions.IgnoreCase));
            var name = string.Join(" ", parts.Take(2));
            if (name.Length > 0)
            {
                _loading = true;
                NameBox.Text = char.ToUpper(name[0]) + name[1..];
                _loading = false;
                UpdateNewDiskPath();
            }
        }
    }

    private void Fail(string message, Control? focus = null)
    {
        ErrorText.Text = message;
        focus?.Focus();
    }

    private void OnOk(object sender, RoutedEventArgs e)
    {
        ErrorText.Text = "";
        var name = NameBox.Text.Trim();
        if (name.Length == 0)
        {
            Fail("Makineye bir ad verin.", NameBox);
            return;
        }
        if (_nameTaken(name, _original))
        {
            Fail($"'{name}' adında bir makine zaten var.", NameBox);
            return;
        }
        var iso = IsoBox.Text.Trim();
        if (iso.Length > 0 && !File.Exists(iso))
        {
            Fail("ISO dosyası bulunamadı.", IsoBox);
            return;
        }
        if (!SandvmCommand.TryParseForwards(ForwardBox.Text, out _, out var forwardError))
        {
            Fail(forwardError, ForwardBox);
            return;
        }

        string? diskPath = null;
        bool ownsDisk = false;
        if (ExistingDiskRadio.IsChecked == true)
        {
            diskPath = DiskBox.Text.Trim();
            if (!File.Exists(diskPath))
            {
                Fail("Disk dosyası bulunamadı.", DiskBox);
                return;
            }
            // Keep ownership when the user left the machine's own disk selected.
            ownsDisk = _original is { OwnsDisk: true } && string.Equals(_original.DiskPath, diskPath, StringComparison.OrdinalIgnoreCase);
        }
        else if (NewDiskRadio.IsChecked == true)
        {
            diskPath = NewDiskPath;
            if (File.Exists(diskPath))
            {
                var answer = MessageBox.Show(this, $"Bu konumda zaten bir disk var:\n{diskPath}\n\nBu disk kullanılsın mı?",
                                             "Disk zaten var", MessageBoxButton.YesNo, MessageBoxImage.Question);
                if (answer != MessageBoxResult.Yes) return;
            }
            else
            {
                try
                {
                    DiskImage.CreateSparse(diskPath, (long)DiskSizeSlider.Value * 1024 * 1024 * 1024);
                    ownsDisk = true;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Fail("Disk oluşturulamadı: " + ex.Message);
                    return;
                }
            }
        }
        if (diskPath is null && iso.Length == 0)
        {
            Fail("Diski olmayan bir makinenin en azından bir ISO'su olmalı.");
            return;
        }

        var vm = _original?.Clone() ?? new VirtualMachine();
        vm.Name = name;
        vm.Os = SelectedOs;
        vm.CdromPath = iso.Length > 0 ? iso : null;
        vm.RamMiB = (int)RamSlider.Value;
        vm.Cpus = (int)CpuSlider.Value;
        vm.DiskPath = diskPath;
        vm.OwnsDisk = ownsDisk;
        vm.Network = NetworkCheck.IsChecked == true;
        vm.PortForwards = ForwardBox.Text.Trim();
        vm.Display = DisplayCheck.IsChecked == true;
        vm.Apic = ApicCheck.IsChecked == true;
        vm.Verbose = VerboseCheck.IsChecked == true;
        vm.SandboxBox = SandboxBox.Text.Trim();
        Result = vm;
        DialogResult = true;
    }
}
