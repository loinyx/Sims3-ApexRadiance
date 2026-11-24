using System.Text.Json;

namespace ApexSetup;

public sealed record SizePoint(DateTime When, long Bytes);
public sealed record SaveReport(Save Save, long Bytes, List<SizePoint> History, DateTime? LastBackup, string? Incomplete);

// Each save's size, how it grew (a point kept in %LOCALAPPDATA%\Apex Launcher\save-sizes.json each time it is checked),
// its last launcher backup and whether it looks incomplete. A save folder holds Meta.data, 1a5tf1l3.dat, one
// <world>_0x<id>.nhd per world (the home world and any travel world), their ExportDB.package and TravelDB.package (seen
// on real saves of 28 to 160 MB).
public static class SaveHealth
{
    // unverified thresholds: community advice says saves get slow to load and save, and fail more, as they grow past a
    // few hundred MB (a fresh base-game world is about 30 MB, one with a travel world about 160 MB on a real install)
    public const long LargeBytes = 300L << 20;
    public const double FastGrowth = 0.5; // half again its size within GrowthDays
    public const int GrowthDays = 30;
    public const int BackupDays = 14;
    const int KeepPoints = 30;

    static string FilePath => Path.Combine(Paths.Data, "save-sizes.json");

    static Dictionary<string, List<SizePoint>> Load()
    {
        try { return File.Exists(FilePath) ? JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.DictionaryStringListSizePoint) ?? new() : new(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return new(); }
    }

    // (two checks at once, e.g. Check now while the start-up check runs, write the history one after the other)
    static readonly object Gate = new();

    // a save's sizes over time, oldest first (what Check recorded)
    public static List<SizePoint> History(string name) => Load().GetValueOrDefault(name) ?? [];

    // a renamed save keeps its history
    public static void Renamed(string from, string to)
    {
        lock (Gate)
        {
            var history = Load();
            if (!history.Remove(from, out var points)) return;
            history[to] = points;
            Fs.Retry(() => Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(history, SetupJson.Default.DictionaryStringListSizePoint)));
        }
    }

    public static List<SaveReport> Check(string docs, bool record = true)
    {
        var history = Load();
        var list = new List<SaveReport>();
        var now = DateTime.Now;
        foreach (var s in Saves.List(docs))
        {
            List<FileInfo> files;
            try { files = new DirectoryInfo(s.Dir).EnumerateFiles().Where(f => !f.Name.EndsWith(".backup", StringComparison.OrdinalIgnoreCase)).ToList(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            var bytes = files.Sum(f => f.Length);
            var incomplete = !files.Any(f => f.Extension.Equals(".nhd", StringComparison.OrdinalIgnoreCase)) ? "no .nhd world file"
                : !files.Any(f => f.Name.Equals("Meta.data", StringComparison.OrdinalIgnoreCase)) ? "no Meta.data"
                : files.FirstOrDefault(f => f.Length == 0) is { } empty ? $"{empty.Name} is empty" : null;
            var points = history.GetValueOrDefault(s.Name) ?? [];
            // one point a day, or when the size changed
            if (record && (points.Count == 0 || points[^1].Bytes != bytes || points[^1].When.Date != now.Date)) points.Add(new(now, bytes));
            history[s.Name] = points = points.TakeLast(KeepPoints).ToList();
            list.Add(new(s, bytes, points, LastBackup(s, docs), incomplete));
        }
        if (record)
            try { Directory.CreateDirectory(Paths.Data); var text = JsonSerializer.Serialize(history, SetupJson.Default.DictionaryStringListSizePoint); lock (Gate) Fs.Retry(() => Fs.WriteAtomic(FilePath, text)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        return list;
    }

    // the newest zip Saves.Backup made of this save ("<name> yyyy-MM-dd HH.mm.ss.zip")
    public static DateTime? LastBackup(Save s, string docs)
    {
        var dir = Saves.BackupDir(docs);
        if (!Directory.Exists(dir)) return null;
        try
        {
            return Directory.EnumerateFiles(dir, "*.zip")
                .Where(z => Path.GetFileNameWithoutExtension(z) is var n && n.StartsWith(s.Name + " ") && n.Length == s.Name.Length + 20)
                .Select(File.GetLastWriteTime).DefaultIfEmpty().Max() is var t && t > DateTime.MinValue ? t : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    // a copy made since the save was last played, or not long ago
    public static bool RecentCopy(DateTime? copy, Save s) => copy is { } c && (c >= s.LastPlayed || c >= DateTime.Now.AddDays(-BackupDays));

    public static List<Finding> Findings(IEnumerable<SaveReport> reports, string docs)
    {
        var f = new List<Finding>();
        foreach (var r in reports)
        {
            var mb = (r.Bytes >> 20).ToString();
            if (r.Incomplete is { } why)
                f.Add(new(Severity.Warning, "The save {0} looks incomplete ({1}). If the game can't open it, load a backup of it.", [r.Save.Name, why]));
            if (r.Bytes >= LargeBytes)
                f.Add(new(Severity.Info, "The save {0} is {1} MB. Saves this large load and save slowly. NRaas Overwatch or moving out unused households makes it smaller.", [r.Save.Name, mb]));
            if (r.History.FirstOrDefault(p => p.When >= DateTime.Now.AddDays(-GrowthDays)) is { Bytes: > 0 } first && r.Bytes > first.Bytes * (1 + FastGrowth) && r.Bytes - first.Bytes > 20L << 20)
                f.Add(new(Severity.Info, "The save {0} grew from {1} MB to {2} MB in {3} days.", [r.Save.Name, (first.Bytes >> 20).ToString(), mb, Math.Max(1, (int)(DateTime.Now - first.When).TotalDays).ToString()]));
            // a save played since its last backup, more than BackupDays ago (or never backed up)
            if (r.Incomplete is null && !RecentCopy(r.LastBackup, r.Save))
            {
                var save = r.Save;
                f.Add(new(Severity.Info, r.LastBackup is null ? "The save {0} has no backup yet." : "The save {0} was last backed up {1} days ago.",
                    [save.Name, r.LastBackup is { } d ? ((int)(DateTime.Now - d).TotalDays).ToString() : ""], () => Saves.Backup(save, docs), Suggest: "Back up the save"));
            }
        }
        return f;
    }
}
