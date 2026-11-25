using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

public enum CopyKind { Launcher, Game, Now }

// A moment a save can go back to: a zip the launcher made (Note says why, when it made it by itself; Label and Pinned are the player's; Stored is what it takes on disk), the previous version
// the game keeps (<name>.sims3.backup), or the save as it is. When: the day the save was like that; Bytes: its size then.
public sealed record SavePoint(CopyKind Kind, string Path, DateTime When, long Bytes, string? Note, string? Label = null, bool Pinned = false, long Stored = 0);

// Copies of a save and what is done with whole save folders: the list of them, going back to one, bringing back a save
// that is gone or that the game only has a previous version of, keeping a save out of the game's sight, duplicate, rename, delete.
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
    public static Action<string> RecycleOwn = ModLibrary.RecycleQuiet; // the launcher's own, with nobody there to ask

    internal static string Sha(string path)
    {
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexString(SHA256.HashData(f));
    }
    // a save's file, read without standing in the way of the game writing it (a read that met a write does not compare)
    static FileStream Open(string path) => new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 20);
    static string Hash(Stream s, CancellationToken ct)
    {
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buf = new byte[1 << 20];
        for (int n; (n = s.Read(buf)) > 0;) { ct.ThrowIfCancellationRequested(); h.AppendData(buf, 0, n); }
        return Convert.ToHexString(h.GetHashAndReset());
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
    // check: each file compared with what the zip gives back. mods: the ones on, a list kept in the zip beside the save.
    internal static void Zip(string dir, string zip, string? note, bool check = true, List<string>? mods = null, CancellationToken ct = default, bool overwrite = false)
    {
        var part = zip + ".part";
        var top = Path.GetFileName(dir);
        string Name(string p) => top + "/" + Path.GetRelativePath(dir, p).Replace('\\', '/');
        try
        {
            var files = Directory.EnumerateFiles(dir, "*", Deep).Where(f => !Saves.Passing(f)).ToList();
            File.Delete(part); // (one a closed launcher left)
            using (var z = ZipFile.Open(part, ZipArchiveMode.Create))
            {
                z.Comment = note ?? "";
                foreach (var d in Directory.EnumerateDirectories(dir, "*", Deep).Where(d => !Directory.EnumerateFileSystemEntries(d).Any())) z.CreateEntry(Name(d) + "/");
                var buf = new byte[1 << 20];
                foreach (var f in files)
                {
                    var entry = z.CreateEntry(Name(f), CompressionLevel.Fastest);
                    if (File.GetLastWriteTime(f) is { Year: >= 1980 and <= 2107 } day) entry.LastWriteTime = day;
                    using var from = Open(f);
                    using var o = entry.Open();
                    for (int n; (n = from.Read(buf)) > 0;) { ct.ThrowIfCancellationRequested(); o.Write(buf, 0, n); }
                }
                if (mods is not null)
                {
                    using var o = new StreamWriter(z.CreateEntry(top + "/" + SaveBackups.ModsName, CompressionLevel.Fastest).Open());
                    o.Write(string.Join('\n', mods));
                }
            }
            using (var z = ZipFile.OpenRead(part))
            {
                var same = z.Entries.Count(e => !e.FullName.EndsWith('/') && !SaveBackups.IsMods(e)) == files.Count;
                foreach (var f in check ? files : [])
                {
                    ct.ThrowIfCancellationRequested();
                    if (!same || z.GetEntry(Name(f)) is not { } entry) { same = false; break; }
                    using var s = entry.Open();
                    using var of = Open(f);
                    same = Hash(s, ct) == Hash(of, ct);
                }
                if (!same) throw new SetupException("The copy of {0} did not match the save. Try again.", Path.GetFileNameWithoutExtension(top));
            }
            File.Move(part, zip, overwrite);
        }
        catch { try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
    }

    // The launcher's zips of this save, the game's previous version and the save now, oldest first (Now is last).
    // SaveHealth.History gives the sizes in between, for a curve.
    public static List<SavePoint> Timeline(string docs, Save s)
    {
        var points = Zips(docs, n => n.Equals(s.Name, StringComparison.OrdinalIgnoreCase)).Select(z => z.Point).ToList();
        if (Directory.Exists(s.Dir + ".backup") && SaveInfo.Read(s.Dir + ".backup") is { Bytes: > 0 } game)
            points.Add(new(CopyKind.Game, s.Dir + ".backup", game.LastPlayed, game.Bytes, null, Stored: game.Bytes));
        points.Sort((a, b) => a.When.CompareTo(b.When));
        points.Add(new(CopyKind.Now, s.Dir, s.LastPlayed, SaveInfo.Of(s).Bytes, null));
        return points;
    }

    // the launcher's zips of the saves it is asked for
    public static List<(string Name, SavePoint Point)> Zips(string docs, Func<string, bool> wanted)
    {
        var zips = new List<(string, SavePoint)>();
        try
        {
            foreach (var (name, zip, when) in ZipFiles(docs).Where(f => wanted(f.Name)))
                try
                {
                    var size = new FileInfo(zip).Length;
                    try
                    {
                        using var z = ZipFile.OpenRead(zip);
                        var (note, label, pinned) = SaveBackups.Said(z.Comment);
                        zips.Add((name, new(CopyKind.Launcher, zip, when, z.Entries.Where(e => !SaveBackups.IsMods(e)).Sum(e => e.Length), note, label, pinned, size)));
                    }
                    catch (InvalidDataException) { } // (a zip that does not open is not a copy to offer)
                    // (one another program holds for a moment is there; what it says is not known, so it counts as the player's own)
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException) { zips.Add((name, new(CopyKind.Launcher, zip, when, size, null, Stored: size))); }
                }
                catch (FileNotFoundException) { }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return zips;
    }

    // the launcher's zips by their names alone; a folder that can't be listed throws
    public static List<(string Name, string Path, DateTime When)> ZipFiles(string docs)
    {
        var list = new List<(string, string, DateTime)>();
        if (!Directory.Exists(Saves.BackupDir(docs))) return list;
        foreach (var zip in Directory.EnumerateFiles(Saves.BackupDir(docs), "*.zip"))
            if (Path.GetFileNameWithoutExtension(zip) is var n && n.Length >= 21 && n[^20] == ' ' && When(zip, n[^19..]) is { } when) list.Add((n[..^20], zip, when));
        return list;
    }

    // the day in a zip's name. Older launchers wrote it in the region's own calendar: a year that cannot be, or a day this
    // calendar has not, takes the file's own time.
    static DateTime? When(string zip, string stamp)
    {
        if (!System.Text.RegularExpressions.Regex.IsMatch(stamp, @"^\d{4}-\d\d-\d\d \d\d\.\d\d\.\d\d$")) return null;
        return DateTime.TryParseExact(stamp, "yyyy-MM-dd HH.mm.ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var when) && when.Year >= 2009 && when.Year <= DateTime.Now.Year + 1
            ? when : File.GetLastWriteTime(zip);
    }

    // the zips of saves that are gone (neither in the game nor kept out of it), newest first; Restore brings one back
    public static List<(Save Save, SavePoint Point)> Gone(string docs) =>
        Zips(docs, n => !Directory.Exists(Path.Combine(SavesDir(docs), n + ".sims3")) && !Directory.Exists(Path.Combine(StoredDir(docs), n + ".sims3")))
            .OrderByDescending(z => z.Point.When).Select(z => (new Save(Path.Combine(SavesDir(docs), z.Name + ".sims3"), z.Name, z.Point.When), z.Point)).ToList();

    // Each zip the launcher makes by itself is a whole save: past the newest of a save (three of those before a clean or
    // before going back, as many of the automatic ones as the player chose) and past the limit they go to the Recycle Bin.
    // Call after a change went through. One the player asked for, named or pinned is never touched (SaveBackups.Extra).
    public static void Thin(string docs, Save s)
    {
        try { lock (Gate) foreach (var p in SaveBackups.Extra(docs, s, KeepOwn)) SaveBackups.RemoveOwn(p); }
        catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { Engine.Log(e); } // (choices that can't be read remove nothing)
    }

    // a zip's files into a new folder. Untrusted input: the names are kept inside the folder, the count and the size are
    // bounded, and each file is read back and compared with what the zip gave.
    // only: the folder of the zip to take (the rest of it is left)
    internal static void Unzip(string zip, string to, CancellationToken ct, string? only = null)
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
                if (only is not null && !names[i].StartsWith(only, StringComparison.Ordinal)) continue;
                var rel = names[i][(only?.Length ?? top)..];
                if (rel.Length == 0 || rel == SaveBackups.ModsName) continue;
                var path = Path.GetFullPath(Path.Combine(to, rel));
                if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase) || rel.Contains(':')) throw Bad("a file name points outside the save");
                if (rel.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                var crc = 0u;
                using (var from = z.Entries[i].Open())
                using (var o = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    for (int n; (n = from.Read(buf)) > 0;)
                    {
                        if ((total += n) > MaxBytes) throw Bad("too large");
                        o.Write(buf, 0, n);
                        hash.AppendData(buf, 0, n);
                        crc = SaveBackups.Crc(crc, buf.AsSpan(0, n));
                    }
                    o.Flush(true);
                }
                if (crc != z.Entries[i].Crc32) throw new InvalidDataException(names[i]);
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
    // leaves the save as it was. The game's .backup is only read. A save that is gone (Gone) comes back under its name.
    public static Save Restore(string docs, Save s, SavePoint from, CancellationToken ct = default)
    {
        if (from.Kind == CopyKind.Now) return s;
        Engine.CheckWritable();
        Recover(docs); // (a save a cut-short change left aside goes home first: its name must not look free)
        lock (Gate)
        {
            string tmp = Work(s.Dir, "new"), old = Work(s.Dir, "old");
            string? kept = null;
            try
            {
                if (from.Kind == CopyKind.Launcher) Unzip(from.Path, tmp, ct); else CopyDir(from.Path, tmp, ct);
                if (!Directory.EnumerateFiles(tmp, "*.nhd").Any()) throw new SetupException("This copy can't be used: {0}", "no world file in it");
                FailPoint("extract");
                var here = Directory.Exists(s.Dir);
                if (here) kept = Saves.Backup(s, docs, BeforeRestore);
                ct.ThrowIfCancellationRequested();
                Engine.CheckWritable();
                if (here) Fs.Retry(() => Directory.Move(s.Dir, old));
                try { FailPoint("swap"); Fs.Retry(() => Directory.Move(tmp, s.Dir)); }
                catch (Exception e) when (here)
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
            try { if (Directory.Exists(old)) Recycle(old); } catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { Engine.Log(e); }
            Tidy(Path.GetDirectoryName(s.Dir)!);
            Thin(docs, s);
            return SaveOf(s.Dir);
        }
    }

    // A point as a save of its own, beside the one that is there.
    public static Save RestoreAs(string docs, Save s, SavePoint from, CancellationToken ct = default)
    {
        Engine.CheckWritable();
        var made = Bring(docs, s.Name, to => { if (from.Kind == CopyKind.Launcher) Unzip(from.Path, to, ct); else CopyDir(from.Path, to, ct); });
        try { if (made.Name != s.Name) SaveStars.Renamed(s.Name, made.Name, keep: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); }
        return made;
    }

    // a save laid out and checked beside the others, then in the game under its name or the next free one
    internal static Save Bring(string docs, string name, Action<string> fill)
    {
        Recover(docs); // (a save a cut-short change left aside goes home first: its name must not look free)
        lock (Gate)
        {
            var root = SavesDir(docs);
            Directory.CreateDirectory(root);
            name = ModLibrary.CleanName(name);
            // (nor the name of a save that is gone: its backups are not this one's)
            var kept = ZipFiles(docs).Select(z => z.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
            if (Taken(root, name) || kept.Contains(name)) name = FreeName(root, name, kept);
            var to = Path.Combine(root, CleanIn(root, name) + ".sims3");
            var tmp = Work(to, "new");
            try
            {
                fill(tmp);
                if (!Directory.Exists(tmp) || !Directory.EnumerateFiles(tmp, "*.nhd").Any()) throw new SetupException("This copy can't be used: {0}", "no world file in it");
                FailPoint("copy");
                Engine.CheckWritable();
                Fs.Retry(() => Directory.Move(tmp, to));
            }
            catch { Drop(tmp); throw; }
            finally { Tidy(root); }
            return SaveOf(to);
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
            // (a backup cut short; one being made is newer)
            if (Directory.Exists(Saves.BackupDir(docs)))
                foreach (var f in Directory.EnumerateFiles(Saves.BackupDir(docs), "*.zip.part").Where(f => File.GetLastWriteTime(f) < DateTime.Now.AddDays(-1)).ToList()) File.Delete(f);
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
    static string FreeName(string root, string name, HashSet<string>? not = null)
    {
        for (var i = 2; ; i++)
            if (!Taken(root, $"{name} ({i})") && not?.Contains($"{name} ({i})") != true) return $"{name} ({i})";
    }

    // a name a save can take, trimmed, or why not (SetupException)
    public static string CleanName(string docs, string name) => CleanIn(SavesDir(docs), name);

    // a name whose backups would mix with another save's: one kept on the other side (in the game, or out of it), or one that
    // is gone and left its backups
    static HashSet<string> Kept(string docs, string root) =>
        ZipFiles(docs).Select(z => z.Name).Concat(Saves.In(Path.GetFullPath(root).Equals(Path.GetFullPath(SavesDir(docs)), StringComparison.OrdinalIgnoreCase) ? StoredDir(docs) : SavesDir(docs)).Select(x => x.Name))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
    static string NewName(string docs, string root, string name)
    {
        name = CleanIn(root, name);
        return Kept(docs, root).Contains(name) ? throw new SetupException("Backups of a save named {0} are kept. Choose another name.", name) : name;
    }
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
            name = name is null ? FreeName(root, s.Name, Kept(docs, root)) : NewName(docs, root, name);
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
            name = name.Trim().Equals(s.Name, StringComparison.OrdinalIgnoreCase) ? ModLibrary.CleanName(name) : NewName(docs, root, name);
            if (name == s.Name) return s;
            // (its backups could not follow it, and would be left as those of a save that is gone)
            if (BackupChoices.Load().FolderOf(docs) is { } far && !Directory.Exists(far)) throw new SetupException("The backup folder isn't available: {0}", far);
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
        Try(() => BackupChoices.Change(c => { if (c.Saves.Remove(from, out var own)) c.Saves[to] = own; }));
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
