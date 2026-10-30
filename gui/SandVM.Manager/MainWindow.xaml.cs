using System.Collections.Concurrent;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;
using SandVM.Manager.Models;
using SandVM.Manager.Services;
using SandVM.Manager.Views;

namespace SandVM.Manager;

public partial class MainWindow : Window
{
    private const int MaxLogChars = 400_000;
    private const int KeptLogChars = 300_000;

    // Terminal escape sequences (colors, cursor moves, OSC titles): the console view is plain text.
    private static readonly Regex AnsiEscape =
        new(@"\x1B\[[0-?]*[ -/]*[@-~]|\x1B\][^\x07\x1B]*(\x07|\x1B\\)|\x1B[()][0-9A-Za-z]|\x1B[@-Z\\-_=>]",
            RegexOptions.Compiled);

    private readonly StoreData _data;
    private readonly ObservableCollection<VirtualMachine> _machines;
    private readonly Dictionary<string, VmSession> _sessions = new();
    private readonly Dictionary<string, StringBuilder> _logs = new();
    private readonly ConcurrentQueue<(string Id, string Text)> _pendingOutput = new();
    private readonly DispatcherTimer _outputTimer;
    private readonly DispatcherTimer _previewTimer;
    private ToolPaths _paths;
    private DateTime _previewStamp;
    private string? _previewPath;

    public MainWindow()
    {
        InitializeComponent();
        _data = Store.Load();
        _machines = new ObservableCollection<VirtualMachine>(_data.Machines);
        foreach (var vm in _machines) vm.PropertyChanged += OnMachineChanged;
        MachineList.ItemsSource = _machines;
        _paths = ToolPaths.Resolve(_data.Settings);

        _outputTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(100), DispatcherPriority.Background, (_, _) => FlushOutput(), Dispatcher);
        _previewTimer = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => RefreshPreview(), Dispatcher);
        _outputTimer.Start();
        _previewTimer.Start();

        HostText.Text = $"Ana bilgisayar: {HostInfo.LogicalProcessors} mantıksal işlemci · " +
                        $"{VirtualMachine.FormatRam((int)HostInfo.TotalRamMiB)} bellek";
        if (_machines.Count > 0) MachineList.SelectedIndex = 0;
        UpdateUi();
        Loaded += async (_, _) => await CheckHypervisorAsync();
    }

    private VirtualMachine? Selected => MachineList.SelectedItem as VirtualMachine;

    private VmSession? SessionOf(VirtualMachine vm) => _sessions.TryGetValue(vm.Id, out var s) ? s : null;

    // --- State and layout -------------------------------------------------------------------------

    private void UpdateUi()
    {
        var vm = Selected;
        EmptyPanel.Visibility = _machines.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        DetailTabs.Visibility = vm is null ? Visibility.Collapsed : Visibility.Visible;
        HeaderPanel.Visibility = vm is null ? Visibility.Hidden : Visibility.Visible;
        HeaderPanel.DataContext = vm;
        DetailTabs.DataContext = vm;
        if (vm is null) return;

        bool stopped = vm.State == VmState.Stopped;
        StartButton.IsEnabled = stopped;
        ShutdownButton.IsEnabled = vm.State is VmState.Running or VmState.Starting;
        StopButton.IsEnabled = !stopped;
        EditButton.IsEnabled = stopped;
        DeleteButton.IsEnabled = stopped;
        DiskInfoText.Text = DescribeDisk(vm);
    }

    private static string DescribeDisk(VirtualMachine vm)
    {
        if (string.IsNullOrEmpty(vm.DiskPath)) return "Yok";
        try
        {
            var info = new FileInfo(vm.DiskPath);
            if (!info.Exists) return vm.DiskPath + "  (dosya bulunamadı)";
            return $"{vm.DiskPath}\n{DiskImage.FormatBytes(info.Length)} · diskte {DiskImage.FormatBytes(DiskImage.AllocatedBytes(vm.DiskPath))} kullanılıyor";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or Win32Exception)
        {
            return vm.DiskPath;
        }
    }

    private void OnMachineChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(VirtualMachine.State) && ReferenceEquals(sender, Selected)) UpdateUi();
    }

    private void OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateUi();
        ShowLogOf(Selected);
        _previewStamp = default;
        _previewPath = null;
        RefreshPreview();
    }

    private void OnListDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (Selected is { IsStopped: true }) OnStart(sender, e);
    }

    private void Save()
    {
        _data.Machines = _machines.ToList();
        try
        {
            Store.Save(_data);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, "Ayarlar kaydedilemedi: " + ex.Message, "SandVM", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private async Task CheckHypervisorAsync()
    {
        if (_paths.Sandvm is null)
        {
            SetHypervisorStatus(false, "sandvm.exe bulunamadı. Ayarlar'dan konumunu seçin.");
            return;
        }
        if (_paths.Bios is null)
        {
            SetHypervisorStatus(false, "BIOS dosyası (bios-256k.bin) bulunamadı. Ayarlar'dan konumunu seçin.");
            return;
        }
        SetHypervisorStatus(null, "Hipervizör denetleniyor...");
        var status = await HostInfo.CheckHypervisorAsync(_paths.Sandvm);
        SetHypervisorStatus(status.Ok, status.Text);
    }

    private void SetHypervisorStatus(bool? ok, string text)
    {
        HypervisorText.Text = text;
        HypervisorDot.Fill = ok switch
        {
            true => Palette.Success,
            false => (Brush)FindResource("DangerBrush"),
            _ => Palette.Muted,
        };
    }

    // --- Machine list actions ---------------------------------------------------------------------

    private void OnNew(object sender, RoutedEventArgs e) => CreateFrom(null);

    private void OnClone(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } vm) return;
        var copy = vm.Clone();
        copy.Id = Guid.NewGuid().ToString("N");
        copy.Name = vm.Name + " (kopya)";
        // A copy must not write into the original's disk.
        copy.DiskPath = null;
        copy.OwnsDisk = false;
        copy.State = VmState.Stopped;
        CreateFrom(copy);
    }

    private void CreateFrom(VirtualMachine? template)
    {
        var editor = new VmEditorWindow(template, isNew: true, _data.Settings.EffectiveMachinesFolder, IsNameTaken) { Owner = this };
        if (editor.ShowDialog() != true || editor.Result is null) return;
        var vm = editor.Result;
        vm.PropertyChanged += OnMachineChanged;
        _machines.Add(vm);
        Save();
        MachineList.SelectedItem = vm;
        UpdateUi();
    }

    private bool IsNameTaken(string name, VirtualMachine? self) =>
        _machines.Any(m => !ReferenceEquals(m, self) && m.Id != self?.Id &&
                           string.Equals(m.Name, name, StringComparison.CurrentCultureIgnoreCase));

    private void OnEdit(object sender, RoutedEventArgs e)
    {
        if (Selected is not { IsStopped: true } vm) return;
        var editor = new VmEditorWindow(vm, isNew: false, _data.Settings.EffectiveMachinesFolder, IsNameTaken) { Owner = this };
        if (editor.ShowDialog() != true || editor.Result is null) return;
        vm.CopySettingsFrom(editor.Result);
        Save();
        UpdateUi();
    }

    private void OnDelete(object sender, RoutedEventArgs e)
    {
        if (Selected is not { IsStopped: true } vm) return;
        bool hasOwnDisk = vm.OwnsDisk && !string.IsNullOrEmpty(vm.DiskPath) && File.Exists(vm.DiskPath);
        bool deleteDisk = false;
        if (hasOwnDisk)
        {
            var answer = MessageBox.Show(this,
                $"'{vm.Name}' silinsin mi?\n\nDisk dosyası da silinsin mi?\n{vm.DiskPath}\n\n" +
                "Evet: makine ve diski silinir (geri alınamaz).\nHayır: yalnızca listeden kaldırılır, disk kalır.",
                "Sanal makineyi sil", MessageBoxButton.YesNoCancel, MessageBoxImage.Warning, MessageBoxResult.Cancel);
            if (answer == MessageBoxResult.Cancel) return;
            deleteDisk = answer == MessageBoxResult.Yes;
        }
        else if (MessageBox.Show(this, $"'{vm.Name}' listeden kaldırılsın mı? Disk ve ISO dosyalarına dokunulmaz.",
                                 "Sanal makineyi sil", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            return;
        }

        if (deleteDisk)
        {
            try
            {
                File.Delete(vm.DiskPath!);
                // Remove the machine's own folder if nothing else is left in it.
                var dir = Path.GetDirectoryName(vm.DiskPath!);
                if (dir is not null && Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any())
                    Directory.Delete(dir);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                MessageBox.Show(this, "Disk silinemedi: " + ex.Message, "SandVM", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }
        try { File.Delete(VmSession.ScreenshotPathFor(vm)); } catch (IOException) { } catch (UnauthorizedAccessException) { }

        vm.PropertyChanged -= OnMachineChanged;
        _logs.Remove(vm.Id);
        int index = _machines.IndexOf(vm);
        _machines.Remove(vm);
        Save();
        if (_machines.Count > 0) MachineList.SelectedIndex = Math.Min(index, _machines.Count - 1);
        UpdateUi();
    }

    private void OnSettings(object sender, RoutedEventArgs e)
    {
        var window = new SettingsWindow(_data.Settings) { Owner = this };
        if (window.ShowDialog() != true) return;
        Save();
        _paths = ToolPaths.Resolve(_data.Settings);
        _ = CheckHypervisorAsync();
    }

    // --- Running machines -------------------------------------------------------------------------

    private void OnStart(object sender, RoutedEventArgs e)
    {
        if (Selected is not { IsStopped: true } vm) return;
        _paths = ToolPaths.Resolve(_data.Settings);
        if (!_paths.IsComplete)
        {
            MessageBox.Show(this, "sandvm.exe veya BIOS dosyası bulunamadı. Lütfen Ayarlar'dan konumlarını seçin.",
                            "SandVM", MessageBoxButton.OK, MessageBoxImage.Warning);
            OnSettings(sender, e);
            return;
        }
        var missing = new[] { vm.DiskPath, vm.CdromPath }.FirstOrDefault(p => !string.IsNullOrEmpty(p) && !File.Exists(p));
        if (missing is not null)
        {
            MessageBox.Show(this, $"Dosya bulunamadı:\n{missing}\n\nMakinenin ayarlarını düzenleyin.", "SandVM",
                            MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (string.IsNullOrEmpty(vm.DiskPath) && string.IsNullOrEmpty(vm.CdromPath))
        {
            MessageBox.Show(this, "Makinenin ne diski ne de ISO'su var; açılacak bir şey yok.", "SandVM",
                            MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        VmSession session;
        try
        {
            session = VmSession.Start(vm, _paths);
        }
        catch (Exception ex) when (ex is Win32Exception or InvalidOperationException or IOException)
        {
            MessageBox.Show(this, "Makine başlatılamadı: " + ex.Message, "SandVM", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        _sessions[vm.Id] = session;
        vm.State = VmState.Starting;
        vm.LastStarted = DateTime.Now;
        Save();
        AppendLog(vm.Id, $"\n[SandVM] {DateTime.Now:HH:mm:ss} başlatılıyor\n[SandVM] {session.CommandLine}\n");

        bool firstOutput = true;
        session.Output += text =>
        {
            _pendingOutput.Enqueue((vm.Id, text));
            if (!firstOutput) return;
            firstOutput = false;
            Dispatcher.BeginInvoke(() =>
            {
                if (vm.State == VmState.Starting) vm.State = VmState.Running;
            });
        };
        var started = DateTime.Now;
        session.Exited += code => Dispatcher.BeginInvoke(() => OnSessionExited(vm, session, code, started));
    }

    private void OnSessionExited(VirtualMachine vm, VmSession session, int code, DateTime started)
    {
        FlushOutput();
        if (_sessions.TryGetValue(vm.Id, out var current) && ReferenceEquals(current, session)) _sessions.Remove(vm.Id);
        session.Dispose();
        vm.State = VmState.Stopped;
        AppendLog(vm.Id, $"\n[SandVM] {DateTime.Now:HH:mm:ss} makine kapandı (çıkış kodu {code})\n");
        // A quick failure is almost always a configuration or host problem: show the log that explains it.
        if (code != 0 && DateTime.Now - started < TimeSpan.FromSeconds(10) && ReferenceEquals(vm, Selected))
            DetailTabs.SelectedIndex = 1;
        if (ReferenceEquals(vm, Selected)) UpdateUi();
    }

    private async void OnShutdown(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } vm || SessionOf(vm) is not { } session) return;
        if (!session.PressPowerButton())
        {
            MessageBox.Show(this, "Makine henüz kapatma isteğini alamıyor. Birkaç saniye sonra tekrar deneyin.",
                            "SandVM", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        vm.State = VmState.ShuttingDown;
        AppendLog(vm.Id, "\n[SandVM] güç düğmesine basıldı, misafir sistemin kapanması bekleniyor\n");

        await Task.Delay(TimeSpan.FromSeconds(60));
        if (!ReferenceEquals(SessionOf(vm), session) || session.HasExited) return;
        var answer = MessageBox.Show(this,
            $"'{vm.Name}' bir dakikadır kapanmadı. Misafir sistem güç düğmesini dinlemiyor olabilir.\n\nZorla durdurulsun mu?",
            "SandVM", MessageBoxButton.YesNo, MessageBoxImage.Question);
        if (answer == MessageBoxResult.Yes) await ForceStopAsync(session);
        else if (vm.State == VmState.ShuttingDown) vm.State = VmState.Running;
    }

    private async void OnForceStop(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } vm || SessionOf(vm) is not { } session) return;
        var answer = MessageBox.Show(this,
            $"'{vm.Name}' zorla durdurulsun mu?\n\nBu, bilgisayarın fişini çekmek gibidir; kaydedilmemiş veriler kaybolabilir. " +
            "Mümkünse önce 'Kapat'ı deneyin.",
            "Zorla durdur", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
        if (answer == MessageBoxResult.Yes) await ForceStopAsync(session);
    }

    private static async Task ForceStopAsync(VmSession session)
    {
        session.ForceStop();
        for (int i = 0; i < 30 && !session.HasExited; i++) await Task.Delay(100);
        if (!session.HasExited) session.Kill();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        var running = _sessions.Values.Where(s => !s.HasExited).ToList();
        if (running.Count > 0)
        {
            var answer = MessageBox.Show(this,
                $"{running.Count} sanal makine hâlâ çalışıyor. Çıkarsanız zorla durdurulacaklar.\n\nYine de çıkılsın mı?",
                "SandVM", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No);
            if (answer != MessageBoxResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            foreach (var s in running) s.ForceStop();
            var deadline = DateTime.Now.AddSeconds(3);
            foreach (var s in running)
            {
                while (!s.HasExited && DateTime.Now < deadline) Thread.Sleep(50);
                if (!s.HasExited) s.Kill();
            }
        }
        Save();
        base.OnClosing(e);
    }

    // --- Console ----------------------------------------------------------------------------------

    private static string CleanTerminalText(string text)
    {
        text = AnsiEscape.Replace(text, "");
        var sb = new StringBuilder(text.Length);
        foreach (char c in text)
        {
            // Carriage returns redraw a line in a terminal; in a text box they would only add noise.
            if (c == '\n' || c == '\t' || !char.IsControl(c)) sb.Append(c);
        }
        return sb.ToString();
    }

    private void AppendLog(string id, string text)
    {
        if (!_logs.TryGetValue(id, out var log)) _logs[id] = log = new StringBuilder();
        log.Append(text);
        if (log.Length > MaxLogChars) log.Remove(0, log.Length - KeptLogChars);

        if (Selected?.Id != id) return;
        bool atEnd = ConsoleOutput.VerticalOffset + ConsoleOutput.ViewportHeight >= ConsoleOutput.ExtentHeight - 4;
        if (ConsoleOutput.Text.Length + text.Length > MaxLogChars) ConsoleOutput.Text = log.ToString();
        else ConsoleOutput.AppendText(text);
        if (atEnd) ConsoleOutput.ScrollToEnd();
    }

    private void FlushOutput()
    {
        if (_pendingOutput.IsEmpty) return;
        var batches = new Dictionary<string, StringBuilder>();
        while (_pendingOutput.TryDequeue(out var item))
        {
            if (!batches.TryGetValue(item.Id, out var sb)) batches[item.Id] = sb = new StringBuilder();
            sb.Append(item.Text);
        }
        foreach (var (id, sb) in batches) AppendLog(id, CleanTerminalText(sb.ToString()));
    }

    private void ShowLogOf(VirtualMachine? vm)
    {
        ConsoleOutput.Text = vm is not null && _logs.TryGetValue(vm.Id, out var log) ? log.ToString() : "";
        ConsoleOutput.ScrollToEnd();
    }

    private void SendToGuest(string text)
    {
        if (Selected is not { } vm || SessionOf(vm) is not { } session)
        {
            MessageBox.Show(this, "Makine çalışmıyor.", "SandVM", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        session.Send(text);
    }

    private void OnSendInput(object sender, RoutedEventArgs e)
    {
        // A serial terminal sends CR for Enter; the guest's tty turns it into a newline.
        SendToGuest(ConsoleInput.Text + "\r");
        ConsoleInput.Clear();
        ConsoleInput.Focus();
    }

    private void OnConsoleInputKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter) return;
        OnSendInput(sender, e);
        e.Handled = true;
    }

    private void OnSendCtrlC(object sender, RoutedEventArgs e) => SendToGuest("\x03");

    private void OnClearConsole(object sender, RoutedEventArgs e)
    {
        if (Selected is { } vm) _logs.Remove(vm.Id);
        ConsoleOutput.Clear();
    }

    // --- Screen preview ---------------------------------------------------------------------------

    private void RefreshPreview()
    {
        if (Selected is not { } vm)
        {
            PreviewImage.Source = null;
            return;
        }
        var path = VmSession.ScreenshotPathFor(vm);
        DateTime stamp;
        try
        {
            stamp = File.Exists(path) ? File.GetLastWriteTimeUtc(path) : default;
        }
        catch (IOException)
        {
            return;
        }
        if (stamp == default)
        {
            PreviewImage.Source = null;
            PreviewPlaceholder.Visibility = Visibility.Visible;
            SaveScreenshotButton.IsEnabled = false;
            return;
        }
        if (stamp == _previewStamp && path == _previewPath) return;

        try
        {
            // sandvm replaces the file atomically; read it into memory so the file is never held open.
            byte[] data;
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                data = new byte[fs.Length];
                fs.ReadExactly(data);
            }
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = new MemoryStream(data);
            image.EndInit();
            image.Freeze();
            PreviewImage.Source = image;
            PreviewPlaceholder.Visibility = Visibility.Collapsed;
            SaveScreenshotButton.IsEnabled = true;
            _previewStamp = stamp;
            _previewPath = path;
        }
        catch (Exception ex) when (ex is IOException or NotSupportedException or UnauthorizedAccessException or EndOfStreamException)
        {
            // Being replaced right now: try again on the next tick.
        }
    }

    private void OnSaveScreenshot(object sender, RoutedEventArgs e)
    {
        if (PreviewImage.Source is not BitmapSource image || Selected is not { } vm) return;
        var dialog = new SaveFileDialog
        {
            Filter = "PNG resmi|*.png",
            FileName = $"{vm.Name} {DateTime.Now:yyyy-MM-dd HHmmss}.png",
        };
        if (dialog.ShowDialog(this) != true) return;
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(image));
        using var fs = File.Create(dialog.FileName);
        encoder.Save(fs);
    }

    // --- Misc -------------------------------------------------------------------------------------

    private void OnCopyCommand(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } vm) return;
        var paths = ToolPaths.Resolve(_data.Settings);
        var exe = paths.Sandvm ?? ToolPaths.SandvmExe;
        Clipboard.SetText(SandvmCommand.ToDisplayString(exe, SandvmCommand.Build(vm, paths, null, null)));
    }

    private void OnOpenFolder(object sender, RoutedEventArgs e)
    {
        if (Selected is not { } vm) return;
        if (!string.IsNullOrEmpty(vm.DiskPath) && File.Exists(vm.DiskPath))
        {
            Process.Start("explorer.exe", $"/select,\"{vm.DiskPath}\"");
            return;
        }
        var folder = _data.Settings.EffectiveMachinesFolder;
        Directory.CreateDirectory(folder);
        Process.Start("explorer.exe", $"\"{folder}\"");
    }
}
