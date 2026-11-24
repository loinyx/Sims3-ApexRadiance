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
    // (true for this run when the move happened: the launcher then says Setup became it)
    public static bool MovedFromSetup { get; private set; }
    public static void MigrateOldData()
    {
        var old = Path.Combine(LocalAppData, "Apex Radiance Setup");
        if (!Directory.Exists(old)) return;
        try
        {
            if (!Directory.Exists(Data)) { Directory.Move(old, Data); MovedFromSetup = true; return; }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (in use: copied below instead)
        // an old folder still there (in use when the move was tried, or left by an earlier start): what the new folder lacks
        // is copied once, so the remembered games and the language are never stranded; the old files stay for the old Setup
        var done = Path.Combine(old, ".copied-to-apex-launcher");
        if (File.Exists(done)) return;
        try
        {
            foreach (var f in Directory.EnumerateFiles(old, "*", SearchOption.AllDirectories))
            {
                var to = Path.Combine(Data, Path.GetRelativePath(old, f));
                if (File.Exists(to)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                File.Copy(f, to);
            }
            File.WriteAllText(done, "");
            MovedFromSetup = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (next start tries again)
    }
}
