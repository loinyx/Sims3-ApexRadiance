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
        // (before anything writes to the new folder; not in the administrator copy an old Setup starts, which only opens the
        // player's: moved there, the player's copy found nothing left and never said that Setup became the launcher)
        if (!Elevation.IsUacElevated) Paths.MigrateOldData();
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
        // (a background task nobody awaited: logged and kept from ending the process)
        TaskScheduler.UnobservedTaskException += (_, e) => { Log(e.Exception); e.SetObserved(); };
    }

    // the theme the window is in (Windows' own unless a test run picks one), and a brush of it by name: code reads the
    // light or dark one itself, the application's lookup only knows the theme it started with
    public static bool Light;
    public static Microsoft.UI.Xaml.Media.Brush Brush(string key)
    {
        var res = Current.Resources;
        var theme = Light ? "Light" : "Default";
        foreach (var d in new[] { res }.Concat(res.MergedDictionaries))
            if (d.ThemeDictionaries.TryGetValue(theme, out var t) && t is ResourceDictionary td && td.TryGetValue(key, out var v) && v is Microsoft.UI.Xaml.Media.Brush b) return b;
        return (Microsoft.UI.Xaml.Media.Brush)res[key];
    }

    static readonly Queue<string> Recent = new();

    public static void Log(object? e)
    {
        // (an expected dry-run refusal: one line, so the real errors stand out in the log)
        if (e is InvalidOperationException { Message: var dry } && dry.StartsWith("Dry run: ", StringComparison.Ordinal)) e = dry;
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
    EventWaitHandle? _show, _replace;
    // the one-at-a-time lock: a copy with a data folder of its own (APEX_LAUNCHER_LOCALAPPDATA, the UI tests) is a launcher of
    // its own, so a test never brings up or closes the player's launcher
    public static readonly string Instance = Environment.GetEnvironmentVariable("APEX_LAUNCHER_LOCALAPPDATA") is { Length: > 0 } own
        ? @"Local\ApexLauncher." + Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(own.ToUpperInvariant())))[..12]
        : @"Local\ApexLauncher";

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
            Environment.Exit(MainWindow.UninstallElevated());
            return;
        }
        // the administrator copy of Game settings: no window, the game folder's files, then it closes
        if (MainWindow.GameSettingsElevated(maintain, screen, argv) is { } code)
        {
            Environment.Exit(code);
            return;
        }
        // the same for the worlds added on the Mods screen
        if (MainWindow.WorldsElevated(maintain, screen, argv) is { } worlds)
        {
            Environment.Exit(worlds);
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
                switch (LauncherInstall.FirstStep(self, Elevation.IsUacElevated && !Elevation.RestartedRecently()))
                {
                    // started as administrator and not installed (the update an old Setup started): again as the player
                    case StartStep.RestartAsPlayer: if (Elevation.RestartAsPlayer(self)) { Exit(); return; } break;
                    case StartStep.Register: LauncherInstall.Register(self, Version, LauncherSettings.Load().DesktopShortcut); break;
                    // installed before: this copy updates the installed one when newer and hands over to it
                    // (when this copy updated the installed one, a running older copy is asked to close so the new one takes its place)
                    case StartStep.HandToInstalled when (File.Exists(Paths.InstalledExe) && LauncherInstall.Newer(self, Paths.InstalledExe)) is var updating && LauncherInstall.CopyIn(self):
                        var handed = updating ? LauncherInstall.HandoverArgs(argv).Append(LauncherInstall.ReplaceArg) : argv;
                        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Paths.InstalledExe, string.Join(" ", handed.Select(SelfUpdate.Quote))) { UseShellExecute = true });
                        Exit();
                        return;
                }
            }
            catch (Exception e) { Log(e); } // (an install that fails leaves this copy running: the launcher still works from here)
        }
        Paths.MigrateOldData(); // (an administrator copy that stays; nothing to do in the others)
        // one launcher at a time: a second start brings the first one's window up
        if (maintain is null && !uninstall)
        {
            _single = new Mutex(true, Instance, out var first);
            _show = new EventWaitHandle(false, EventResetMode.AutoReset, Instance + ".Show");
            _replace = new EventWaitHandle(false, EventResetMode.AutoReset, Instance + ".Replace");
            if (!first && argv.Contains(LauncherInstall.ReplaceArg)) _replace.Set();
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
            var replace = _replace;
            new Thread(() => { while (replace.WaitOne()) _window?.DispatcherQueue.TryEnqueue(() => (_window as MainWindow)?.QuitForNewerCopy()); }) { IsBackground = true }.Start();
            new Thread(() => { while (show.WaitOne()) _window?.DispatcherQueue.TryEnqueue(() => { _window.AppWindow.Show(); _window.Activate(); (_window as MainWindow)?.TakeHandedFiles(); }); }) { IsBackground = true }.Start();
        }
        Design.DesignSystem.Install(LauncherSettings.Load().Theme);
        var w = new MainWindow();
        _window = w;
        w.OpenWhenReady(MainWindow.ModFilesIn(argv));
        if (argv.Contains(LauncherInstall.TrayArg)) w.StartInTray();
        else w.Activate();
        if (uninstall) w.UninstallWhenShown();
    }
}
