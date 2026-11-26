namespace ApexSetup;

// Uninstall Apex Launcher: everything it put on the PC goes, and what it changed is put back. Per game (as administrator
// when the game folder needs it): Apex Radiance and every component the launcher installed are removed and the files they
// replaced restored (Engine.RemoveAll), and the backup folder goes once nothing in it is still needed. Then, as the
// player: the mods the player switched off are put back in place, Apex Radiance's settings go unless kept, then the
// shortcuts, the Installed apps entry, the Run value, the launcher's data and its exe. Never touched: saves, save backups, mods the player installed, Mods\Resource.cfg and
// the .apex-backup copies of the player's own settings files.
public static class Uninstall
{
    // The game folder's part (as administrator when the folder needs it): what stayed and why, empty when the game is clean.
    // The backup folder goes once nothing in it is needed; the player's own files a fix set aside there (an old ASI
    // loader, a second copy of a plugin) go to the Recycle Bin, never deleted for good.
    public static List<string> FromGame(Game g, string docs)
    {
        var left = new List<string>();
        GameSettings.RestoreBin(g); // (before its copies, in the backup folder, go)
        Engine.RemoveAll(g, docs);
        var dir = Manifest.DirOf(g.Bin);
        if (Manifest.Load(g.Bin).Files.Count > 0) { left.Add(dir); return left; } // (a file could not be put back: its backup stays where Repair can find it)
        if (!Directory.Exists(dir)) return left;
        var restore = RestorePoints.RootOf(g.Bin) + Path.DirectorySeparatorChar;
        var all = new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint };
        foreach (var f in Directory.EnumerateFiles(dir, "*", all).ToList())
        {
            if (Path.GetFileName(f).Equals("setup.json", StringComparison.OrdinalIgnoreCase) && Path.GetDirectoryName(f)!.Equals(dir, StringComparison.OrdinalIgnoreCase)) continue;
            if (f.StartsWith(restore, StringComparison.OrdinalIgnoreCase)) continue;
            try { ModLibrary.Recycle(f); }
            catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { if (File.Exists(f)) left.Add(f); } // (an antivirus's own file comes and goes)
        }
        if (left.Count > 0) return left;
        try { Fs.DeleteClear(dir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(dir); }
        return left;
    }

    // The Documents part, always as the player (never the administrator copy: the player's folders can hold links that
    // point anywhere): the switched-off mods come back on and, unless kept, Apex Radiance's settings go
    public static List<string> FromDocuments(Game g, string docs, bool keepSettings)
    {
        Mods.EnableAll(g, docs);
        GameSettings.RestoreDocs(docs);
        return keepSettings ? [] : ApexSettings(docs);
    }

    // Apex Radiance's settings, logs and caches in Documents go; the pictures and videos it saved there stay (its
    // Screenshots and Videos folders, and any folder its settings send captures to), and so does the folder holding them
    public static List<string> ApexSettings(string docs)
    {
        // (also where an ASI that never moved it left it; what stays is known before the settings that say it go)
        var all = new[] { Scan.ApexDir(docs), Path.Combine(docs, "Apex Radiance") }.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        var keep = Gallery.PictureFolders(docs).Concat(Gallery.VideoFolders(docs)).Concat(all.SelectMany(d => new[] { Path.Combine(d, "Screenshots"), Path.Combine(d, "Videos") }))
            .Select(f => Path.GetFullPath(f).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar).ToList();
        return all.SelectMany(d => ApexSettings(d, keep)).ToList();
    }

    static List<string> ApexSettings(string apex, List<string> keep)
    {
        var left = new List<string>();
        if (!Directory.Exists(apex) || File.GetAttributes(apex).HasFlag(FileAttributes.ReparsePoint)) return left;
        bool Holds(string entry)
        {
            var e = Path.GetFullPath(entry).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            // (a capture folder inside this entry, or this entry inside a capture folder: the Apex folder itself can be one)
            return keep.Any(k => k.StartsWith(e, StringComparison.OrdinalIgnoreCase) || e.StartsWith(k, StringComparison.OrdinalIgnoreCase));
        }
        foreach (var entry in Directory.EnumerateFileSystemEntries(apex).ToList())
        {
            if (Holds(entry)) continue;
            try
            {
                // (a link goes alone, never what it points to)
                if (Directory.Exists(entry) && File.GetAttributes(entry).HasFlag(FileAttributes.ReparsePoint)) Directory.Delete(entry);
                else if (Directory.Exists(entry)) Fs.DeleteClear(entry);
                else Fs.Retry(() => File.Delete(entry));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(entry); }
        }
        try { if (!Directory.EnumerateFileSystemEntries(apex).Any()) Directory.Delete(apex); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return left;
    }

    // Smooth Motion's and AFMF's markers (machine-wide, written as administrator): gone once the drivers are back.
    // Returns the turn-off errors of the removals before it (taken, so a retry starts clean); with one, the markers stay:
    // they are what lets a later try, or the player's next install, still turn frame generation off.
    public static (string? Nv, string? Amd) MachineData()
    {
        var failed = Engine.TakeFrameGenOffError();
        if (failed != (null, null)) return failed;
        var d = Path.GetDirectoryName(SmoothMotion.Marker)!;
        try { if (Directory.Exists(d)) Directory.Delete(d, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return failed;
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
        var install = Paths.InstallDir;
        Paths.SetInstallDir(null);
        string[] data = keepData ? [Path.Combine(Paths.Data, "thumbs"), Paths.Ops] : [Paths.Data];
        LauncherInstall.DeleteInstallAfterExit(install, outside ? [.. data, exe!, LauncherInstall.MarkerOf(exe!)] : data);
    }
}
