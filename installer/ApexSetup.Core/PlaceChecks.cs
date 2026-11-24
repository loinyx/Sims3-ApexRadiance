namespace ApexSetup;

// Where the game and its Documents folder live: OneDrive (it syncs saves and caches while the game writes them, and
// "files on demand" leaves placeholders the game cannot read), drives running out of space, and a game folder on a network
// or removable drive.
public static class PlaceChecks
{
    public const long LowFreeBytes = 5L << 30;

    // the OneDrive roots Windows sets for the signed-in user (personal and work); settable for tests
    public static Func<IEnumerable<string>> OneDriveRoots = () =>
        new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" }.Select(Environment.GetEnvironmentVariable).OfType<string>().Where(r => r.Length > 0);

    public static bool InOneDrive(string path)
    {
        var full = Path.GetFullPath(path);
        bool Under(string root) => full.StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        // (a OneDrive folder not named in the environment, such as another account's, still has OneDrive in its name)
        return OneDriveRoots().Any(Under) || full.Split(Path.DirectorySeparatorChar).Any(p => p.Equals("OneDrive", StringComparison.OrdinalIgnoreCase) || p.StartsWith("OneDrive - ", StringComparison.OrdinalIgnoreCase));
    }

    public static List<Finding> Check(Game g, string docs)
    {
        var f = new List<Finding>();
        if (InOneDrive(docs))
            f.Add(new(Severity.Warning, "Your Documents folder is in OneDrive ({0}). If OneDrive syncs saves while the game writes them, they can break. Move The Sims 3's folder out of OneDrive.", [docs]));
        foreach (var root in new[] { docs, g.Root }.Select(p => Path.GetPathRoot(Path.GetFullPath(p))).OfType<string>().Distinct(StringComparer.OrdinalIgnoreCase))
            try
            {
                var d = new DriveInfo(root);
                if (d.IsReady && d.AvailableFreeSpace < LowFreeBytes)
                    f.Add(new(Severity.Warning, "Drive {0} has {1} GB free. The game needs space to save and rebuild its caches. Free up some space.", [d.Name, (d.AvailableFreeSpace >> 30).ToString()]));
            }
            catch (Exception e) when (e is ArgumentException or IOException or UnauthorizedAccessException) { } // (a UNC share: no free space read)
        if (Path.GetFullPath(g.Root).StartsWith(@"\\") || DriveOf(g.Root) is DriveType.Network or DriveType.Removable)
            f.Add(new(Severity.Warning, "The game is on a network or removable drive ({0}). It loads slowly and crashes if the drive disconnects. Install it on an internal drive.", [g.Root]));
        return f;
    }

    static DriveType DriveOf(string path)
    {
        try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(path))!).DriveType; }
        catch (ArgumentException) { return DriveType.Unknown; }
    }
}
