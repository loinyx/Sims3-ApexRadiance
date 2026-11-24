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
    public int HomePictureHeight { get; set; } = 330;     // its height on Home, in pixels
    public int HomePictureStrength { get; set; } = 100;   // how strongly it shows, 30 to 100 (%)
    public string Backdrop { get; set; } = "mica";        // the window's background: "mica", "micaalt", "acrylic" or "solid"
    public bool NavCompact { get; set; }                  // the side pane kept to its icons (the toggle at its top)

    // the game options chosen before the game ever wrote Options.ini, by its Documents folder: set once the game has made it
    public Dictionary<string, Dictionary<string, string>> PendingOptions { get; set; } = new();

    public List<string> IgnoredConflicts { get; set; } = new(); // mod conflicts the player does not want to see again (ModConflict.Id)
    public List<string> IgnoredFindings { get; set; } = new();  // and every other finding of Health (Health.Item.Id)

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
