namespace ApexSetup;

public sealed record Save(string Dir, string Name, DateTime LastPlayed);

// The player's saves (Documents\...\Saves\<name>.sims3 folders). "Last played" is the newest file inside, as the restart
// host reads it (a folder's own time misses files rewritten in place); .backup copies do not count.
public static class Saves
{
    // the household photo kept in a save (unverified on saves of every edition: a save without one shows no photo)
    public const uint PhotoType = 0x6B6D837E;

    public static List<Save> List(string docs) => In(Path.Combine(docs, "Saves"));

    // the saves of a folder (Saves, or where the launcher keeps saves out of the game's sight)
    internal static List<Save> In(string dir)
    {
        if (!Directory.Exists(dir)) return [];
        var list = new List<Save>();
        foreach (var d in Directory.EnumerateDirectories(dir, "*.sims3"))
        {
            var newest = DateTime.MinValue;
            try
            {
                foreach (var f in Directory.EnumerateFiles(d).Where(f => !Passing(f)))
                    if (!f.EndsWith(".backup", StringComparison.OrdinalIgnoreCase) && File.GetLastWriteTime(f) is var t && t > newest) newest = t;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            if (newest > DateTime.MinValue) list.Add(new Save(d, Path.GetFileNameWithoutExtension(d), newest));
        }
        return list.OrderByDescending(s => s.LastPlayed).ToList();
    }

    public static Save? Newest(string docs) => List(docs).FirstOrDefault();

    // the save's own household picture (a world file holds one of every household: Meta.data says which)
    public static byte[]? Photo(Save s) => SaveInfo.Photo(s.Dir);

    // what an antivirus puts beside a file it scans and takes away at once (NAME.tmp); no file of a save ends so
    internal static bool Passing(string file) => file.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase);

    // the folder the player chose, else the launcher's own in Documents
    public static string BackupDir(string docs) => BackupChoices.Load().FolderOf(docs) ?? OwnBackupDir(docs);
    public static string OwnBackupDir(string docs) => Path.Combine(docs, "Saves backups (Apex Launcher)");

    // a zip of the save folder, checked file by file before it counts (SaveCopies.Zip); the save itself is only read.
    // note: why the launcher made it by itself (SaveCopies.BeforeClean). The uninstall never deletes these.
    public static string Backup(Save s, string docs, string? note = null, CancellationToken ct = default)
    {
        lock (SaveCopies.Gate) return BackupNow(s, docs, note, ct);
    }

    // the day and time in a backup's name, the same in every region (Thai, Arabic and Persian Windows count other years)
    public static string Stamp(DateTime when) => when.ToString("yyyy-MM-dd HH.mm.ss", System.Globalization.CultureInfo.InvariantCulture);

    static string BackupNow(Save s, string docs, string? note, CancellationToken ct)
    {
        var (choices, dir) = (BackupChoices.Load(), BackupDir(docs));
        try { Directory.CreateDirectory(dir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException && choices.FolderOf(docs) is not null) { Engine.Log(e); throw new SetupException("The backup folder isn't available: {0}", dir); }
        var when = DateTime.Now;
        string zip;
        // (never over an older copy: one made in the same second takes the next)
        while (File.Exists(zip = Path.Combine(dir, $"{s.Name} {Stamp(when)}.zip"))) when = when.AddSeconds(1);
        // (a copy that lets a change go ahead is always compared with the save)
        SaveCopies.Zip(s.Dir, zip, note, choices.Check || note is SaveCopies.BeforeClean or SaveCopies.BeforeRestore || Games.IsRunning(), choices.RememberMods ? SaveBackups.ModsNow(docs) : null, ct,
            companions: SaveCompanions.Files(docs, s.Name));
        return zip;
    }
}
