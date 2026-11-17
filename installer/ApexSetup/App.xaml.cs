using Microsoft.UI.Xaml;

namespace ApexSetup;

public partial class App : Application
{
    Window? _window;

    // attach this file to a bug report: in the player's own %LOCALAPPDATA% even when Setup runs elevated as another administrator
    public static readonly string LogDir = Path.Combine(
        Scan.PlayerShellFolder("Local AppData") ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Apex Radiance Setup");
    public static readonly string LogPath = Path.Combine(LogDir, "ApexRadianceSetup.log");

    // what Setup is working on (the game, its edition and version, the renderer, the loaders), written with every entry
    public static Func<string> Context = () => "";
    public static string Version => typeof(App).Assembly.GetName().Version?.ToString(3) ?? "?";

    public App()
    {
        InitializeComponent();
        Engine.Log = Log;
        // logged and kept running: a UI error must not end Setup in the middle of an install
        UnhandledException += (_, e) => { Log(e.Exception); e.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log(e.ExceptionObject as Exception);
    }

    public static void Log(Exception? e)
    {
        string ctx;
        try { ctx = Context(); } catch (Exception) { ctx = ""; }
        try
        {
            Directory.CreateDirectory(LogDir);
            File.AppendAllText(LogPath, $"{DateTime.Now:s} Setup {Version} {ctx}\r\n{e}\r\n");
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
        return $"Apex Radiance Setup {Version}\r\n{ctx}\r\n\r\n{tail}";
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        if (Environment.ProcessPath is { } self) SelfUpdate.CleanUp(self); // (the exe a Setup update replaced)
        _window = new MainWindow();
        _window.Activate();
    }
}
