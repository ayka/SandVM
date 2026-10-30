using System.Diagnostics;
using System.IO;
using System.Text;
using SandVM.Manager.Models;

namespace SandVM.Manager.Services;

// One running sandvm.exe process. Its stdout/stderr (the guest's serial console and sandvm's log) are
// streamed through Output, and stdin feeds the guest's COM1. Events are raised on worker threads.
public sealed class VmSession : IDisposable
{
    private readonly Process _process;
    private readonly string _controlName;

    public VirtualMachine Machine { get; }
    public string ScreenshotPath { get; }
    public string CommandLine { get; }

    public event Action<string>? Output;
    public event Action<int>? Exited;

    private VmSession(VirtualMachine machine, Process process, string controlName, string screenshotPath, string commandLine)
    {
        Machine = machine;
        _process = process;
        _controlName = controlName;
        ScreenshotPath = screenshotPath;
        CommandLine = commandLine;
    }

    public static string ScreenshotPathFor(VirtualMachine vm) => Path.Combine(Store.DataFolder, "screens", vm.Id + ".bmp");

    public static VmSession Start(VirtualMachine vm, ToolPaths paths)
    {
        if (paths.Sandvm is null) throw new InvalidOperationException("sandvm.exe bulunamadı.");
        var screenshot = ScreenshotPathFor(vm);
        Directory.CreateDirectory(Path.GetDirectoryName(screenshot)!);
        // Unique per run, so a stale sandvm from an earlier run cannot receive this run's requests.
        var controlName = $"{vm.Id}.{Environment.ProcessId}.{DateTime.Now.Ticks}";
        var args = SandvmCommand.Build(vm, paths, controlName, vm.Display ? screenshot : null);

        var psi = new ProcessStartInfo(paths.Sandvm)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(paths.Sandvm)!,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);

        var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var session = new VmSession(vm, process, controlName, screenshot,
                                    SandvmCommand.ToDisplayString(paths.Sandvm, args));
        process.Start();
        var readers = new[] { session.Pump(process.StandardOutput.BaseStream), session.Pump(process.StandardError.BaseStream) };
        _ = Task.Run(async () =>
        {
            await process.WaitForExitAsync();
            // Drain what is still in the pipes before reporting the exit.
            await Task.WhenAll(readers);
            session.Exited?.Invoke(process.ExitCode);
        });
        return session;
    }

    // The guest's serial output arrives byte by byte (prompts have no newline), so read raw chunks.
    private Task Pump(Stream stream) => Task.Run(async () =>
    {
        var decoder = Encoding.UTF8.GetDecoder();
        var bytes = new byte[4096];
        var chars = new char[Encoding.UTF8.GetMaxCharCount(bytes.Length)];
        try
        {
            int n;
            while ((n = await stream.ReadAsync(bytes)) > 0)
            {
                int c = decoder.GetChars(bytes, 0, n, chars, 0);
                if (c > 0) Output?.Invoke(new string(chars, 0, c));
            }
        }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    });

    public bool HasExited
    {
        get
        {
            try { return _process.HasExited; }
            catch (InvalidOperationException) { return true; }
        }
    }

    // Sends bytes to the guest's COM1 (the serial console).
    public void Send(string text)
    {
        try
        {
            var data = Encoding.UTF8.GetBytes(text);
            _process.StandardInput.BaseStream.Write(data);
            _process.StandardInput.BaseStream.Flush();
        }
        catch (IOException) { }
        catch (InvalidOperationException) { }
    }

    // ACPI power button: the guest OS shuts itself down cleanly (if it listens to ACPI).
    public bool PressPowerButton() => Signal("power");

    // Stops the VM at once, like pulling the plug; falls back to killing the process.
    public void ForceStop()
    {
        if (Signal("stop")) return;
        Kill();
    }

    public void Kill()
    {
        try
        {
            if (!_process.HasExited) _process.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }

    private bool Signal(string what)
    {
        try
        {
            using var handle = EventWaitHandle.OpenExisting($@"Local\sandvm.{_controlName}.{what}");
            return handle.Set();
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            return false;  // not created yet (still starting) or already gone
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    public void Dispose() => _process.Dispose();
}
