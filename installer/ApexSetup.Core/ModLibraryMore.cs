using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

public static partial class ModLibrary
{
    // a folder moved into another (never into itself or below itself), its switched-off copy, notes and merges along
    public static string MoveFolder(string docs, string rel, string intoRel, IReadOnlyCollection<string>? locked = null)
    {
        Engine.CheckWritable();
        var to = Path.Combine(intoRel, Path.GetFileName(rel));
        if (to.Equals(rel, StringComparison.OrdinalIgnoreCase)) return rel;
        if (intoRel.Equals(rel, StringComparison.OrdinalIgnoreCase) || intoRel.StartsWith(rel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new SetupException("A folder can't be moved into itself.");
        MoveWithOffCopy(docs, rel, to, locked: locked);
        return to;
    }

    public const int MaxGamePath = 259;

    public sealed record AddResult(List<string> Added, List<string> Skipped);

    // files the player dropped or picked: packages into the folder, a zip's packages taken out by name only (never a path
    // from inside the zip). Every file
    // is checked first (ModInstall): a broken, protected, Sims 4 or empty one is not added, nor one already installed unless
    // asked. Skipped: "name: why".
    public static AddResult AddAny(string docs, string folderRel, IEnumerable<string> files, IReadOnlyList<ModFile>? installed = null, bool duplicates = false)
    {
        Engine.CheckWritable();
        var dir = Under(docs, folderRel);
        var added = new List<string>();
        var skipped = new List<string>();
        foreach (var item in ModInstall.Inspect(files, installed ?? []))
        {
            if (!item.Ok) { skipped.Add($"{item.Name}: {item.Problems[0]}"); continue; }
            if (item.SameAs is { } same && !duplicates) { skipped.Add($"{item.Name}: already installed as {same}"); continue; }
            if (!SafeFileName(item.Name)) { skipped.Add($"{item.Name}: the name can't be used for a file"); continue; }
            var into = dir;
            var target = Mods.Free(Path.Combine(into, item.Name), Path.Combine(Mods.Disabled(docs), folderRel));
            // (the game, a 32-bit program, does not read files past the old 260-character path limit)
            if (target.Length >= MaxGamePath) { skipped.Add($"{item.Name}: the folder and name are too long for the game to read"); continue; }
            Directory.CreateDirectory(into);
            if (item.ZipEntry is { } entry)
            {
                using var zip = ZipFile.OpenRead(item.Source);
                (zip.GetEntry(entry) ?? throw new SetupException("The zip changed while it was being added. Try adding it again.")).ExtractToFile(target);
                // (checked again where it landed: the zip may have changed since it was inspected)
                if (item.Kind == "package" && ModInstall.PackageProblems(target).FirstOrDefault() is { } late) { File.Delete(target); skipped.Add($"{item.Name}: {late}"); continue; }
            }
            else File.Copy(item.Source, target);
            added.Add(target);
        }
        return new AddResult(added, skipped);
    }

    // a folder name from a display name: its letters, digits and spaces, at most 60 characters; null when nothing is left
    static string? SafeFolder(string name)
    {
        var s = new string(name.Select(c => char.IsLetterOrDigit(c) || c is ' ' or '-' or '_' ? c : ' ').ToArray()).Trim();
        while (s.Contains("  ")) s = s.Replace("  ", " ");
        return s.Length == 0 ? null : s.Length > 60 ? s[..60].Trim() : s;
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

    // newer versions of merged mods the player put off ("Later"), by file: one that changes again is offered again
    public static string LaterKey(ModFile m) { var f = new FileInfo(m.Path); return $"{m.Rel}*{(f.Exists ? f.Length : 0)}*{(f.Exists ? f.LastWriteTimeUtc.Ticks : 0)}"; }
    public static List<string> MergeLater
    {
        get => (Read().GetValueOrDefault("mergelater") ?? "").Split('|', StringSplitOptions.RemoveEmptyEntries).ToList();
        set { var d = Read(); d["mergelater"] = string.Join("|", value.Distinct(StringComparer.OrdinalIgnoreCase).TakeLast(500)); Write(d); }
    }

    // the end of the last game session: mods newer than it are "new since last play"
    public static DateTime? LastPlay
    {
        get => DateTime.TryParse(Read().GetValueOrDefault("lastplay"), null, System.Globalization.DateTimeStyles.RoundtripKind, out var t) ? t : null;
        set { var d = Read(); if (value is { } v) d["lastplay"] = v.ToString("o"); else d.Remove("lastplay"); Write(d); }
    }
}
