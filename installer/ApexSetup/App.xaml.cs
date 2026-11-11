using Microsoft.UI.Xaml;

namespace ApexSetup;

public partial class App : Application
{
    Window? _window;

    // attach this file to a bug report
    public static readonly string LogPath = Path.Combine(Path.GetTempPath(), "ApexRadianceSetup.log");

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) => Log(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log(e.ExceptionObject as Exception);
    }

    public static void Log(Exception? e)
    {
        try { File.AppendAllText(LogPath, $"{DateTime.Now:s} {e}\r\n"); } catch (IOException) { }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
