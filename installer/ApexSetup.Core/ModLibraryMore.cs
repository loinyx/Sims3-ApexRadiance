using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

public static partial class ModLibrary
{
    // a folder moved into another (never into itself or below itself), its switched-off copy, notes and merges along
    public static string MoveFolder(string docs, string rel, string intoRel)
    {
        Engine.CheckWritable();
        var to = Path.Combine(intoRel, Path.GetFileName(rel));
        if (to.Equals(rel, StringComparison.OrdinalIgnoreCase)) return rel;
        if (intoRel.Equals(rel, StringComparison.OrdinalIgnoreCase) || intoRel.StartsWith(rel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new SetupException("A folder can't go inside itself.");
        var from = Under(docs, rel);
        var target = Under(docs, to);
        if (Directory.Exists(target)) throw new SetupException("A folder with that name is already there.");
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        Directory.Move(from, target);
        var offFrom = Path.Combine(Mods.Disabled(docs), rel);
        if (Directory.Exists(offFrom))
        {
            var offTo = Path.Combine(Mods.Disabled(docs), to);
            Directory.CreateDirectory(Path.GetDirectoryName(offTo)!);
            Directory.Move(offFrom, offTo);
        }
        ModNotes.Move(Path.Combine("Mods", rel), Path.Combine("Mods", to), folder: true);
        ModMerge.Moved(docs, Path.Combine("Mods", rel), Path.Combine("Mods", to), folder: true);
        return to;
    }

    public sealed record AddResult(List<string> Added, List<string> Skipped);

    // files the player dropped or picked: packages into the folder, Sims3Packs into Downloads (the game's launcher installs
    // them from there), a zip's packages and Sims3Packs taken out (names only, never a path from inside the zip)
    public static AddResult AddAny(string docs, string folderRel, IEnumerable<string> files)
    {
        Engine.CheckWritable();
        var dir = Under(docs, folderRel);
        var downloads = Path.Combine(docs, "Downloads");
        var added = new List<string>();
        var skipped = new List<string>();
        void Put(string name, Func<string, bool> write)
        {
            var ext = Path.GetExtension(name).ToLowerInvariant();
            var into = ext == ".sims3pack" ? downloads : ext == ".package" ? dir : null;
            if (into is null || !SafeFileName(name)) { skipped.Add(name); return; }
            Directory.CreateDirectory(into);
            var target = Mods.Free(Path.Combine(into, Path.GetFileName(name)));
            if (write(target)) added.Add(target); else skipped.Add(name);
        }
        foreach (var f in files)
        {
            if (Directory.Exists(f)) { foreach (var x in Directory.EnumerateFiles(f, "*", SearchOption.AllDirectories)) Put(Path.GetFileName(x), t => { File.Copy(x, t); return true; }); continue; }
            if (!File.Exists(f)) { skipped.Add(Path.GetFileName(f)); continue; }
            if (f.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    using var zip = ZipFile.OpenRead(f);
                    var any = false;
                    foreach (var e in zip.Entries.Where(e => e.Length > 0 && e.Length < 4L << 30))
                    {
                        var n = Path.GetFileName(e.FullName.Replace('\\', '/'));
                        if (!n.EndsWith(".package", StringComparison.OrdinalIgnoreCase) && !n.EndsWith(".sims3pack", StringComparison.OrdinalIgnoreCase)) continue;
                        any = true;
                        Put(n, t => { e.ExtractToFile(t); return true; });
                    }
                    if (!any) skipped.Add(Path.GetFileName(f));
                }
                catch (InvalidDataException) { skipped.Add(Path.GetFileName(f)); }
                continue;
            }
            Put(Path.GetFileName(f), t => { File.Copy(f, t); return true; });
        }
        return new AddResult(added, skipped);
    }

    static bool SafeFileName(string n) => n.Length is > 0 and < 200 && n.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 && n is not ("." or "..");

    // packages that are the same file twice (equal size, then equal SHA-256), each pointing at the other copy's path;
    // shortcut: files over 512 MB are not hashed (rare, slow to read), upgrade if players report duplicates that big
    public static Dictionary<string, string> Duplicates(IReadOnlyList<ModFile> mods)
    {
        var found = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var group in mods.Where(m => m.FileType == "package" && m.Size is > 0 and < 512L << 20).GroupBy(m => m.Size).Where(g => g.Count() > 1))
        {
            var byHash = new Dictionary<string, string>();
            foreach (var m in group.OrderBy(m => m.Rel, StringComparer.OrdinalIgnoreCase))
            {
                string hash;
                try { using var s = File.OpenRead(m.Path); hash = Convert.ToHexString(SHA256.HashData(s)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
                if (byHash.TryGetValue(hash, out var first)) { found[m.Path] = first; found.TryAdd(first, m.Path); }
                else byHash[hash] = m.Path;
            }
        }
        return found;
    }
}

// How the Mods screen was left (the place, list or grid, the sort, the side groups folded) and when the game last closed,
// in %LOCALAPPDATA%\Apex Launcher\mods-view.txt as key=value lines.
public static class ModViewPrefs
{
    public sealed record Prefs(string? Place, bool Grid, int Sort, List<string> Collapsed);

    static string FilePath => Path.Combine(Paths.Data, "mods-view.txt");

    static Dictionary<string, string> Read()
    {
        var d = new Dictionary<string, string>();
        try
        {
            if (File.Exists(FilePath))
                foreach (var l in File.ReadAllLines(FilePath))
                    if (l.IndexOf('=') is > 0 and var k) d[l[..k]] = l[(k + 1)..];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return d;
    }

    static void Write(Dictionary<string, string> d)
    {
        try { Directory.CreateDirectory(Paths.Data); File.WriteAllLines(FilePath, d.Select(x => $"{x.Key}={x.Value.Replace('\n', ' ')}")); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    public static Prefs Load()
    {
        var d = Read();
        return new Prefs(d.GetValueOrDefault("place"), d.GetValueOrDefault("grid") == "1", int.TryParse(d.GetValueOrDefault("sort"), out var s) ? Math.Clamp(s, 0, 5) : 0,
            (d.GetValueOrDefault("collapsed") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries).ToList());
    }

    public static void Save(Prefs p)
    {
        var d = Read();
        if (p.Place is not null) d["place"] = p.Place;
        d["grid"] = p.Grid ? "1" : "0";
        d["sort"] = p.Sort.ToString();
        d["collapsed"] = string.Join(",", p.Collapsed);
        Write(d);
    }

    // the end of the last game session: mods newer than it are "new since last play"
    public static DateTime? LastPlay
    {
        get => DateTime.TryParse(Read().GetValueOrDefault("lastplay"), null, System.Globalization.DateTimeStyles.RoundtripKind, out var t) ? t : null;
        set { var d = Read(); if (value is { } v) d["lastplay"] = v.ToString("o"); else d.Remove("lastplay"); Write(d); }
    }
}
