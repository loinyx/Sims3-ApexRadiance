using System.Reflection;
using System.Text.Json;

namespace ApexSetup;

public sealed class RestoreEntry
{
    public string Path { get; set; } = "";   // absolute
    public bool Existed { get; set; }
    public string? Copy { get; set; }        // the original, a file in the point's folder
    public bool Dir { get; set; }
    public bool RestartHostKey { get; set; } // ApexRadiance.toml: undone by putting back only [ui] restart_host (RestartHostWas)
    public bool? RestartHostWas { get; set; } // its value before, null when the key was missing
    public bool FpsLimitKey { get; set; }    // ApexRadiance.toml: undone by putting back only [screen] fps_limit (FpsLimitWas)
    public int? FpsLimitSet { get; set; }     // the value Setup wrote (null in older points: 60, frame generation's)
    public int? FpsLimitWas { get; set; }     // its value before, null when the key was missing
}

// A copy of everything one action of Setup changed, taken as it changes it:
// Game\Bin\Apex Radiance backup\restore\<yyyyMMdd-HHmmss>\point.json and the original files beside it.
public sealed class RestorePoint
{
    public DateTime Created { get; set; }           // UTC
    public string CreatedLocal { get; set; } = "";
    public string Label { get; set; } = "";         // an English key with {n} arguments, as Finding.Text
    public string[] Args { get; set; } = Array.Empty<string>();
    public string SetupVersion { get; set; } = "";
    public string Bin { get; set; } = "";
    public List<RestoreEntry> Entries { get; set; } = new();
    [System.Text.Json.Serialization.JsonIgnore] public string Dir { get; set; } = "";
    public int FileCount => Entries.Count(e => !e.Dir);
}

public static class RestorePoints
{
    public const int Keep = 5;
    public static string RootOf(string bin) => Path.Combine(Manifest.DirOf(bin), "restore");

    // The point being recorded: one action of Setup at a time (the UI is busy meanwhile). A Begin inside another is the same point.
    static Scope? _open;

    public sealed class Scope : IDisposable
    {
        internal readonly RestorePoint Point;
        internal readonly HashSet<string> Seen = new(StringComparer.OrdinalIgnoreCase);
        readonly bool _outer;
        public RestorePoint? Committed { get; private set; } // after Dispose: the point kept for this action, null when nothing changed

        internal Scope(RestorePoint p, bool outer) { Point = p; _outer = outer; }

        public void Dispose()
        {
            if (!_outer) return;
            _open = null;
            if (Point.Entries.Count == 0) { if (Directory.Exists(Point.Dir)) DeleteTree(Point.Dir); return; }
            Committed = Point; // (point.json is already written, entry by entry)
            foreach (var old in List(Point.Bin).Skip(Keep)) DeleteTree(old.Dir);
            // a folder with no point.json: cut off before its first entry was saved, so nothing it copied was changed (one that
            // only can't be read now, as when an antivirus holds it, is a restore point all the same)
            foreach (var d in Directory.EnumerateDirectories(RootOf(Point.Bin)))
                if (!File.Exists(Path.Combine(d, "point.json"))) DeleteTree(d);
        }
    }

    static void Save(RestorePoint p)
    {
        var json = Path.Combine(p.Dir, "point.json");
        File.WriteAllText(json + ".tmp", JsonSerializer.Serialize(p, SetupJson.Default.RestorePoint));
        File.Move(json + ".tmp", json, true);
    }

    static RestorePoint? Read(string dir)
    {
        try
        {
            var p = JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(dir, "point.json")), SetupJson.Default.RestorePoint);
            if (p is not null) p.Dir = dir;
            return p;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }

    static bool Same(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)), StringComparison.OrdinalIgnoreCase);

    public static Scope Begin(string bin, string label, params string[] args)
    {
        if (_open is { } open) return new Scope(open.Point, false);
        var now = DateTime.Now;
        var p = new RestorePoint
        {
            Created = now.ToUniversalTime(), CreatedLocal = now.ToString("yyyy-MM-dd HH:mm:ss zzz"), Label = label, Args = args, Bin = bin,
            SetupVersion = Assembly.GetEntryAssembly()?.GetName().Version?.ToString(3) ?? "",
        };
        var name = now.ToString("yyyyMMdd-HHmmss");
        p.Dir = Path.Combine(RootOf(bin), name);
        for (int n = 2; Directory.Exists(p.Dir); n++) p.Dir = Path.Combine(RootOf(bin), $"{name}-{n}"); // (two actions in one second)
        return _open = new Scope(p, true);
    }

    // first touch wins: the path as it was before this action. All or nothing: the entry is kept only once its copy and
    // point.json are on disk (an action cut off after it can still be restored); otherwise it throws before the change
    internal static void Touch(string path, bool dir = false, bool key = false, bool? keyWas = null, bool fps = false, int? fpsWas = null, int? fpsSet = null)
    {
        if (_open is not { } s) return;
        path = Path.GetFullPath(path);
        if (s.Seen.Contains(path)) return;
        var e = new RestoreEntry { Path = path, Dir = dir, Existed = dir ? Directory.Exists(path) : File.Exists(path), RestartHostKey = key, RestartHostWas = keyWas, FpsLimitKey = fps, FpsLimitWas = fpsWas, FpsLimitSet = fpsSet };
        Directory.CreateDirectory(s.Point.Dir);
        try
        {
            if (e.Existed && !dir)
            {
                e.Copy = $"{s.Point.Entries.Count}-{Path.GetFileName(path)}";
                File.Copy(path, Path.Combine(s.Point.Dir, e.Copy), true);
            }
            s.Point.Entries.Add(e);
            Save(s.Point);
        }
        catch
        {
            s.Point.Entries.Remove(e);
            if (e.Copy is not null) try { File.Delete(Path.Combine(s.Point.Dir, e.Copy)); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            throw;
        }
        s.Seen.Add(path);
    }

    // newest first, only the points of this Game\Bin (a game folder copied or moved brings the old one's, whose paths are
    // elsewhere); a folder without a readable point.json is left out
    public static List<RestorePoint> List(string bin)
    {
        if (!Directory.Exists(RootOf(bin))) return new();
        // by time, not name: a name freed by pruning can be taken again within the same second
        return Directory.EnumerateDirectories(RootOf(bin)).Select(Read).OfType<RestorePoint>().Where(p => Same(p.Bin, bin))
            .OrderByDescending(p => p.Created).ToList();
    }

    // Undoes that point and every newer one, newest first. Restoring is final: the points restored are deleted (one with a path
    // that could not be put back stays, to try again, and the older ones are left untouched: each point's copies are the state
    // after the older ones, so they are undone only once every newer one is). Returns the paths that could not be put back.
    public static List<string> Restore(string bin, RestorePoint point)
    {
        Engine.CheckWritable();
        if (_open is not null) throw new InvalidOperationException("The launcher is applying changes");
        var points = List(bin);
        if (!points.Any(p => p.Dir == point.Dir)) throw new InvalidOperationException("This restore point is gone.");
        var failed = new List<string>();
        foreach (var p in points.TakeWhile(p => p.Dir != point.Dir).Append(point))
        {
            var before = failed.Count;
            // last change first: a file goes before the folder it was created in
            foreach (var e in Enumerable.Reverse(p.Entries))
                try { Undo(p, e); }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { failed.Add(e.Path); }
            if (failed.Count > before) break;
            DeleteTree(p.Dir);
        }
        // the backup folder Setup made goes too, once nothing is left in it (an antivirus's own file, NAME.tmp, is waited for)
        foreach (var d in new[] { RootOf(bin), Manifest.DirOf(bin) })
            try { Fs.Retry(() => { if (Directory.Exists(d) && Directory.EnumerateFileSystemEntries(d).All(f => f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) && Fs.Emptied(d)) { Directory.Delete(d); Fs.Gone(d); } }); }
            catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
        return failed;
    }

    static void Undo(RestorePoint p, RestoreEntry e)
    {
        if (e.Dir)
        {
            if (e.Existed) Directory.CreateDirectory(e.Path);
            // (an antivirus may still list a file just deleted, or one of its own beside it: waited for)
            else Fs.Retry(() => { if (Directory.Exists(e.Path) && Fs.Emptied(e.Path)) { Directory.Delete(e.Path); Fs.Gone(e.Path); } });
            return;
        }
        if (File.Exists(e.Path)) File.SetAttributes(e.Path, File.GetAttributes(e.Path) & ~FileAttributes.ReadOnly);
        if (e.FpsLimitKey)
        {
            // only the key goes back: the game may have saved other settings since
            if (!File.Exists(e.Path)) return;
            var text = File.ReadAllText(e.Path);
            // only while the file still holds what Setup wrote: a limit the player changed since stays (Restore checked the game is closed)
            if (Engine.FpsLimitKey(text) == (e.FpsLimitSet ?? Engine.FrameGenFps))
                Fs.WriteAtomic(e.Path, e.FpsLimitWas is { } was ? Engine.WithFpsLimit(text, was) : Engine.WithoutFpsLimit(text));
            return;
        }
        if (e.RestartHostKey)
        {
            // the game may have saved other settings in the file since: only the key goes back (the whole file when the game
            // never wrote it after Setup created it)
            if (!File.Exists(e.Path)) return;
            var text = File.ReadAllText(e.Path);
            if (!e.Existed && text == Engine.NewApexToml) File.Delete(e.Path);
            else if (Engine.RestartHostKey(text) != e.RestartHostWas)
                Fs.WriteAtomic(e.Path, e.RestartHostWas is { } was ? Engine.WithRestartHost(text, was) : Engine.WithoutRestartHost(text));
            return;
        }
        if (!e.Existed) { if (File.Exists(e.Path)) File.Delete(e.Path); return; }
        // through a temporary name: a failed copy never leaves half a file
        Directory.CreateDirectory(Path.GetDirectoryName(e.Path)!);
        var tmp = e.Path + ".apex-restore";
        try
        {
            // (a temporary file left by an earlier try, read-only from its copy, is replaced)
            if (File.Exists(tmp)) { File.SetAttributes(tmp, FileAttributes.Normal); File.Delete(tmp); }
            File.Copy(Path.Combine(p.Dir, e.Copy!), tmp, true);
            File.Move(tmp, e.Path, true);
        }
        catch
        {
            try { if (File.Exists(tmp)) { File.SetAttributes(tmp, FileAttributes.Normal); File.Delete(tmp); } } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            throw;
        }
    }

    static void DeleteTree(string dir)
    {
        try
        {
            Fs.DeleteClear(dir, () =>
            {
                foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)) File.SetAttributes(f, FileAttributes.Normal);
                return true;
            });
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (pruned the next time)
    }
}

// Every change Setup makes to the player's files goes through here, so the open restore point has the file as it was.
public static class Fs
{
    // a read-only destination is made writable first (the restore point keeps it as it was)
    public static void Copy(string from, string to)
    {
        RestorePoints.Touch(to);
        Retry(() =>
        {
            Writable(to);
            File.Copy(from, to, true);
        });
    }
    public static void Move(string from, string to) { RestorePoints.Touch(from); RestorePoints.Touch(to); Retry(() => File.Move(from, to, true)); }
    public static void Write(string path, string text, System.Text.Encoding? encoding = null) { RestorePoints.Touch(path); Retry(() => WriteAtomic(path, text, encoding)); }

    // a file an antivirus scan or the game's launcher holds a moment: tried again 4 times, waiting 0.5, 1, 2 and 4 s (a scan of
    // a large package just written can take seconds); a missing file fails at once
    public static TimeSpan RetryDelay = TimeSpan.FromMilliseconds(500);
    public const int RetryTimes = 4;
    public static void Retry(Action io)
    {
        for (var i = 0; ; i++)
            try { io(); return; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException && e is not FileNotFoundException && e is not DirectoryNotFoundException && i < RetryTimes)
            { Thread.Sleep(RetryDelay * (1 << i)); }
    }
    // a deleted folder an antivirus or indexer still holds stays listed until Windows lets go: waits up to 2 s for it, so the
    // folder above it can be seen empty and removed too
    // a folder whose files were just deleted, empty once Windows lets go of them (up to 2 s)
    public static bool Emptied(string dir)
    {
        // (one look decides: an antivirus may add a file of its own for a moment between two)
        for (var i = 0; Directory.EnumerateFileSystemEntries(dir).Any(); i++)
        {
            if (i == 40) return false;
            Thread.Sleep(50);
        }
        return true;
    }
    // A folder and its subfolders, while clear says they hold nothing to keep. A file just moved that an antivirus still holds
    // stays listed a moment, and the antivirus puts a file of its own beside it: any look or delete can meet one, and a delete
    // that meets one in a subfolder says nothing. So both are done again until the folder is gone: 2 s for a file to go, 8 s
    // for the folder.
    public static void DeleteClear(string dir, Func<bool>? clear = null)
    {
        for (var i = 0; i < 160 && Directory.Exists(dir); i++)
            try
            {
                if (i > 0) Thread.Sleep(50);
                if (clear is null || clear()) Directory.Delete(dir, true);
                else if (i >= 40) return;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException && i < 159) { }
    }
    public static void Gone(string dir)
    {
        for (var i = 0; i < 40 && Directory.Exists(dir); i++) Thread.Sleep(50);
    }
    // ApexRadiance.toml with [ui] restart_host changed from keyWas: a restore point puts back only that key
    public static void WriteKey(string path, string text, bool? keyWas) { RestorePoints.Touch(path, key: true, keyWas: keyWas); Retry(() => WriteAtomic(path, text)); }
    public static void WriteFpsKey(string path, string text, int? was, int set) { RestorePoints.Touch(path, fps: true, fpsWas: was, fpsSet: set); Retry(() => WriteAtomic(path, text)); }
    // a temporary file in the same folder, then moved over: a crash never leaves half of ApexRadiance.toml. A read-only target is
    // made writable (the restore point keeps it as it was); a failed move leaves no temporary file behind.
    public static void WriteAtomic(string path, string text, System.Text.Encoding? encoding = null)
    {
        var tmp = path + ".apex-tmp";
        try
        {
            if (encoding is null) File.WriteAllText(tmp, text); else File.WriteAllText(tmp, text, encoding);
            Writable(path);
            File.Move(tmp, path, true);
        }
        catch
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            throw;
        }
    }
    // (a read-only file, such as the copy of a read-only Options.ini, is made writable first)
    public static void Delete(string path)
    {
        if (!File.Exists(path)) return;
        RestorePoints.Touch(path);
        Retry(() => { Writable(path); File.Delete(path); });
    }
    static void Writable(string path)
    {
        if (File.Exists(path) && File.GetAttributes(path).HasFlag(FileAttributes.ReadOnly)) File.SetAttributes(path, File.GetAttributes(path) & ~FileAttributes.ReadOnly);
    }
    public static void ClearReadOnly(string path)
    {
        if (!File.Exists(path)) return;
        RestorePoints.Touch(path);
        File.SetAttributes(path, File.GetAttributes(path) & ~FileAttributes.ReadOnly);
    }
    public static void SetAttributes(string path, FileAttributes attributes) { RestorePoints.Touch(path); File.SetAttributes(path, attributes); }

    // the folders this creates are recorded, outermost first
    public static void CreateDir(string path)
    {
        var made = new Stack<string>();
        for (var d = Path.GetFullPath(path); d is not null && !Directory.Exists(d); d = Path.GetDirectoryName(d)) made.Push(d);
        foreach (var d in made) RestorePoints.Touch(d, dir: true);
        Directory.CreateDirectory(path);
    }

    public static void DeleteDir(string path) { RestorePoints.Touch(path, dir: true); Retry(() => Directory.Delete(path)); }
}
