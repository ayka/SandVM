using System.IO;
using System.Windows;
using Microsoft.Win32;
using SandVM.Manager.Services;

namespace SandVM.Manager.Views;

public partial class SettingsWindow : Window
{
    private readonly AppSettings _settings;

    public SettingsWindow(AppSettings settings)
    {
        InitializeComponent();
        _settings = settings;
        SandvmBox.Text = settings.SandvmPath;
        BiosBox.Text = settings.BiosPath;
        FolderBox.Text = settings.MachinesFolder;

        // Show what automatic detection finds, so empty fields are not a mystery.
        var found = ToolPaths.Resolve(new AppSettings());
        SandvmHint.Text = found.Sandvm is null
            ? "Otomatik olarak bulunamadı; lütfen seçin."
            : "Boş bırakılırsa otomatik bulunan kullanılır: " + found.Sandvm;
        BiosHint.Text = found.Bios is null
            ? "Otomatik olarak bulunamadı; lütfen seçin."
            : "Boş bırakılırsa otomatik bulunan kullanılır: " + found.Bios;
    }

    private void OnBrowseSandvm(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "sandvm.exe|sandvm.exe|Programlar (*.exe)|*.exe" };
        if (dialog.ShowDialog(this) == true) SandvmBox.Text = dialog.FileName;
    }

    private void OnBrowseBios(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "BIOS kalıbı (*.bin)|*.bin|Tüm dosyalar|*.*" };
        if (dialog.ShowDialog(this) == true) BiosBox.Text = dialog.FileName;
    }

    private void OnBrowseFolder(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Sanal makine klasörünü seçin" };
        if (dialog.ShowDialog(this) == true) FolderBox.Text = dialog.FolderName;
    }

    private void OnOk(object sender, RoutedEventArgs e)
    {
        foreach (var (box, label) in new[] { (SandvmBox, "sandvm.exe"), (BiosBox, "BIOS dosyası") })
        {
            var path = box.Text.Trim();
            if (path.Length > 0 && !File.Exists(path))
            {
                MessageBox.Show(this, $"{label} bulunamadı:\n{path}", "Ayarlar", MessageBoxButton.OK, MessageBoxImage.Warning);
                box.Focus();
                return;
            }
        }
        _settings.SandvmPath = SandvmBox.Text.Trim();
        _settings.BiosPath = BiosBox.Text.Trim();
        _settings.MachinesFolder = FolderBox.Text.Trim();
        DialogResult = true;
    }
}
