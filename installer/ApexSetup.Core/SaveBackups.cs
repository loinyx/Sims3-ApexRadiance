using System.IO.Compression;
using System.Text;
using System.Text.Json;

namespace ApexSetup;

// What the player chose about backups of saves, in %LOCALAPPDATA%\Apex Launcher\backups.json: for every save, and for one
// (null: as every save). Keep and LimitGb only ever remove what the launcher made by itself.
public sealed class BackupTimes
{
    public bool? OnClose { get; set; }
    public bool? BeforePlay { get; set; }
    public bool? Daily { get; set; }
    public int? Keep { get; set; }
    public bool Off { get; set; } // (none by itself)
}

public sealed class BackupChoices
{
    public bool OnClose { get; set; } = true;
    public bool BeforePlay { get; set; }
    public bool Daily { get; set; }
    public int Keep { get; set; } = 5;      // of each save
    public int LimitGb { get; set; } = 10;  // of all; 0: no limit
    public bool Check { get; set; } = true;
    public bool RememberMods { get; set; } = true;
    public Dictionary<string, string> Folders { get; set; } = new();    // the game's Documents folder -> where its backups go
    public Dictionary<string, BackupTimes> Saves { get; set; } = new(); // by save name

    static string FilePath => Path.Combine(Paths.Data, "backups.json");
    static readonly object Gate = new();

    // the choices as they are, or the error that kept them from being read (what removes backups must not go by the defaults)
    internal static BackupChoices Sure() { lock (Gate) return Read(); }

    static BackupChoices Read()
    {
        if (!File.Exists(FilePath)) return new();
        var c = JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.BackupChoices) ?? new();
        c.Folders = new(c.Folders ?? [], StringComparer.OrdinalIgnoreCase);
        c.Saves = new((c.Saves ?? []).Where(s => s.Value is not null), StringComparer.OrdinalIgnoreCase);
        (c.Keep, c.LimitGb) = (Math.Clamp(c.Keep, 1, 50), Math.Clamp(c.LimitGb, 0, 10_000));
        return c;
    }

    public static BackupChoices Load()
    {
        try { lock (Gate) return Read(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { Engine.Log(e); return new(); }
    }

    // (a file that can't be opened now is not written over; one that says nothing readable is)
    public static void Change(Action<BackupChoices> edit)
    {
        lock (Gate)
        {
            BackupChoices c;
            try { c = Read(); } catch (JsonException e) { Engine.Log(e); c = new(); }
            edit(c);
            Directory.CreateDirectory(Paths.Data);
            Fs.Retry(() => Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(c, SetupJson.Default.BackupChoices)));
        }
    }

    static string Key(string docs) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(docs));
    public string? FolderOf(string docs) => Folders.Count == 0 || docs.Length == 0 ? null : Folders.GetValueOrDefault(Key(docs));
    public void SetFolder(string docs, string? folder) { if (folder is null) Folders.Remove(Key(docs)); else Folders[Key(docs)] = folder; }

    internal IEnumerable<string> Others(string docs) => Folders.Where(f => !f.Key.Equals(Key(docs), StringComparison.OrdinalIgnoreCase)).Select(f => Path.TrimEndingDirectorySeparator(Path.GetFullPath(f.Value)));

    // what holds for one save
    public (bool OnClose, bool BeforePlay, bool Daily, int Keep) Of(string save) => Saves.GetValueOrDefault(save) is not { } s ? (OnClose, BeforePlay, Daily, Keep)
        : s.Off ? (false, false, false, Math.Clamp(s.Keep ?? Keep, 1, 50)) : (s.OnClose ?? OnClose, s.BeforePlay ?? BeforePlay, s.Daily ?? Daily, Math.Clamp(s.Keep ?? Keep, 1, 50));
}

public enum BackupMoment { Close, Play, Day }

// The backups themselves: what is written with a zip (why, its name, pinned: in the zip's own comment, so they go wherever
// the file goes; the mods that were on: a file inside), the ones the launcher makes by itself and how many of those stay,
// the folder, and zips taken to and brought from somewhere else. SaveCopies makes and restores them.
public static class SaveBackups
{
    public const string AutoClose = "auto-close", AutoPlay = "auto-play", AutoDay = "auto-day"; // a zip's Note
    public const string ModsName = "apex-mods.txt"; // beside the save's files in the zip; no restore writes it out

    public static bool IsAuto(string? note) => note is AutoClose or AutoPlay or AutoDay;
    // made by the launcher by itself, and neither named nor pinned since
    public static bool IsOwn(SavePoint p) => p.Kind == CopyKind.Launcher && IsOwn(p.Note) && !p.Pinned && p.Label is null;
    static bool IsOwn(string? note) => IsAuto(note) || note is SaveCopies.BeforeClean or SaveCopies.BeforeRestore;

    // ---------- what a zip says ----------

    // the comment: why, then "name=" and "pin" lines (an older launcher reads the whole as a note it does not know, and
    // leaves the zip alone)
    internal static string Comment(string? note, string? label, bool pinned) =>
        string.Join('\n', new[] { note ?? "", label is { Length: > 0 } ? "name=" + label : null, pinned ? "pin" : null }.OfType<string>());

    internal static (string? Note, string? Label, bool Pinned) Said(string? comment)
    {
        var lines = (comment ?? "").Split('\n');
        var label = lines.Skip(1).FirstOrDefault(l => l.StartsWith("name=", StringComparison.Ordinal))?[5..];
        return (lines[0].Length > 0 ? lines[0] : null, label is { Length: > 0 } ? label : null, lines.Skip(1).Contains("pin"));
    }

    // Its name and whether it is pinned, written over the comment at the zip's end: nothing before it is touched, and the
    // file's day stays.
    public static void Mark(SavePoint p, string? label, bool pinned)
    {
        Engine.CheckDry();
        if (p.Kind != CopyKind.Launcher) return;
        label = label?.Replace('\r', ' ').Replace('\n', ' ').Trim();
        if (label?.Length > 120) label = label[..120];
        lock (SaveCopies.Gate)
        {
            var day = File.GetLastWriteTimeUtc(p.Path);
            using (var f = new FileStream(p.Path, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            {
                var tail = new byte[Math.Min(f.Length, 22 + 65535)];
                f.Position = f.Length - tail.Length;
                f.ReadExactly(tail);
                var at = tail.Length - 22;
                // (the end record: its signature, and a comment that reaches exactly to the end of the file)
                while (at >= 0 && !(tail[at] == 0x50 && tail[at + 1] == 0x4B && tail[at + 2] == 5 && tail[at + 3] == 6 && (tail[at + 20] | tail[at + 21] << 8) == tail.Length - at - 22)) at--;
                if (at < 0) throw new SetupException("This backup is damaged.");
                var note = Said(Encoding.UTF8.GetString(tail, at + 22, tail.Length - at - 22)).Note;
                var text = Encoding.UTF8.GetBytes(Comment(note, label, pinned));
                var end = new byte[2 + text.Length];
                (end[0], end[1]) = ((byte)text.Length, (byte)(text.Length >> 8));
                text.CopyTo(end, 2);
                f.Position = f.Length - tail.Length + at + 20;
                f.Write(end);
                f.SetLength(f.Position);
                f.Flush(true);
            }
            File.SetLastWriteTimeUtc(p.Path, day);
        }
    }

    // The mods the game loads, by file name; a merged file counts as the mods in it, so merging or splitting changes
    // nothing here.
    public static List<string> ModsNow(string docs)
    {
        var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var dir = Path.Combine(docs, "Mods", "Packages");
            if (Directory.Exists(dir))
                foreach (var f in Directory.EnumerateFiles(dir, "*.package", new EnumerationOptions { RecurseSubdirectories = true }))
                    if (ModMerge.OriginalsOf(docs, Path.GetRelativePath(docs, f)) is { } parts) foreach (var p in parts) names.Add(Path.GetFileName(p));
                    else names.Add(Path.GetFileName(f));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        return [.. names];
    }

    // the mods that were on that day and are not now; empty too when the zip does not say
    public static List<string> MissingMods(string docs, SavePoint p)
    {
        if (p.Kind != CopyKind.Launcher) return [];
        try
        {
            using var z = ZipFile.OpenRead(p.Path);
            if (z.Entries.FirstOrDefault(IsMods) is not { Length: < 16 << 20 } e) return [];
            using var r = new StreamReader(e.Open(), Encoding.UTF8);
            var now = ModsNow(docs).ToHashSet(StringComparer.OrdinalIgnoreCase);
            return r.ReadToEnd().Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Where(m => !now.Contains(m)).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { Engine.Log(e); return []; }
    }

    internal static bool IsMods(ZipArchiveEntry e) => e.Name == ModsName && e.FullName.Count(c => c is '/' or '\\') == 1;

    // every file in it is read and compared with its checksum, and it has a world; why not, as a SetupException
    public static void Verify(SavePoint p, CancellationToken ct = default)
    {
        try
        {
            using var z = ZipFile.OpenRead(p.Path);
            var buf = new byte[1 << 16];
            foreach (var e in z.Entries)
            {
                using var s = e.Open();
                var crc = 0u;
                for (int n; (n = s.Read(buf)) > 0;) { ct.ThrowIfCancellationRequested(); crc = Crc(crc, buf.AsSpan(0, n)); }
                if (crc != e.Crc32) throw new InvalidDataException(e.FullName);
            }
            if (!z.Entries.Any(e => e.Name.EndsWith(".nhd", StringComparison.OrdinalIgnoreCase))) throw new InvalidDataException("no world file");
        }
        catch (InvalidDataException e) { Engine.Log(e); throw new SetupException("This backup is damaged."); }
    }

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(i => { var c = (uint)i; for (var k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ c >> 1 : c >> 1; return c; }).ToArray();
    internal static uint Crc(uint crc, ReadOnlySpan<byte> data)
    {
        crc = ~crc;
        foreach (var b in data) crc = CrcTable[(byte)(crc ^ b)] ^ crc >> 8;
        return ~crc;
    }

    // ---------- the ones the launcher makes by itself ----------

    // the saves with a backup due: chosen for that moment, and changed since their newest one. With none yet, when the game
    // closes only one played lately (the first time must not copy every old save)
    public static List<Save> Due(string docs, BackupMoment moment)
    {
        var c = BackupChoices.Load();
        bool Chosen(Save s) { var of = c.Of(s.Name); return moment switch { BackupMoment.Close => of.OnClose, BackupMoment.Play => of.BeforePlay, _ => of.Daily }; }
        var due = Saves.List(docs).Where(Chosen).ToList();
        if (due.Count == 0) return due;
        var newest = SaveCopies.ZipFiles(docs).GroupBy(z => z.Name, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Max(z => z.When), StringComparer.OrdinalIgnoreCase);
        // (a zip's day is in its name, without the fraction of a second)
        return due.Where(s => !newest.TryGetValue(s.Name, out var last) ? moment != BackupMoment.Close || s.LastPlayed > DateTime.Now.AddDays(-1)
                              : last < s.LastPlayed.AddTicks(-(s.LastPlayed.Ticks % TimeSpan.TicksPerSecond)) && (moment != BackupMoment.Day || last.Date < DateTime.Today)).ToList();
    }

    // One after the other, never while the game runs (it would be writing them). The saves that got one; a save that could
    // not be backed up does not stop the rest, and the last reason comes back.
    public static (List<Save> Made, Exception? Error) Run(string docs, BackupMoment moment, Action<Save, int, int>? say = null, CancellationToken ct = default)
    {
        var made = new List<Save>();
        if (Engine.DryRun) return (made, null);
        Exception? error = null;
        try
        {
            var due = Due(docs, moment);
            for (var i = 0; i < due.Count && !ct.IsCancellationRequested && !Games.IsRunning(); i++)
            {
                say?.Invoke(due[i], i, due.Count);
                try
                {
                    lock (SaveCopies.Gate)
                    {
                        // (one the game left cut short would only push a good one out)
                        if (!Directory.Exists(due[i].Dir) || SaveHealth.Incomplete(SaveInfo.FilesOf(due[i].Dir)) is not null) continue;
                        Saves.Backup(due[i], docs, moment switch { BackupMoment.Close => AutoClose, BackupMoment.Play => AutoPlay, _ => AutoDay }, ct);
                    }
                    made.Add(due[i]);
                }
                catch (OperationCanceledException) { break; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException or InvalidDataException) { Engine.Log(e); error = e; }
            }
            foreach (var s in made) SaveCopies.Thin(docs, s);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); error = e; }
        return (made, error);
    }

    // What goes so the choices hold: of a save, the launcher's own past the newest ones; then, while the launcher's own pass
    // the limit together, the oldest of them. Never one the player made, named or pinned, nor a save's newest.
    public static List<SavePoint> Extra(string docs, Save s, int keepOwn, long? limit = null)
    {
        var c = BackupChoices.Sure();
        var all = SaveCopies.Zips(docs, _ => true).OrderByDescending(z => z.Point.When).ToList();
        static bool Free(SavePoint p) => IsOwn(p);
        var mine = all.Where(z => z.Name.Equals(s.Name, StringComparison.OrdinalIgnoreCase)).Select(z => z.Point).ToList();
        var go = mine.Where(p => Free(p) && !IsAuto(p.Note)).Skip(keepOwn).Concat(mine.Where(p => Free(p) && IsAuto(p.Note)).Skip(c.Of(s.Name).Keep)).ToList();
        limit ??= c.LimitGb == 0 ? long.MaxValue : (long)c.LimitGb << 30;
        var used = all.Where(z => Free(z.Point)).Sum(z => z.Point.Stored) - go.Sum(p => p.Stored);
        var newest = all.GroupBy(z => z.Name, StringComparer.OrdinalIgnoreCase).Select(g => g.First().Point.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var (_, p) in Enumerable.Reverse(all))
        {
            if (used <= limit) break;
            if (!Free(p) || go.Contains(p) || newest.Contains(p.Path)) continue;
            go.Add(p);
            used -= p.Stored;
        }
        return go;
    }

    // (a USB drive has no Recycle Bin)
    static void Remove(SavePoint p) { if (HasBin(p.Path)) SaveCopies.Recycle(p.Path); else File.Delete(p.Path); }
    internal static void RemoveOwn(SavePoint p) { if (HasBin(p.Path)) SaveCopies.RecycleOwn(p.Path); else File.Delete(p.Path); }
    public static bool HasBin(string path)
    {
        try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(path))!).DriveType == DriveType.Fixed; }
        catch (Exception e) when (e is ArgumentException or IOException or UnauthorizedAccessException) { return false; }
    }

    public static void Delete(IEnumerable<SavePoint> points)
    {
        Engine.CheckDry();
        lock (SaveCopies.Gate) foreach (var p in points.Where(p => p.Kind == CopyKind.Launcher)) Remove(p);
    }

    // ---------- the folder ----------

    // Every backup copied to the other folder and compared there first; only then do they leave the old one. One that
    // fails, or a stop, leaves them all where they were. folder null: the launcher's own in Documents. leave: the folder of now
    // is known to be out of reach, so only the choice changes.
    public static void MoveTo(string docs, string? folder, Action<int, int>? say = null, CancellationToken ct = default, bool leave = false)
    {
        Engine.CheckDry();
        lock (SaveCopies.Gate)
        {
            string from = Path.GetFullPath(Saves.BackupDir(docs)), to = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder ?? Saves.OwnBackupDir(docs)));
            if (from.Equals(to, StringComparison.OrdinalIgnoreCase)) return;
            foreach (var game in new[] { Path.Combine(docs, "Saves"), Path.Combine(docs, "Mods") })
                if ((to + Path.DirectorySeparatorChar).StartsWith(Path.GetFullPath(game) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new SetupException("Choose a folder the game does not use.");
            if (BackupChoices.Sure().Others(docs).Any(f => f.Equals(to, StringComparison.OrdinalIgnoreCase))) throw new SetupException("Another game already keeps its backups in this folder.");
            var reach = Directory.Exists(from);
            if (!reach && !leave && BackupChoices.Load().FolderOf(docs) is not null) throw new SetupException("The backup folder isn't available: {0}", from);
            var zips = SaveCopies.ZipFiles(docs).Select(z => z.Path).ToList();
            Directory.CreateDirectory(to);
            // (the same folder by another path, a link or a mapped drive: nothing to move, and nothing may be removed)
            var probe = Path.Combine(to, Guid.NewGuid().ToString("N") + ".apex");
            File.WriteAllText(probe, "");
            try { if (File.Exists(Path.Combine(from, Path.GetFileName(probe)))) { BackupChoices.Change(c => c.SetFolder(docs, folder is null ? null : to)); return; } }
            finally { File.Delete(probe); }
            var done = new List<string>();
            try
            {
                for (var i = 0; i < zips.Count; i++)
                {
                    ct.ThrowIfCancellationRequested();
                    say?.Invoke(i, zips.Count);
                    var there = Path.Combine(to, Path.GetFileName(zips[i]));
                    if (File.Exists(there) && SaveCopies.Sha(there) == SaveCopies.Sha(zips[i])) continue;
                    File.Copy(zips[i], there);
                    done.Add(there);
                    if (SaveCopies.Sha(there) != SaveCopies.Sha(zips[i])) throw new IOException("a copied backup did not read back as written");
                }
                ct.ThrowIfCancellationRequested();
                BackupChoices.Change(c => c.SetFolder(docs, folder is null ? null : to));
            }
            catch
            {
                foreach (var f in done) try { File.Delete(f); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
                throw;
            }
            foreach (var zip in zips) try { File.Delete(zip); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            try { if (Directory.Exists(from) && !Directory.EnumerateFileSystemEntries(from).Any()) Directory.Delete(from); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }

    // ---------- to and from somewhere else ----------

    public static void Export(SavePoint p, string to)
    {
        Engine.CheckDry();
        if (p.Kind != CopyKind.Launcher) { SaveCopies.Zip(p.Path, to, null, overwrite: true); return; }
        if (Path.GetFullPath(to).Equals(Path.GetFullPath(p.Path), StringComparison.OrdinalIgnoreCase)) return;
        var part = to + ".part";
        try
        {
            File.Copy(p.Path, part, true);
            if (SaveCopies.Sha(part) != SaveCopies.Sha(p.Path)) throw new IOException("a copied backup did not read back as written");
            File.Move(part, to, true);
        }
        catch { try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
    }

    // A zip with one save folder in it, from this launcher or not, as a save of its own (a taken name gets the next number).
    public static Save Import(string docs, string zip, CancellationToken ct = default)
    {
        Engine.CheckWritable();
        string top, name;
        try
        {
            using var z = ZipFile.OpenRead(zip);
            var worlds = z.Entries.Where(e => e.Name.EndsWith(".nhd", StringComparison.OrdinalIgnoreCase)).Select(e => e.FullName.Replace('\\', '/')).Select(n => n[..(n.LastIndexOf('/') + 1)]).Distinct().ToList();
            if (worlds.Count == 0) throw new SetupException("There is no save in this file.");
            if (worlds.Count > 1) throw new SetupException("This file has more than one save. Put each one in its own zip.");
            top = worlds[0];
            name = top.TrimEnd('/') is { Length: > 0 } dir ? dir[(dir.LastIndexOf('/') + 1)..] : Path.GetFileNameWithoutExtension(zip);
        }
        catch (InvalidDataException e) { Engine.Log(e); throw new SetupException("This file can't be opened. Choose a .zip file."); }
        foreach (var end in new[] { ".backup", ".sims3" }) if (name.EndsWith(end, StringComparison.OrdinalIgnoreCase)) name = name[..^end.Length];
        return SaveCopies.Bring(docs, name, to => SaveCopies.Unzip(zip, to, ct, top));
    }
}
