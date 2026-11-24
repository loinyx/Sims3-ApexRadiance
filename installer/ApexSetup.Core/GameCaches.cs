namespace ApexSetup;

public sealed record CacheFile(string Path, string Rel, long Size);

// The cache files The Sims 3 rebuilds by itself in Documents. Clearing them is the community's first answer to CAS or
// thumbnail glitches and slow loads after mods change. Clean moves them to the launcher's data (never deletes): the copy is
// kept until the next game start that ends normally (SessionHistory.Add), and Restore puts it back.
public static class GameCaches
{
    // Each was seen in a real Documents folder; the list is the one Mod The Sims' and NRaas' "delete your caches" guides give.
    // DCCache is left out on purpose: its .dbc files hold the game's installed store content, not a cache.
    static readonly string[] RootFiles =
    [
        "CASPartCache.package",       // CAS parts index, rebuilt at start
        "compositorCache.package",    // composited textures
        "scriptCache.package",        // compiled script cache
        "simCompositorCache.package", // composited Sim textures
        "socialCache.package",        // social network cache
    ];
    // (folder, pattern): every file in them is rebuilt or downloaded again
    static readonly (string Dir, string Pattern)[] Folders =
    [
        ("WorldCaches", "*.package"),   // one <world>_objects/_sims pair per world played, rebuilt on the next load of that world
        ("Thumbnails", "*.package"),    // CAS, object and downloads thumbnails
        ("FeaturedItems", "*"),         // store thumbnails the game downloads again (some guides only; unverified that it matters)
        ("IGACache", "*"),              // in-game advertising cache (empty on most installs)
    ];

    public static string BackupRoot => Path.Combine(Paths.Data, "cache-backup");

    public static List<CacheFile> List(string docs)
    {
        var list = new List<CacheFile>();
        void Add(string p) { try { list.Add(new(p, Path.GetRelativePath(docs, p), new FileInfo(p).Length)); } catch (IOException) { } }
        foreach (var f in RootFiles.Select(n => Path.Combine(docs, n)).Where(File.Exists)) Add(f);
        foreach (var (dir, pattern) in Folders)
            if (Path.Combine(docs, dir) is var d && Directory.Exists(d))
                try { foreach (var f in Directory.EnumerateFiles(d, pattern)) Add(f); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return list;
    }

    // the backup of the last Clean, null when none is kept
    // (a backup marked dropped is gone for the player even when Windows still holds a file in it; it is deleted later)
    public static string? Backup => Directory.Exists(BackupRoot) && !File.Exists(DroppedMark) ? Directory.EnumerateDirectories(BackupRoot).OrderDescending().FirstOrDefault() : null;
    static string DroppedMark => Path.Combine(BackupRoot, "dropped");

    // moves every cache into a new dated backup folder; returns the bytes moved. Refuses while the game runs.
    public static long Clean(string docs)
    {
        Engine.CheckWritable();
        var files = List(docs);
        if (files.Count == 0) return 0;
        // one backup at a time: an older one is from a clean whose game start never ended well, its caches are stale anyway
        Forget();
        // (an old backup Windows kept hold of stays until the next Forget; the newest one is the one Restore uses)
        try { File.Delete(DroppedMark); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        var to = Path.Combine(BackupRoot, DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"));
        Directory.CreateDirectory(to);
        File.WriteAllText(Path.Combine(to, "docs.txt"), docs); // (first: a clean cut off half way can still be restored)
        long moved = 0;
        foreach (var f in files)
        {
            var dest = Path.Combine(to, f.Rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            Fs.Retry(() => File.Move(f.Path, dest));
            moved += f.Size;
        }
        return moved;
    }

    // puts the last backup back over whatever the game rebuilt since; false when there is none
    public static bool Restore()
    {
        Engine.CheckWritable();
        if (Backup is not { } from || !File.Exists(Path.Combine(from, "docs.txt"))) return false;
        var docs = File.ReadAllText(Path.Combine(from, "docs.txt")).Trim();
        foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(from, f);
            if (rel == "docs.txt") continue;
            var dest = Path.Combine(docs, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            Fs.Retry(() => File.Move(f, dest, true));
        }
        // (an emptied backup an antivirus still holds is dropped like an old one: hidden now, deleted by the next Clean or Forget)
        try { Fs.Retry(() => Directory.Delete(from, true)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); Forget(); }
        return true;
    }

    // drops the backups (the game started and closed normally since the clean)
    public static void Forget()
    {
        // what an earlier Forget renamed but could not delete
        foreach (var old in Directory.Exists(Paths.Data) ? Directory.EnumerateDirectories(Paths.Data, "cache-backup.old-*") : [])
            try { Directory.Delete(old, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        if (!Directory.Exists(BackupRoot)) return;
        // renamed first, so the backup is gone at once even while Windows finishes deleting files an antivirus or indexer
        // still holds (deleting in place could leave the folder half there, or vanishing under the dropped mark)
        var gone = BackupRoot + ".old-" + DateTime.Now.Ticks;
        try { Fs.Retry(() => Directory.Move(BackupRoot, gone)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // the folder itself held: marked dropped now, deleted by the next Clean or Forget
            Engine.Log(e);
            try { Directory.CreateDirectory(BackupRoot); File.WriteAllText(DroppedMark, ""); } catch (Exception e2) when (e2 is IOException or UnauthorizedAccessException) { Engine.Log(e2); }
            return;
        }
        try { Fs.Retry(() => Directory.Delete(gone, true)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
