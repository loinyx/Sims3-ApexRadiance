using System.Text.Json;

namespace ApexSetup;

// The launcher's own choices, in %LOCALAPPDATA%\Apex Launcher\settings.json (the language stays in language.txt; start
// with Windows is the Run key itself, LauncherInstall.RunsAtStartup).
public sealed class LauncherSettings
{
    public bool FixSafeAutomatically { get; set; } = true;
    public bool InstallUpdatesOnPlay { get; set; } = true;
    public bool HideWhilePlaying { get; set; } = true;
    public bool WelcomedBack { get; set; }
    public bool DesktopShortcut { get; set; } = true;
    public string HomePicture { get; set; } = "gallery"; // the picture on top of Home: "gallery", "household" or "none"
    public bool NavCompact { get; set; }                  // the side pane kept to its icons (the toggle at its top)

    static string FilePath => Path.Combine(Paths.Data, "settings.json");

    public static LauncherSettings Load()
    {
        try { return File.Exists(FilePath) ? JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.LauncherSettings) ?? new() : new(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return new(); }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Paths.Data);
            Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(this, SetupJson.Default.LauncherSettings));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
