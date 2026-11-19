namespace ApexSetup;

// Where Apex Launcher keeps its own files: the signed-in player's %LOCALAPPDATA%, also when a helper runs elevated as
// another administrator. Settable so tests point it at a temporary folder.
public static class Paths
{
    // (APEX_LAUNCHER_LOCALAPPDATA: the UI feature harness keeps its notes, tags and settings away from the player's)
    public static string LocalAppData = Environment.GetEnvironmentVariable("APEX_LAUNCHER_LOCALAPPDATA")
        ?? Scan.PlayerShellFolder("Local AppData") ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

    // games.txt, language.txt, settings.json, the log and the helper's plan files
    public static string Data => Path.Combine(LocalAppData, "Apex Launcher");
    public static string Ops => Path.Combine(Data, "ops");
    // the copy the Start menu and desktop shortcuts open, replaced in place by self-update without admin
    public static string InstallDir => Path.Combine(LocalAppData, "Programs", "Apex Launcher");
    public static string InstalledExe => Path.Combine(InstallDir, "ApexLauncher.exe");

    // Apex Radiance Setup kept the same files in "Apex Radiance Setup": moved once, so the remembered games and the
    // language choice carry over. A folder already there wins (moved before, or written since).
    public static void MigrateOldData()
    {
        try
        {
            var old = Path.Combine(LocalAppData, "Apex Radiance Setup");
            if (Directory.Exists(old) && !Directory.Exists(Data)) Directory.Move(old, Data);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (in use: the old files stay where they are)
    }
}
