using System.Security.Cryptography;
using System.Text.Json;

namespace ApexSetup;

public sealed class BackupFile
{
    public string Original { get; set; } = "";  // where the file was, and where a restore puts it back
    public string Saved { get; set; } = "";     // the copy in this backup
    public string Sha256 { get; set; } = "";
    public string Reason { get; set; } = "";
}

// One run of Setup that replaced or changed files: Documents\Electronic Arts\The Sims 3\Apex Radiance\Backups\
// <date time> <what changed>\, with the files under "Game Bin" and "Documents" (their paths from there) and backup.json.
// The folder is created with the first file, so a run that replaces nothing leaves none.
public sealed class Backup
{
    public DateTime Created { get; set; } = DateTime.Now;
    public string Label { get; set; } = "";
    public List<BackupFile> Files { get; set; } = new();

    readonly string _bin = "", _docs = "";
    [System.Text.Json.Serialization.JsonIgnore] public string? Dir { get; private set; }

    public Backup(string bin, string docs, string label) { _bin = bin; _docs = docs; Label = label; }

    public static string Root(string docs) => Path.Combine(docs, "Apex Radiance", "Backups");

    // Moves (or copies) the file into this backup and returns the copy's path.
    public string Keep(string file, string reason, bool move = false)
    {
        if (Dir is null)
        {
            var name = $"{Created:yyyy-MM-dd HH-mm-ss} {Label}";
            foreach (var c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '-');
            Dir = Path.Combine(Root(_docs), name.Trim());
            for (int n = 2; Directory.Exists(Dir); n++) Dir = Path.Combine(Root(_docs), $"{name.Trim()} ({n})");
            Directory.CreateDirectory(Dir);
        }
        var saved = Path.Combine(Dir, Relative(file));
        for (int n = 2; File.Exists(saved); n++) saved = Path.Combine(Dir, Relative(file) + $" ({n})");
        Directory.CreateDirectory(Path.GetDirectoryName(saved)!);
        if (move) File.Move(file, saved);
        else File.Copy(file, saved);
        Files.Add(new BackupFile { Original = file, Saved = saved, Sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(saved))), Reason = reason });
        File.WriteAllText(Path.Combine(Dir, "backup.json"), JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        return saved;
    }

    string Relative(string file)
    {
        var full = Path.GetFullPath(file);
        foreach (var (root, folder) in new[] { (_bin, "Game Bin"), (_docs, "Documents") })
            if (root.Length > 0 && full.StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                return Path.Combine(folder, Path.GetRelativePath(root, full));
        return Path.Combine("Other", Path.GetFileName(full));
    }

    // Backups the previous Setup left beside the game's files (Game\Bin\Apex Radiance backup\*.bak, *.apex-backup next to
    // a changed file) move into one backup and setup.json points at them there; nothing is deleted.
    public static void Collect(Manifest m, string docs)
    {
        var bin = m.Bin;
        var loose = new List<string>();
        if (Directory.Exists(Manifest.DirOf(bin))) loose.AddRange(Directory.EnumerateFiles(Manifest.DirOf(bin), "*.bak"));
        foreach (var dir in new[] { bin, Path.Combine(docs, "S3SS") })
            if (Directory.Exists(dir)) loose.AddRange(Directory.EnumerateFiles(dir, "*.apex-backup"));
        if (loose.Count == 0) return;
        var b = new Backup(bin, docs, "Earlier Setup backups");
        foreach (var f in loose)
        {
            var saved = b.Keep(f, "Kept by an earlier Setup", move: true);
            foreach (var e in m.Files.Where(e => e.Backup is not null && Same(e.Backup, f))) e.Backup = saved;
            if (m.OptionsBackup is not null && Same(m.OptionsBackup, f)) m.OptionsBackup = saved;
        }
        m.Save();
    }

    static bool Same(string a, string b) => string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
}
