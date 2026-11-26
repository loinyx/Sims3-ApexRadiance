using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

// Data a mod keeps for each save outside Saves (the game writes a save's folder anew on every save, so nothing of a mod's
// can live in it). A mod declares it in <Documents>\Save companions (Apex Launcher)\<mod>.json:
//   { "paths": ["MyMod/{key}", "MyMod/{key}.dat"], "key": { "allowed": "A-Za-z0-9._-", "replace": "_", "maxLength": 100 } }
// paths: folders or files relative to the game's Documents folder, each with {key}, never in Saves or Mods. key: how the
// save's folder name (<name>.sims3) becomes {key}: each character outside allowed (a regex character class) and a leading
// '.' become replace, then it is cut to maxLength; without it {key} is the folder name. Backups hold the data under
// apex-companions/<mod>/<path> beside the save's folder; restore, rename, duplicate, delete, export and import carry it.
public sealed record SaveCompanion(string Mod, List<string> Paths, Func<string, string> Key);

public static class SaveCompanions
{
    public const string Top = "apex-companions/";
    public static string DeclDir(string docs) => Path.Combine(docs, "Save companions (Apex Launcher)");
    static string WorkRoot(string docs) => Path.Combine(DeclDir(docs), ".apex-work");
    static readonly Regex Plain = new("^[A-Za-z0-9._-]{1,64}$");
    static readonly EnumerationOptions Deep = new() { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint };

    // the declarations that can be used; one that can't be read or points where it must not is logged and left out
    public static List<SaveCompanion> Of(string docs)
    {
        var list = new List<SaveCompanion>();
        try
        {
            if (!Directory.Exists(DeclDir(docs))) return list;
            foreach (var f in Directory.EnumerateFiles(DeclDir(docs), "*.json").Order(StringComparer.OrdinalIgnoreCase))
                try { list.Add(Read(docs, f)); }
                catch (Exception e) when (e is JsonException or InvalidDataException or InvalidOperationException or KeyNotFoundException or ArgumentException or FormatException or IOException or UnauthorizedAccessException)
                { Engine.Log(new InvalidDataException($"save companion {Path.GetFileName(f)} left out: {e.Message}", e)); }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        return list;
    }

    static SaveCompanion Read(string docs, string file)
    {
        var mod = Path.GetFileNameWithoutExtension(file);
        if (!Plain.IsMatch(mod)) throw new InvalidDataException("the file name is not a plain name");
        using var j = JsonDocument.Parse(File.ReadAllText(file));
        var paths = j.RootElement.GetProperty("paths").EnumerateArray().Select(p => p.GetString()!.Replace('\\', '/').Trim('/')).ToList();
        if (paths.Count == 0) throw new InvalidDataException("no paths");
        var root = Path.GetFullPath(docs) + Path.DirectorySeparatorChar;
        // (the game's saves and mods, and the launcher's and Apex Radiance's own folders)
        static bool Theirs(string top) => top.Equals("Saves", StringComparison.OrdinalIgnoreCase) || top.Equals("Mods", StringComparison.OrdinalIgnoreCase)
            || top.Equals("Apex Radiance", StringComparison.OrdinalIgnoreCase) || top.EndsWith(" (Apex Launcher)", StringComparison.OrdinalIgnoreCase);
        foreach (var p in paths)
            if (!p.Contains("{key}") || Path.IsPathRooted(p) || p.Contains(':') || p.Split('/').Any(s => s is "" or "." or "..") || Theirs(p.Split('/')[0])
                || !Path.GetFullPath(Path.Combine(docs, p.Replace("{key}", "k"))).StartsWith(root, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"{p}: a path in the Documents folder with {{key}}, outside Saves and Mods");
        Func<string, string> key = n => n;
        if (j.RootElement.TryGetProperty("key", out var k))
        {
            var other = new Regex($"[^{k.GetProperty("allowed").GetString()}]");
            var replace = k.TryGetProperty("replace", out var r) ? r.GetString() ?? "_" : "_";
            var max = k.TryGetProperty("maxLength", out var m) ? m.GetInt32() : 255;
            key = n =>
            {
                var s = other.Replace(n, _ => replace);
                if (s.StartsWith('.')) s = replace + s[1..];
                return s.Length > max ? s[..max] : s;
            };
        }
        return new(mod, paths, key);
    }

    // the save's {key} for this mod, or null when the rule makes no usable file name of it
    static string? KeyOf(SaveCompanion c, string save) =>
        c.Key(save + ".sims3") is { Length: > 0 } k && k is not ("." or "..") && k.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 ? k : null;
    static string Full(string docs, string path, string key) => Path.Combine(docs, path.Replace("{key}", key));
    static bool Exists(string p) => File.Exists(p) || Directory.Exists(p);

    // another save, in the game or kept out of it, whose data is in the same place (two names one key rule makes alike)
    static bool Shared(string docs, SaveCompanion c, string key, string? but) =>
        new[] { Path.Combine(docs, "Saves"), SaveCopies.StoredDir(docs) }.SelectMany(Saves.In).Any(s => !s.Name.Equals(but, StringComparison.OrdinalIgnoreCase) && KeyOf(c, s.Name) == key);

    // each mod's place for this save and the one for another name (null when the rule gives none for either)
    static IEnumerable<(SaveCompanion C, string Path, string From, string? To)> Places(string docs, string save, string? other = null)
    {
        foreach (var c in Of(docs))
            if (KeyOf(c, save) is { } key)
                foreach (var p in c.Paths)
                    yield return (c, p, Full(docs, p, key), other is null ? null : KeyOf(c, other) is { } to && to != key ? Full(docs, p, to) : null);
    }

    // the files that go into a backup of the save: (name in the zip, file); a place that is not there has none
    internal static List<(string Entry, string File)> Files(string docs, string save)
    {
        var list = new List<(string, string)>();
        foreach (var (c, p, full, _) in Places(docs, save))
        {
            var entry = Top + c.Mod + "/" + p;
            if (File.Exists(full)) list.Add((entry, full));
            else if (Directory.Exists(full))
                foreach (var f in Directory.EnumerateFiles(full, "*", Deep).Where(f => !Saves.Passing(f)))
                    list.Add((entry + "/" + Path.GetRelativePath(full, f).Replace('\\', '/'), f));
        }
        return list;
    }

    // ---------- from a backup ----------

    // A zip's companion data laid out and checked in a work folder (the same checks and bounds as the save's files), before
    // anything changes. Null: it has none.
    internal static string? Unpack(string docs, string zip, CancellationToken ct)
    {
        using (var z = System.IO.Compression.ZipFile.OpenRead(zip))
            if (!z.Entries.Any(e => IsCompanion(e.FullName))) return null;
        var work = Path.Combine(WorkRoot(docs), Guid.NewGuid().ToString("N"));
        try { SaveCopies.Unzip(zip, work, ct, Top); }
        catch { Drop(work); throw; }
        return work;
    }
    internal static bool IsCompanion(string entry) => entry.Replace('\\', '/').StartsWith(Top, StringComparison.Ordinal);

    // What Unpack laid out, put in the save's places. replace: what is there leaves for the Recycle Bin (a restore over the
    // save, whose "before restoring" backup holds it), and so does a place the backup has nothing for when it has the mod's
    // data (it was not there that day); otherwise a place that is taken keeps what it has. A backup without the mod's data
    // (made before the mod declared it) leaves the mod's places as they are. Only the places declared now are written; the
    // work folder is removed.
    internal static void PutIn(string docs, string work, string save, bool replace)
    {
        try
        {
            foreach (var (c, p, to, _) in Places(docs, save))
            {
                var from = Path.Combine(work, c.Mod, p);
                if (!Exists(from))
                {
                    if (replace && Directory.Exists(Path.Combine(work, c.Mod)) && Exists(to)) SaveCopies.Recycle(to);
                    continue;
                }
                if (Exists(to) && !replace) { Engine.Log(new IOException($"save companion {c.Mod}: {to} is taken, left as it is")); continue; }
                Place(from, to);
            }
        }
        finally { Drop(work); }
    }

    internal static void FromZip(string docs, string zip, string save, bool replace, CancellationToken ct)
    {
        if (Unpack(docs, zip, ct) is { } work) PutIn(docs, work, save, replace);
    }

    // ---------- with the save ----------

    // a duplicate gets its own copy; a place the new name already has keeps what it has
    internal static void Copy(string docs, string from, string to, CancellationToken ct)
    {
        foreach (var (c, _, src, dst) in Places(docs, from, to))
            if (dst is not null && Exists(src))
            {
                if (Exists(dst)) { Engine.Log(new IOException($"save companion {c.Mod}: {dst} is taken, not copied")); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                if (File.Exists(src)) { File.Copy(src, dst); if (SaveCopies.Sha(src) != SaveCopies.Sha(dst)) throw new IOException("a copied file did not read back as written"); }
                else SaveCopies.CopyDir(src, dst, ct);
            }
    }

    // a renamed save takes its data along, unless another save still uses the old place
    internal static void Move(string docs, string from, string to)
    {
        foreach (var (c, _, src, dst) in Places(docs, from, to))
            if (dst is not null && Exists(src) && !Shared(docs, c, KeyOf(c, from)!, to))
            {
                if (Exists(dst)) { Engine.Log(new IOException($"save companion {c.Mod}: {dst} is taken, not moved")); continue; }
                Place(src, dst);
            }
    }

    // a deleted save's data to the Recycle Bin, unless another save still uses it
    internal static void Delete(string docs, string save)
    {
        foreach (var (c, _, at, _) in Places(docs, save))
            if (Exists(at) && !Shared(docs, c, KeyOf(c, save)!, null)) SaveCopies.Recycle(at);
    }

    // from to to; what was at to steps aside first, comes back on a failure and goes to the Recycle Bin after
    // shortcut: a crash between the two moves leaves <place>.apex-old-<id> beside the place; nothing tidies it at start yet
    static void Place(string from, string to)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        string? old = null;
        if (Exists(to)) { old = $"{to}.apex-old-{Guid.NewGuid():N}"; MoveAny(to, old); }
        try { MoveAny(from, to); }
        catch { if (old is not null) MoveAny(old, to); throw; }
        if (old is not null) try { SaveCopies.Recycle(old); } catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // (a folder can't move to another drive in one step: there it is copied, checked, and the original removed)
    static void MoveAny(string from, string to)
    {
        if (File.Exists(from)) { Fs.Retry(() => File.Move(from, to)); return; }
        if (SaveCopies.Drive(from).Equals(SaveCopies.Drive(Path.GetDirectoryName(to)!), StringComparison.OrdinalIgnoreCase)) { Fs.Retry(() => Directory.Move(from, to)); return; }
        SaveCopies.CopyDir(from, to, default);
        Directory.Delete(from, true);
    }

    static void Drop(string dir)
    {
        try
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            if (Path.GetDirectoryName(dir) is { } up && Path.GetFileName(up) == ".apex-work" && !Directory.EnumerateFileSystemEntries(up).Any()) Directory.Delete(up);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // work folders a closed launcher left (call with the saves' gate held)
    internal static void Tidy(string docs) => Drop(WorkRoot(docs));
}
