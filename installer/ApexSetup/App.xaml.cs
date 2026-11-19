using Microsoft.UI.Xaml;

namespace ApexSetup;

public partial class App : Application
{
    Window? _window;

    // attach this file to a bug report: in the player's own %LOCALAPPDATA% even when the helper runs elevated as another administrator
    public static readonly string LogDir = Paths.Data;
    public static readonly string LogPath = Path.Combine(LogDir, "ApexLauncher.log");

    // what Setup is working on (the game, its edition and version, the renderer, the loaders), written with every entry
    public static Func<string> Context = () => "";
    public static string Version => typeof(App).Assembly.GetName().Version?.ToString(3) ?? "?";

    public App()
    {
        Paths.MigrateOldData(); // (before anything writes to the new folder)
        InitializeComponent();
        Engine.Log = Log;
        // logged and kept running: a UI error must not end Setup in the middle of an install. A WinUI error reaches here without
        // its stack (a stowed exception): the last COM errors are kept with the stack where they were thrown and logged with it.
        AppDomain.CurrentDomain.FirstChanceException += (_, e) =>
        {
            if (e.Exception is not System.Runtime.InteropServices.COMException c) return;
            lock (Recent)
            {
                Recent.Enqueue($"{DateTime.Now:s} {c.GetType().Name} 0x{c.HResult:X8} {c.Message}\r\n{Environment.StackTrace}");
                while (Recent.Count > 5) Recent.Dequeue();
            }
        };
        UnhandledException += (_, e) =>
        {
            e.Handled = true;
            string recent;
            lock (Recent) recent = string.Join("\r\n", Recent);
            Log(new InvalidOperationException("Unhandled: " + e.Message + "\r\nRecent COM errors:\r\n" + recent, e.Exception));
            (_window as MainWindow)?.ShowUnexpected(e.Exception, log: false);
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log(e.ExceptionObject as Exception);
    }

    static readonly Queue<string> Recent = new();

    public static void Log(object? e)
    {
        string ctx;
        try { ctx = Context(); } catch (Exception) { ctx = ""; }
        try
        {
            Directory.CreateDirectory(LogDir);
            File.AppendAllText(LogPath, $"{DateTime.Now:s} Launcher {Version} {ctx}\r\n{e}\r\n");
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
    }

    // for "Copy details": the context and the log's last lines
    public static string Details()
    {
        string tail;
        try { tail = File.Exists(LogPath) ? string.Join("\r\n", File.ReadLines(LogPath).TakeLast(80)) : ""; }
        catch (IOException) { tail = ""; }
        string ctx;
        try { ctx = Context(); } catch (Exception) { ctx = ""; }
        return $"Apex Launcher {Version}\r\n{ctx}\r\n\r\n{tail}";
    }

    Mutex? _single;
    EventWaitHandle? _show;

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var argv = Environment.GetCommandLineArgs().Skip(1).ToList();
        var self = Environment.ProcessPath;
        if (self is not null) SelfUpdate.CleanUp(self); // (the exe an update replaced)
        SelfUpdate.CleanTemp(); // (and the copy it was downloaded to)
        var (maintain, screen) = Elevation.Parse(argv);
        // the administrator copy of an uninstall: no window, every game, then it closes
        if (maintain == "*" && screen is "uninstall" or "uninstall-keep" && Elevation.IsElevated)
        {
            Environment.Exit(MainWindow.UninstallElevated(screen == "uninstall-keep"));
            return;
        }
        var uninstall = argv.Contains(LauncherInstall.UninstallArg);
        // (a test or a dry run never installs: APEX_LAUNCHER_PORTABLE=1 runs the exe where it is)
        var portable = Environment.GetEnvironmentVariable(LauncherInstall.PortableVar) == "1" || Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1";
        if (maintain is null && !uninstall && !portable && self is not null)
        {
            try
            {
                // never installs by itself: a first start (NotChosen) opens here and the window asks installed or portable
                switch (LauncherInstall.FirstStep(self, Elevation.IsElevated))
                {
                    // started as administrator and not installed (the update an old Setup started): again as the player
                    case StartStep.RestartAsPlayer: if (Elevation.RestartAsPlayer(self)) { Exit(); return; } break;
                    case StartStep.Register: LauncherInstall.Register(self, Version, LauncherSettings.Load().DesktopShortcut); break;
                    // installed before: this copy updates the installed one when newer and hands over to it
                    case StartStep.HandToInstalled when LauncherInstall.CopyIn(self):
                        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Paths.InstalledExe, string.Join(" ", argv.Select(SelfUpdate.Quote))) { UseShellExecute = true });
                        Exit();
                        return;
                }
            }
            catch (Exception e) { Log(e); } // (an install that fails leaves this copy running: the launcher still works from here)
        }
        // one launcher at a time: a second start brings the first one's window up
        if (maintain is null && !uninstall)
        {
            _single = new Mutex(true, @"Local\ApexLauncher", out var first);
            _show = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\ApexLauncher.Show");
            // (started by Install, MakePortable or an update while the copy before it still closes: waits for it)
            if (!first && argv.Contains(LauncherInstall.HandoverArg))
                try { first = _single.WaitOne(TimeSpan.FromSeconds(20)); } catch (AbandonedMutexException) { first = true; }
            // (a second start with mod files, "Open with": the files are handed to the first one through the ops folder)
            if (!first)
            {
                if (MainWindow.ModFilesIn(argv) is { Count: > 0 } handed)
                    try { Directory.CreateDirectory(Paths.Ops); File.WriteAllLines(Path.Combine(Paths.Ops, $"open-{Guid.NewGuid():N}.txt"), handed); }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Log(e); }
                _show.Set(); Exit(); return;
            }
            var show = _show;
            new Thread(() => { while (show.WaitOne()) _window?.DispatcherQueue.TryEnqueue(() => { _window.AppWindow.Show(); _window.Activate(); (_window as MainWindow)?.TakeHandedFiles(); }); }) { IsBackground = true }.Start();
        }
        var w = new MainWindow();
        _window = w;
        w.OpenWhenReady(MainWindow.ModFilesIn(argv));
        if (argv.Contains(LauncherInstall.TrayArg)) w.StartInTray();
        else w.Activate();
        if (uninstall) w.UninstallWhenShown();
    }
}
