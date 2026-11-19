namespace ApexSetup;

// Uninstall Apex Launcher: everything it put on the PC goes, and what it changed is put back. Per game (as administrator
// when the game folder needs it): Apex Radiance and every component the launcher installed are removed and the files they
// replaced restored (Engine.RemoveAll), the mods the player switched off are put back in place, and the backup folder
// goes once nothing in it is still needed. Then, as the player: the shortcuts, the Installed apps entry, the Run value,
// the launcher's data and its exe. Never touched: saves, save backups, mods the player installed, Mods\Resource.cfg and
// the .apex-backup copies of the player's own settings files.
public static class Uninstall
{
    // what stayed and why, empty when the game is clean
    public static List<string> FromGame(Game g, string docs, bool keepSettings)
    {
        var left = new List<string>();
        Engine.RemoveAll(g, docs);
        Mods.EnableAll(g, docs);
        var dir = Manifest.DirOf(g.Bin);
        if (Manifest.Load(g.Bin).Files.Count == 0)
        {
            try { if (Directory.Exists(dir)) Fs.Retry(() => Directory.Delete(dir, true)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(dir); }
        }
        else left.Add(dir); // (a file could not be put back: its backup stays where Repair can find it)
        if (!keepSettings)
        {
            var apex = Path.Combine(docs, "Apex Radiance");
            try { if (Directory.Exists(apex)) Fs.Retry(() => Directory.Delete(apex, true)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(apex); }
        }
        return left;
    }

    // Smooth Motion's and AFMF's markers (machine-wide, written as administrator): gone once the drivers are back
    public static void MachineData()
    {
        var d = Path.GetDirectoryName(SmoothMotion.Marker)!;
        try { if (Directory.Exists(d)) Directory.Delete(d, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // the games that have anything of the launcher's: Apex Radiance, a manifest or switched-off mods
    public static List<Game> Games() =>
        ApexSetup.Games.Detect().Concat(ApexSetup.Games.Remembered())
            .GroupBy(g => g.Bin, StringComparer.OrdinalIgnoreCase).Select(x => x.First())
            .Where(g => Scan.Run(g).ApexVersion is not null || Directory.Exists(Manifest.DirOf(g.Bin)) || Directory.Exists(Mods.Disabled(Scan.DocumentsFor(g))))
            .ToList();

    // as the player, last: nothing of the launcher left once this process closes (a copy outside the install folder, portable
    // or never chosen, goes with its marker; the folder it sits in stays)
    public static void Launcher(string? exe = null)
    {
        exe ??= Environment.ProcessPath;
        LauncherInstall.RemoveTraces();
        var outside = exe is not null && !LauncherInstall.IsInstalledCopy(exe);
        LauncherInstall.DeleteAfterExit(outside ? [Paths.InstallDir, Paths.Data, exe!, LauncherInstall.MarkerOf(exe!)] : [Paths.InstallDir, Paths.Data]);
    }
}
