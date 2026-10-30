using System.Windows;

namespace SandVM.Manager;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // An unexpected error should explain itself instead of silently closing the manager.
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(args.Exception.Message, "SandVM Yöneticisi - beklenmeyen hata",
                            MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };
        base.OnStartup(e);
    }
}
