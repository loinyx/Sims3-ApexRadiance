namespace ApexSetup;

public enum CacheGroup { CreateASim, Scripts, Worlds, Thumbnails, Online, ErrorLogs }
public sealed record CacheFile(string Path, string Rel, long Size, CacheGroup Group);

// The cache files The Sims 3 rebuilds by itself in Documents. Clearing them is the community's first answer to CAS or
// thumbnail glitches and slow loads after mods change. Clean moves them to the launcher's data (never deletes): the copy is
// kept until the next game start that ends normally (SessionHistory.Add), and Restore puts it back.
public static class GameCaches
{
    // Each was seen in a real Documents folder; the list is the one Mod The Sims' and NRaas' "delete your caches" guides give.
    // DCCache is left out on purpose: its .dbc files hold the game's installed store content, not a cache.
    static readonly (string Name, CacheGroup Group)[] RootFiles =
    [
        ("CASPartCache.package", CacheGroup.CreateASim),       // CAS parts index, rebuilt at start
        ("compositorCache.package", CacheGroup.CreateASim),    // composited textures
        ("scriptCache.package", CacheGroup.Scripts),           // compiled script cache
        ("simCompositorCache.package", CacheGroup.CreateASim), // composited Sim textures
        ("socialCache.package", CacheGroup.Online),            // social network cache
    ];
    // (folder, pattern): every file in them is rebuilt or downloaded again
    static readonly (string Dir, string Pattern, CacheGroup Group)[] Folders =
    [
        ("WorldCaches", "*.package", CacheGroup.Worlds),    // one <world>_objects/_sims pair per world played, rebuilt on the next load of that world
        ("Thumbnails", "*.package", CacheGroup.Thumbnails), // CAS, object and downloads thumbnails
        ("FeaturedItems", "*", CacheGroup.Online),          // store thumbnails the game downloads again (some guides only; unverified that it matters)
        ("IGACache", "*", CacheGroup.Online),               // in-game advertising cache (empty on most installs)
    ];
    // what one click clears: every cache the game rebuilds. The error logs are cleared only when chosen.
    static readonly CacheGroup[] OneClick = [CacheGroup.CreateASim, CacheGroup.Scripts, CacheGroup.Worlds, CacheGroup.Thumbnails, CacheGroup.Online];

    public static string BackupRoot => Path.Combine(Paths.Data, "cache-backup");

    // groups: what to list (null: what one click clears)
    public static List<CacheFile> List(string docs, IReadOnlyCollection<CacheGroup>? groups = null)
    {
        groups ??= OneClick;
        var list = new List<CacheFile>();
        void Add(string p, CacheGroup g) { try { list.Add(new(p, Path.GetRelativePath(docs, p), new FileInfo(p).Length, g)); } catch (IOException) { } }
        foreach (var (name, g) in RootFiles.Where(x => groups.Contains(x.Group)))
            if (Path.Combine(docs, name) is var f && File.Exists(f)) Add(f, g);
        foreach (var (dir, pattern, g) in Folders.Where(x => groups.Contains(x.Group)))
            if (Path.Combine(docs, dir) is var d && Directory.Exists(d))
                try { foreach (var f in Directory.EnumerateFiles(d, pattern)) Add(f, g); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        // (only the logs Health is done with: it would lose the errors of the ones it still counts)
        if (groups.Contains(CacheGroup.ErrorLogs)) foreach (var f in ScriptErrors.OldLogs(docs)) Add(f, CacheGroup.ErrorLogs);
        return list;
    }

    // the newest backup, null when none is kept
    // (a backup marked dropped is gone for the player even when Windows still holds a file in it; it is deleted later)
    public static string? Backup => Backups().LastOrDefault();
    static string DroppedMark => Path.Combine(BackupRoot, "dropped");

    // the backups kept, oldest first
    static List<string> Backups() => Directory.Exists(BackupRoot) && !File.Exists(DroppedMark)
        ? Directory.EnumerateDirectories(BackupRoot).Where(d => !File.Exists(Path.Combine(d, "dropped"))).Order().ToList() : [];

    // moves the caches of those groups into a new dated backup folder; returns the bytes moved. Refuses while the game runs.
    // A move that fails puts back the ones already moved.
    public static long Clean(string docs, IReadOnlyCollection<CacheGroup>? groups = null)
    {
        Engine.CheckWritable();
        var files = List(docs, groups);
        if (files.Count == 0) return 0;
        // (a dropped backup Windows kept hold of: marked itself, so a new backup beside it does not bring it back)
        if (File.Exists(DroppedMark))
        {
            Forget();
            if (Directory.Exists(BackupRoot))
            {
                foreach (var d in Directory.EnumerateDirectories(BackupRoot)) File.WriteAllText(Path.Combine(d, "dropped"), "");
                Fs.Retry(() => File.Delete(DroppedMark));
            }
        }
        var older = Backups();
        var to = Path.Combine(BackupRoot, DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", System.Globalization.CultureInfo.InvariantCulture));
        Directory.CreateDirectory(to);
        File.WriteAllText(Path.Combine(to, "docs.txt"), docs); // (first: a clean cut off half way can still be restored)
        var moved = new List<(string From, string To)>();
        try
        {
            foreach (var f in files)
            {
                var dest = Path.Combine(to, f.Rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                Fs.Retry(() => File.Move(f.Path, dest));
                moved.Add((f.Path, dest));
            }
        }
        catch
        {
            foreach (var (from, dest) in Enumerable.Reverse(moved))
                try { Fs.Retry(() => File.Move(dest, from)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); } // (it stays in the backup, which Restore puts back)
            try { if (!Kept(to).Any()) Fs.DeleteClear(to); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            throw;
        }
        // an older backup (its game start never ended well) keeps what this clean did not touch; its copy of the rest is stale
        foreach (var d in older)
            try
            {
                if (DocsOf(d)?.Equals(docs, StringComparison.OrdinalIgnoreCase) != true) continue;
                foreach (var f in files)
                    if (Path.Combine(d, f.Rel) is var stale && File.Exists(stale)) File.Delete(stale);
                if (!Kept(d).Any()) Fs.DeleteClear(d);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        return files.Sum(f => f.Size);
    }

    static string? DocsOf(string backup) => Path.Combine(backup, "docs.txt") is var f && File.Exists(f) ? File.ReadAllText(f).Trim() : null;
    static IEnumerable<string> Kept(string backup) => Directory.EnumerateFiles(backup, "*", SearchOption.AllDirectories)
        .Where(f => Path.GetRelativePath(backup, f) != "docs.txt" && !f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)); // (what an antivirus adds for a moment; no cache ends so)

    // puts every backup back over whatever the game rebuilt since, the newest one last (last: only that one, to undo the last
    // Clean); false when there is none
    public static bool Restore(bool last = false)
    {
        Engine.CheckWritable();
        var all = Backups().Where(d => DocsOf(d) is { Length: > 0 }).ToList();
        if (last) all = all.TakeLast(1).ToList();
        foreach (var from in all)
        {
            var docs = DocsOf(from)!;
            foreach (var f in Kept(from).ToList())
            {
                var dest = Path.Combine(docs, Path.GetRelativePath(from, f));
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                Fs.Retry(() => File.Move(f, dest, true));
            }
        }
        // (an emptied backup an antivirus still holds is dropped like an old one: hidden now, deleted by the next Clean or Forget)
        foreach (var from in all)
            try { Fs.DeleteClear(from); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); Forget(); break; }
        return all.Count > 0;
    }

    // drops the backups (the game started and closed normally since the clean)
    public static void Forget()
    {
        // what an earlier Forget renamed but could not delete
        foreach (var old in Directory.Exists(Paths.Data) ? Directory.EnumerateDirectories(Paths.Data, "cache-backup.old-*") : [])
            try { Fs.DeleteClear(old); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
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
        try { Fs.DeleteClear(gone); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
