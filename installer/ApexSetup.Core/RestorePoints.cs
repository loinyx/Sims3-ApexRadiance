using System.Reflection;
using System.Text.Json;

namespace ApexSetup;

public sealed class RestoreEntry
{
    public string Path { get; set; } = "";   // absolute
    public bool Existed { get; set; }
    public string? Copy { get; set; }        // the original, a file in the point's folder
    public bool Dir { get; set; }
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
            // a folder with no readable point.json: cut off before its first entry was saved, so nothing it copied was changed
            foreach (var d in Directory.EnumerateDirectories(RootOf(Point.Bin)))
                if (Read(d) is null) DeleteTree(d);
        }
    }

    static void Save(RestorePoint p)
    {
        var json = Path.Combine(p.Dir, "point.json");
        File.WriteAllText(json + ".tmp", JsonSerializer.Serialize(p, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(json + ".tmp", json, true);
    }

    static RestorePoint? Read(string dir)
    {
        try
        {
            var p = JsonSerializer.Deserialize<RestorePoint>(File.ReadAllText(Path.Combine(dir, "point.json")));
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
    internal static void Touch(string path, bool dir = false)
    {
        if (_open is not { } s) return;
        path = Path.GetFullPath(path);
        if (s.Seen.Contains(path)) return;
        var e = new RestoreEntry { Path = path, Dir = dir, Existed = dir ? Directory.Exists(path) : File.Exists(path) };
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
        if (_open is not null) throw new InvalidOperationException("Setup is applying changes");
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
        // the backup folder Setup made goes too, once nothing is left in it
        foreach (var d in new[] { RootOf(bin), Manifest.DirOf(bin) })
            try { if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Directory.Delete(d); }
            catch (IOException) { }
        return failed;
    }

    static void Undo(RestorePoint p, RestoreEntry e)
    {
        if (e.Dir)
        {
            if (e.Existed) Directory.CreateDirectory(e.Path);
            else if (Directory.Exists(e.Path) && !Directory.EnumerateFileSystemEntries(e.Path).Any()) Directory.Delete(e.Path);
            return;
        }
        if (File.Exists(e.Path)) File.SetAttributes(e.Path, File.GetAttributes(e.Path) & ~FileAttributes.ReadOnly);
        if (!e.Existed) { if (File.Exists(e.Path)) File.Delete(e.Path); return; }
        // through a temporary name: a failed copy never leaves half a file
        Directory.CreateDirectory(Path.GetDirectoryName(e.Path)!);
        var tmp = e.Path + ".apex-restore";
        File.Copy(Path.Combine(p.Dir, e.Copy!), tmp, true);
        File.Move(tmp, e.Path, true);
    }

    static void DeleteTree(string dir)
    {
        try
        {
            foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)) File.SetAttributes(f, FileAttributes.Normal);
            Directory.Delete(dir, true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (pruned the next time)
    }
}

// Every change Setup makes to the player's files goes through here, so the open restore point has the file as it was.
public static class Fs
{
    public static void Copy(string from, string to) { RestorePoints.Touch(to); File.Copy(from, to, true); }
    public static void Move(string from, string to) { RestorePoints.Touch(from); RestorePoints.Touch(to); File.Move(from, to, true); }
    public static void Write(string path, string text) { RestorePoints.Touch(path); File.WriteAllText(path, text); }
    public static void Delete(string path) { if (!File.Exists(path)) return; RestorePoints.Touch(path); File.Delete(path); }
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

    public static void DeleteDir(string path) { RestorePoints.Touch(path, dir: true); Directory.Delete(path); }
}
