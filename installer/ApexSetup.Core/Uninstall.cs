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
        // (the switched-off mods come back first: they are the player's, whatever happens to the rest)
        try { Mods.EnableAll(g, docs); }
        finally { Engine.RemoveAll(g, docs); }
        var dir = Manifest.DirOf(g.Bin);
        if (Manifest.Load(g.Bin).Files.Count == 0)
        {
            try { if (Directory.Exists(dir)) Fs.Retry(() => Directory.Delete(dir, true)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(dir); }
        }
        else left.Add(dir); // (a file could not be put back: its backup stays where Repair can find it)
        if (!keepSettings) left.AddRange(ApexSettings(docs));
        return left;
    }

    // Apex Radiance's settings, logs and caches in Documents go; the pictures and videos it saved there stay (its
    // Screenshots and Videos folders, and any folder its settings send captures to), and so does the folder holding them
    public static List<string> ApexSettings(string docs)
    {
        var left = new List<string>();
        var apex = Path.Combine(docs, "Apex Radiance");
        if (!Directory.Exists(apex)) return left;
        var keep = Gallery.PictureFolders(docs).Concat(Gallery.VideoFolders(docs))
            .Append(Path.Combine(apex, "Screenshots")).Append(Path.Combine(apex, "Videos"))
            .Select(f => Path.GetFullPath(f).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar).ToList();
        bool Holds(string entry)
        {
            var e = Path.GetFullPath(entry).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            return keep.Any(k => k.StartsWith(e, StringComparison.OrdinalIgnoreCase));
        }
        foreach (var entry in Directory.EnumerateFileSystemEntries(apex).ToList())
        {
            if (Holds(entry)) continue;
            try
            {
                if (Directory.Exists(entry)) Fs.Retry(() => Directory.Delete(entry, true));
                else Fs.Retry(() => File.Delete(entry));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(entry); }
        }
        try { if (!Directory.EnumerateFileSystemEntries(apex).Any()) Directory.Delete(apex); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
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
    // keepData: the player's albums, captions, mod notes and settings stay for a later install (only the picture copies
    // and the plans made for the administrator copy go)
    public static void Launcher(string? exe = null, bool keepData = false)
    {
        exe ??= Environment.ProcessPath;
        LauncherInstall.RemoveTraces();
        var outside = exe is not null && !LauncherInstall.IsInstalledCopy(exe);
        string[] data = keepData ? [Path.Combine(Paths.Data, "thumbs"), Paths.Ops] : [Paths.Data];
        LauncherInstall.DeleteAfterExit(outside ? [Paths.InstallDir, .. data, exe!, LauncherInstall.MarkerOf(exe!)] : [Paths.InstallDir, .. data]);
    }
}
