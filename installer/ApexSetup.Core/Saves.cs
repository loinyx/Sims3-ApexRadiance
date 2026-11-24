using System.IO.Compression;

namespace ApexSetup;

public sealed record Save(string Dir, string Name, DateTime LastPlayed);

// The player's saves (Documents\...\Saves\<name>.sims3 folders). "Last played" is the newest file inside, as the restart
// host reads it (a folder's own time misses files rewritten in place); .backup copies do not count.
public static class Saves
{
    // the household photo kept in a save (unverified on saves of every edition: a save without one shows no photo)
    public const uint PhotoType = 0x6B6D837E;

    public static List<Save> List(string docs)
    {
        var dir = Path.Combine(docs, "Saves");
        if (!Directory.Exists(dir)) return [];
        var list = new List<Save>();
        foreach (var d in Directory.EnumerateDirectories(dir, "*.sims3"))
        {
            var newest = DateTime.MinValue;
            try
            {
                foreach (var f in Directory.EnumerateFiles(d))
                    if (!f.EndsWith(".backup", StringComparison.OrdinalIgnoreCase) && File.GetLastWriteTime(f) is var t && t > newest) newest = t;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            if (newest > DateTime.MinValue) list.Add(new Save(d, Path.GetFileNameWithoutExtension(d), newest));
        }
        return list.OrderByDescending(s => s.LastPlayed).ToList();
    }

    public static Save? Newest(string docs) => List(docs).FirstOrDefault();

    // the photo's image bytes, from the save's .nhd files first
    public static byte[]? Photo(Save s)
    {
        try
        {
            foreach (var f in Directory.EnumerateFiles(s.Dir).OrderBy(f => f.EndsWith(".nhd", StringComparison.OrdinalIgnoreCase) ? 0 : 1))
                if (!f.EndsWith(".backup", StringComparison.OrdinalIgnoreCase) && Dbpf.First(f, [PhotoType]) is { } bytes) return bytes;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return null;
    }

    public static string BackupDir(string docs) => Path.Combine(docs, "Saves backups (Apex Launcher)");

    // a zip of the save folder; the save itself is only read. The uninstall never deletes these.
    public static string Backup(Save s, string docs)
    {
        Directory.CreateDirectory(BackupDir(docs));
        var zip = Path.Combine(BackupDir(docs), $"{s.Name} {DateTime.Now:yyyy-MM-dd HH.mm.ss}.zip");
        // (written beside and moved in when whole: a zip cut short by a locked file or a full disk never counts as a backup)
        var part = zip + ".part";
        try
        {
            ZipFile.CreateFromDirectory(s.Dir, part, CompressionLevel.Fastest, includeBaseDirectory: true);
            File.Move(part, zip);
        }
        catch { try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
        return zip;
    }
}
