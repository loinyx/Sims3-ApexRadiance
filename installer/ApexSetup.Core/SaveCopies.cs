using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

public enum CopyKind { Launcher, Game, Now }

// A moment a save can go back to: a zip the launcher made (Note says why, when it made it by itself), the previous version
// the game keeps (<name>.sims3.backup), or the save as it is. When: the day the save was like that; Bytes: its size then.
public sealed record SavePoint(CopyKind Kind, string Path, DateTime When, long Bytes, string? Note);

// Copies of a save and what is done with whole save folders: the timeline, going back to a point, bringing back a save
// the game only has a previous version of, keeping a save out of the game's sight, duplicate, rename, delete.
// Nothing here writes over a save: a folder that replaces one is copied and checked beside it first, the one it replaces
// is zipped (and checked) before it leaves, and a removed folder goes to the Recycle Bin. All of it is refused while the
// game runs.
public static class SaveCopies
{
    public const string BeforeClean = "before-clean", BeforeRestore = "before-restore"; // a zip's Note
    public const string StoredName = "Saves kept (Apex Launcher)";
    public static string StoredDir(string docs) => Path.Combine(docs, StoredName);
    static string SavesDir(string docs) => Path.Combine(docs, "Saves");
    // (folders on their way in or out wait inside the saves folder: the same drive as the saves, whatever the folder links
    // to. Every listing here and in Apex takes only *.sims3 folders; unverified that the game does the same, though it
    // does not list its own <name>.sims3.backup folders)
    static string WorkDir(string root) => Path.Combine(root, ".apex-work");
    const int KeepOwn = 3;

    const int MaxFiles = 10_000;        // (a save has under ten files)
    const long MaxBytes = 16L << 30;
    static readonly EnumerationOptions Deep = new() { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint };

    internal static readonly object Gate = new(); // one change to the saves at a time
    // test hooks: called at "extract", "swap", "copy" and "move"; and the Recycle Bin (tests must not fill the real one)
    public static Action<string> FailPoint = _ => { };
    public static Action<string> Recycle = ModLibrary.Recycle;

    internal static string Sha(string path)
    {
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexString(SHA256.HashData(f));
    }
    static Save SaveOf(string dir) => new(dir, Path.GetFileNameWithoutExtension(dir), SaveInfo.Read(dir).LastPlayed);
    static void Drop(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
    static void DropEmpty(string dir)
    {
        try { if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
    static void Tidy(string root) => DropEmpty(WorkDir(root));
    // the drive a folder is really on (the Saves folder can be a link to another one)
    static string Drive(string dir) => Path.GetPathRoot(Directory.ResolveLinkTarget(dir, true)?.FullName ?? Path.GetFullPath(dir))!;
    static string Work(string dir, string what)
    {
        var work = WorkDir(Path.GetDirectoryName(dir)!);
        Directory.CreateDirectory(work);
        return Path.Combine(work, $"{Path.GetFileName(dir)}.{what}-{Guid.NewGuid():N}");
    }

    // ---------- copies ----------
    // A zip of a save folder that is known to be whole: written beside as .part, opened again and every file compared by
    // hash with what the zip gives back, then moved in. A save that changed meanwhile, a locked file or a full disk leaves
    // no zip.
    internal static void Zip(string dir, string zip, string? note)
    {
        var part = zip + ".part";
        var top = Path.GetFileName(dir);
        string Name(string p) => top + "/" + Path.GetRelativePath(dir, p).Replace('\\', '/');
        try
        {
            var files = Directory.EnumerateFiles(dir, "*", Deep).Where(f => !Saves.Passing(f)).ToList();
            using (var z = ZipFile.Open(part, ZipArchiveMode.Create))
            {
                z.Comment = note ?? "";
                foreach (var d in Directory.EnumerateDirectories(dir, "*", Deep).Where(d => !Directory.EnumerateFileSystemEntries(d).Any())) z.CreateEntry(Name(d) + "/");
                foreach (var f in files) z.CreateEntryFromFile(f, Name(f), CompressionLevel.Fastest);
            }
            using (var z = ZipFile.OpenRead(part))
            {
                var same = z.Entries.Count(e => !e.FullName.EndsWith('/')) == files.Count;
                foreach (var f in files)
                {
                    if (!same || z.GetEntry(Name(f)) is not { } entry) { same = false; break; }
                    using var s = entry.Open();
                    same = Convert.ToHexString(SHA256.HashData(s)) == Sha(f);
                }
                if (!same) throw new SetupException("The copy of {0} did not match the save. Try again.", Path.GetFileNameWithoutExtension(top));
            }
            File.Move(part, zip);
        }
        catch { try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
    }

    // The launcher's zips of this save, the game's previous version and the save now, oldest first (Now is last).
    // SaveHealth.History gives the sizes in between, for a curve.
    public static List<SavePoint> Timeline(string docs, Save s)
    {
        var points = new List<SavePoint>();
        try
        {
            if (Directory.Exists(Saves.BackupDir(docs)))
                foreach (var zip in Directory.EnumerateFiles(Saves.BackupDir(docs), "*.zip"))
                {
                    var n = Path.GetFileNameWithoutExtension(zip);
                    if (n.Length != s.Name.Length + 20 || !n.StartsWith(s.Name + " ", StringComparison.OrdinalIgnoreCase)
                        || !DateTime.TryParseExact(n[^19..], "yyyy-MM-dd HH.mm.ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var when)) continue;
                    try
                    {
                        using var z = ZipFile.OpenRead(zip);
                        points.Add(new(CopyKind.Launcher, zip, when, z.Entries.Sum(e => e.Length), z.Comment is { Length: > 0 } c ? c : null));
                    }
                    catch (InvalidDataException) { } // (a zip that does not open is not a copy to offer)
                }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        if (Directory.Exists(s.Dir + ".backup") && SaveInfo.Read(s.Dir + ".backup") is { Bytes: > 0 } game)
            points.Add(new(CopyKind.Game, s.Dir + ".backup", game.LastPlayed, game.Bytes, null));
        points.Sort((a, b) => a.When.CompareTo(b.When));
        points.Add(new(CopyKind.Now, s.Dir, s.LastPlayed, SaveInfo.Of(s).Bytes, null));
        return points;
    }

    // Each zip the launcher makes by itself (before a clean, before going back) is a whole save: past the newest three of
    // a save they go to the Recycle Bin. Call after a change went through. A copy the player asked for carries no note
    // and is never touched.
    internal static void Thin(string docs, Save s)
    {
        try { foreach (var p in Timeline(docs, s).Where(p => p.Kind == CopyKind.Launcher && p.Note is BeforeClean or BeforeRestore).Reverse().Skip(KeepOwn)) Recycle(p.Path); }
        catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // a zip's files into a new folder. Untrusted input: the names are kept inside the folder, the count and the size are
    // bounded, and each file is read back and compared with what the zip gave.
    static void Unzip(string zip, string to, CancellationToken ct)
    {
        static SetupException Bad(string why) => new("This copy can't be used: {0}", why);
        try
        {
            using var z = ZipFile.OpenRead(zip);
            if (z.Entries.Count > MaxFiles) throw Bad("too many files");
            // (the zip holds the save's folder at the top: its name is dropped, the save may have been renamed since)
            var names = z.Entries.Select(e => e.FullName.Replace('\\', '/')).ToList();
            var top = names.Count > 0 && names[0].IndexOf('/') is var cut and > 0 && names.All(n => n.StartsWith(names[0][..(cut + 1)], StringComparison.Ordinal)) ? cut + 1 : 0;
            var root = Path.GetFullPath(to) + Path.DirectorySeparatorChar;
            Directory.CreateDirectory(to);
            long total = 0;
            var buf = new byte[1 << 16];
            for (var i = 0; i < names.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                var rel = names[i][top..];
                if (rel.Length == 0) continue;
                var path = Path.GetFullPath(Path.Combine(to, rel));
                if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase) || rel.Contains(':')) throw Bad("a file name points outside the save");
                if (rel.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                using (var from = z.Entries[i].Open())
                using (var o = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    for (int n; (n = from.Read(buf)) > 0;)
                    {
                        if ((total += n) > MaxBytes) throw Bad("too large");
                        o.Write(buf, 0, n);
                        hash.AppendData(buf, 0, n);
                    }
                    o.Flush(true);
                }
                if (Sha(path) != Convert.ToHexString(hash.GetHashAndReset())) throw new IOException("a restored file did not read back as written");
                File.SetLastWriteTime(path, z.Entries[i].LastWriteTime.DateTime); // (the save is as old as the copy says)
            }
        }
        catch (InvalidDataException e) { Engine.Log(e); throw Bad("the file is damaged"); }
    }

    // a folder copied whole, each file read back and compared
    static void CopyDir(string from, string to, CancellationToken ct)
    {
        Directory.CreateDirectory(to);
        foreach (var d in Directory.EnumerateDirectories(from, "*", Deep)) Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, d)));
        foreach (var f in Directory.EnumerateFiles(from, "*", Deep).Where(f => !Saves.Passing(f)))
        {
            ct.ThrowIfCancellationRequested();
            var t = Path.Combine(to, Path.GetRelativePath(from, f));
            File.Copy(f, t);
            if (Sha(f) != Sha(t)) throw new IOException("a copied file did not read back as written");
        }
    }

    // The save as it was at a point. The copy is laid out and checked beside the saves first, the save of now is zipped
    // (one more point on the timeline, so nothing is lost), then the two folders change places. A failure at any step
    // leaves the save as it was. The game's .backup is only read.
    public static Save Restore(string docs, Save s, SavePoint from, CancellationToken ct = default)
    {
        if (from.Kind == CopyKind.Now) return s;
        Engine.CheckWritable();
        lock (Gate)
        {
            string tmp = Work(s.Dir, "new"), old = Work(s.Dir, "old");
            string? kept = null;
            try
            {
                if (from.Kind == CopyKind.Launcher) Unzip(from.Path, tmp, ct); else CopyDir(from.Path, tmp, ct);
                if (!Directory.EnumerateFiles(tmp, "*.nhd").Any()) throw new SetupException("This copy can't be used: {0}", "no world file in it");
                FailPoint("extract");
                kept = Saves.Backup(s, docs, BeforeRestore);
                ct.ThrowIfCancellationRequested();
                Engine.CheckWritable();
                Fs.Retry(() => Directory.Move(s.Dir, old));
                try { FailPoint("swap"); Fs.Retry(() => Directory.Move(tmp, s.Dir)); }
                catch (Exception e)
                {
                    try { Fs.Retry(() => Directory.Move(old, s.Dir)); }
                    catch (Exception x) when (x is IOException or UnauthorizedAccessException) { Engine.Log(e); throw new SetupException("The save could not be put back in place. It is in {0}.", old); }
                    throw;
                }
            }
            catch
            {
                Drop(tmp);
                // (nothing changed: the copy made for this restore is not a point anyone asked for)
                if (kept is not null && Directory.Exists(s.Dir)) try { File.Delete(kept); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
                Tidy(Path.GetDirectoryName(s.Dir)!);
                throw;
            }
            // (the folder that left is in the zip just made, whole and checked; one Windows can't take now goes at the next start)
            try { Recycle(old); } catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { Engine.Log(e); }
            Tidy(Path.GetDirectoryName(s.Dir)!);
            Thin(docs, s);
            return SaveOf(s.Dir);
        }
    }

    // What a change that was cut short (the launcher closed, the power went) left behind, put right: a save folder or a
    // save file that was moved aside goes back when nothing took its place, and to the Recycle Bin when something did
    // (what took its place was checked before the swap); copies that never got in place are removed. Call at start-up.
    // False: it did not run, or not to the end (ask again later).
    public static bool Recover(string docs)
    {
        try { Engine.CheckWritable(); } catch (InvalidOperationException) { return false; } // (a test run, the game running: next time)
        if (!Monitor.TryEnter(Gate)) return false;
        try
        {
            foreach (var root in new[] { SavesDir(docs), StoredDir(docs) })
            {
                if (Directory.Exists(WorkDir(root)))
                    foreach (var d in Directory.EnumerateDirectories(WorkDir(root)).ToList())
                    {
                        var n = Path.GetFileName(d);
                        if (n.LastIndexOf(".old-", StringComparison.Ordinal) is var at and > 0)
                        {
                            var home = Path.Combine(root, n[..at]);
                            if (Directory.Exists(home)) Recycle(d); else Fs.Retry(() => Directory.Move(d, home));
                        }
                        else Drop(d);
                    }
                Tidy(root);
                if (!Directory.Exists(root)) continue;
                foreach (var dir in Directory.EnumerateDirectories(root, "*.sims3"))
                {
                    foreach (var f in Directory.EnumerateFiles(dir, "*" + SaveClean.NewSuffix).ToList()) Fs.Retry(() => File.Delete(f));
                    foreach (var old in Directory.EnumerateFiles(dir, "*" + SaveClean.OldSuffix).ToList())
                    {
                        var file = old[..^SaveClean.OldSuffix.Length];
                        if (File.Exists(file)) Recycle(old); else Fs.Retry(() => File.Move(old, file));
                    }
                }
            }
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); return false; }
        finally { Monitor.Exit(Gate); }
    }

    // ---------- whole saves ----------
    // saves the game no longer lists but still keeps the previous version of (<name>.sims3.backup with no <name>.sims3);
    // SaveInfo.Read and SaveInfo.Photo work on their Dir
    public static List<Save> Lost(string docs)
    {
        var list = new List<Save>();
        try
        {
            if (Directory.Exists(SavesDir(docs)))
                foreach (var d in Directory.EnumerateDirectories(SavesDir(docs), "*.sims3.backup"))
                    if (!Directory.Exists(d[..^7]) && SaveInfo.Read(d) is { Bytes: > 0 } i) list.Add(new(d, Path.GetFileName(d)[..^13], i.LastPlayed));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return list.OrderByDescending(s => s.LastPlayed).ToList();
    }

    // a lost save back in the game under its own name: a checked copy of the .backup, which stays
    public static Save Rescue(string docs, Save lost, CancellationToken ct = default)
    {
        Engine.CheckWritable();
        Recover(docs); // (a save a cut-short change left aside goes home first: its name must not look free)
        lock (Gate)
        {
            var to = Path.Combine(SavesDir(docs), lost.Name + ".sims3");
            if (Directory.Exists(to)) throw new SetupException("A save named {0} already exists.", lost.Name);
            return CopyAs(lost.Dir, to, ct);
        }
    }

    static Save CopyAs(string from, string to, CancellationToken ct)
    {
        var tmp = Work(to, "new");
        try
        {
            CopyDir(from, tmp, ct);
            FailPoint("copy");
            Fs.Retry(() => Directory.Move(tmp, to));
        }
        catch { Drop(tmp); throw; }
        finally { Tidy(Path.GetDirectoryName(to)!); }
        return SaveOf(to);
    }

    static bool Taken(string root, string name) => Directory.Exists(Path.Combine(root, name + ".sims3")) || Directory.Exists(Path.Combine(root, name + ".sims3.backup"));
    static string FreeName(string root, string name)
    {
        for (var i = 2; ; i++)
            if (!Taken(root, $"{name} ({i})")) return $"{name} ({i})";
    }

    // a name a save can take, trimmed, or why not (SetupException)
    public static string CleanName(string docs, string name) => CleanIn(SavesDir(docs), name);
    static string CleanIn(string root, string name)
    {
        name = ModLibrary.CleanName(name);
        // (the game is 32-bit: it can't open a save whose files' paths pass 259 characters; 80 covers the longest file name)
        if (Path.Combine(root, name + ".sims3.backup").Length + 80 > 259) throw new SetupException("That name is too long for a save.");
        if (Taken(root, name)) throw new SetupException("A save named {0} already exists.", name);
        return name;
    }

    // a second, independent save with the same content (the game's previous version is not copied)
    public static Save Duplicate(string docs, Save s, string? name = null, CancellationToken ct = default)
    {
        Engine.CheckWritable();
        lock (Gate)
        {
            var root = SavesDir(docs);
            name = name is null ? FreeName(root, s.Name) : CleanIn(root, name);
            var copy = CopyAs(s.Dir, Path.Combine(root, name + ".sims3"), ct);
            try { SaveStars.Renamed(s.Name, name, keep: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); }
            return copy;
        }
    }

    // The folder is the save's name (nothing inside repeats it), so a rename moves the folder, the game's previous version
    // with it, and what the launcher keeps by name: its zips, the size history, the stars and the Gallery's household.
    public static Save Rename(string docs, Save s, string name)
    {
        Engine.CheckWritable();
        lock (Gate)
        {
            var root = Path.GetDirectoryName(s.Dir)!;
            name = name.Trim().Equals(s.Name, StringComparison.OrdinalIgnoreCase) ? ModLibrary.CleanName(name) : CleanIn(root, name);
            if (name == s.Name) return s;
            return Move(docs, s, root, name);
        }
    }

    // out of the game's sight (the game lists only Saves) and back; both keep the game's previous version with the save
    public static List<Save> Stored(string docs) => Saves.In(StoredDir(docs));
    public static Save Store(string docs, Save s) { Engine.CheckWritable(); lock (Gate) return Move(docs, s, StoredDir(docs), null); }
    public static Save Unstore(string docs, Save s) { Engine.CheckWritable(); lock (Gate) return Move(docs, s, SavesDir(docs), null); }

    // (name null: its own, or the next free one when that is taken there)
    static Save Move(string docs, Save s, string root, string? name)
    {
        Directory.CreateDirectory(root);
        try
        {
            // (a folder can't be moved to another drive in one step, and a copy would not be this safe)
            if (!Drive(root).Equals(Drive(Path.GetDirectoryName(s.Dir)!), StringComparison.OrdinalIgnoreCase))
                throw new SetupException("A save can't be kept out of the game or put back while the Saves folder is on another drive.");
            name ??= Taken(root, s.Name) ? FreeName(root, s.Name) : s.Name;
            var to = Path.Combine(root, name + ".sims3");
            Fs.Retry(() => Directory.Move(s.Dir, to));
            if (Directory.Exists(s.Dir + ".backup"))
                try { FailPoint("move"); Fs.Retry(() => Directory.Move(s.Dir + ".backup", to + ".backup")); }
                catch { Fs.Retry(() => Directory.Move(to, s.Dir)); throw; }
            // (another save still has the old name, in the game or out of it: the copies, the history and the Gallery's
            // household can't be told apart by name and stay with it; the stars go to both, so no starred memory is left
            // without its protection)
            if (!name.Equals(s.Name, StringComparison.OrdinalIgnoreCase) && new[] { SavesDir(docs), StoredDir(docs) }.Any(r => Directory.Exists(Path.Combine(r, s.Name + ".sims3"))))
                try { SaveStars.Renamed(s.Name, name, keep: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); }
            else if (name != s.Name) Follow(docs, s.Name, name);
            return SaveOf(to);
        }
        finally { DropEmpty(StoredDir(docs)); }
    }

    // what the launcher keeps by save name follows the save (each on its own: one that fails is logged, the rest still move)
    static void Follow(string docs, string from, string to)
    {
        static void Try(Action a) { try { a(); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); } }
        Try(() =>
        {
            var dir = Saves.BackupDir(docs);
            if (!Directory.Exists(dir)) return;
            foreach (var zip in Directory.EnumerateFiles(dir, "*.zip").ToList())
                if (Path.GetFileName(zip) is var n && n.Length == from.Length + 24 && n.StartsWith(from + " ", StringComparison.OrdinalIgnoreCase)
                    && Path.Combine(dir, to + n[from.Length..]) is var z && !File.Exists(z)) File.Move(zip, z);
        });
        Try(() => SaveHealth.Renamed(from, to));
        Try(() => SaveStars.Renamed(from, to));
        Try(() =>
        {
            bool Is(string? h) => string.Equals(h, from, StringComparison.OrdinalIgnoreCase);
            var g = Gallery.Load();
            var changed = false;
            foreach (var i in g.Pictures.Values.Where(i => Is(i.Household))) { i.Household = to; changed = true; }
            foreach (var c in g.Custom.Where(c => Is(c.Household))) { c.Household = to; changed = true; }
            if (g.Albums.Remove(from, out var look)) { g.Albums.TryAdd(to, look); changed = true; }
            if (Is(g.LastAlbum)) { g.LastAlbum = to; changed = true; }
            if (changed) Gallery.Save(g);
        });
    }

    // a save (or a lost one's .backup) to the Recycle Bin, whole
    public static void Delete(Save s)
    {
        Engine.CheckWritable();
        lock (Gate)
        {
            Recycle(s.Dir);
            // (out of the game nothing shows the game's previous version alone, and it would hold the name: it goes too)
            var kept = Path.GetDirectoryName(s.Dir)!;
            if (Path.GetFileName(kept) != StoredName) return;
            if (Directory.Exists(s.Dir + ".backup")) Recycle(s.Dir + ".backup");
            DropEmpty(kept);
        }
    }
}
