using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

// What the launcher checks before it adds a file to the player's mods, so a broken or wrong file never reaches the game:
// a .package must be a Sims 3 package (DBPF 2.0; 2.1 is The Sims 4's), not a protected one, with an index that lies inside
// the file and resources that do too; a zip's packages are checked one by one. A file already installed (same bytes) is not added twice. Nothing here writes; Inspect is read-only and
// ModLibrary.AddAny checks every file again before it copies.
public static class ModInstall
{
    public sealed record Item(string Name, string Source, string? ZipEntry, string Kind, long Size, List<string> Problems, List<string> Warnings, string? SameAs)
    {
        public bool Ok => Problems.Count == 0;
    }

    // every file the player gave (zips opened), each with what is wrong with it and what is worth knowing
    public static List<Item> Inspect(IEnumerable<string> paths, IReadOnlyList<ModFile> installed)
    {
        var hashes = installed.Where(m => m.FileType == "package").GroupBy(m => m.Size).ToDictionary(g => g.Key, g => g.ToList());
        var seen = new Dictionary<string, string>(); // (by hash, the first file of this batch: a zip and its own extracted copy, or a file given twice)
        var items = new List<Item>();
        foreach (var p in paths)
        {
            if (Directory.Exists(p)) { foreach (var f in Directory.EnumerateFiles(p, "*", SearchOption.AllDirectories)) items.Add(CheckFile(f, null, hashes, seen)); continue; }
            if (p.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) { items.AddRange(CheckZip(p, hashes, seen)); continue; }
            items.Add(CheckFile(p, null, hashes, seen));
        }
        return items;
    }

    static Item CheckFile(string path, string? entry, Dictionary<long, List<ModFile>> installed, Dictionary<string, string> seen)
    {
        var name = Path.GetFileName(entry ?? path);
        var ext = Path.GetExtension(name).ToLowerInvariant();
        var kind = ext == ".package" ? "package" : "";
        var problems = new List<string>();
        var warnings = new List<string>();
        long size = 0;
        string? same = null;
        try
        {
            size = new FileInfo(path).Length;
            if (kind == "") problems.Add("Not a mod file: only .package files and zips of them are added.");
            else if (size == 0) problems.Add("The file is empty.");
            else problems.AddRange(PackageProblems(path, warnings));
            if (problems.Count == 0)
            {
                var hash = Hash(path);
                if (installed.TryGetValue(size, out var sameSize))
                    same = sameSize.FirstOrDefault(m => { try { return Hash(m.Path) == hash; } catch (IOException) { return false; } })?.Name;
                if (!seen.TryAdd(hash, name)) same ??= seen[hash];
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { problems.Add("The file could not be read."); }
        return new Item(name, path, entry, kind, size, problems, warnings, same);
    }

    // a zip's mod files, each taken out to a temporary file and checked like a loose one
    static IEnumerable<Item> CheckZip(string zip, Dictionary<long, List<ModFile>> installed, Dictionary<string, string> seen)
    {
        var items = new List<Item>();
        var tmp = Path.Combine(Path.GetTempPath(), "apex-zip-" + Guid.NewGuid().ToString("N"));
        try
        {
            using var z = ZipFile.OpenRead(zip);
            foreach (var e in z.Entries.Where(e => e.Length > 0))
            {
                var n = Path.GetFileName(e.FullName.Replace('\\', '/'));
                if (!n.EndsWith(".package", StringComparison.OrdinalIgnoreCase)) continue;
                if (e.Length > 4L << 30) { items.Add(new(n, zip, e.FullName, "package", e.Length, ["The file is too big for the game (over 4 GB)."], [], null)); continue; }
                Directory.CreateDirectory(tmp);
                var t = Path.Combine(tmp, Guid.NewGuid().ToString("N") + Path.GetExtension(n));
                e.ExtractToFile(t);
                items.Add(CheckFile(t, e.FullName, installed, seen) with { Source = zip });
            }
            if (items.Count == 0) items.Add(new(Path.GetFileName(zip), zip, null, "", new FileInfo(zip).Length, ["The zip holds no .package."], [], null));
        }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            items.Add(new(Path.GetFileName(zip), zip, null, "", 0, ["The zip could not be opened."], [], null));
        }
        finally { try { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); } catch (IOException) { } }
        return items;
    }

    // DBPF header and index, by the format the game reads (and Dbpf.Index): what makes the game skip or choke on a package
    public static List<string> PackageProblems(string path, List<string>? warnings = null)
    {
        var p = new List<string>();
        using var f = File.OpenRead(path);
        if (f.Length < 96) { p.Add("Not a package: the file is too short."); return p; }
        var head = new byte[96];
        f.ReadExactly(head);
        var magic = System.Text.Encoding.ASCII.GetString(head, 0, 4);
        if (magic == "DBPP") { p.Add("A protected package: the game can't read it."); return p; }
        if (magic != "DBPF") { p.Add("Not a package: the file doesn't start like one."); return p; }
        var major = BitConverter.ToUInt32(head, 4);
        var minor = BitConverter.ToUInt32(head, 8);
        if (major == 2 && minor == 1) { p.Add("A package for The Sims 4: The Sims 3 can't use it."); return p; }
        if (major != 2 || minor != 0) { p.Add("A package for another game or version."); return p; }
        var count = BitConverter.ToUInt32(head, 0x24);
        var indexSize = BitConverter.ToUInt32(head, 0x2C);
        var at = BitConverter.ToUInt32(head, 0x40);
        if (count == 0) { p.Add("The package is empty."); return p; }
        if (at + (long)indexSize > f.Length || indexSize < 4 + count * 16L) { p.Add("The package is damaged: its index points outside the file."); return p; }
        f.Position = 0;
        var entries = Dbpf.Index(f);
        if (entries.Count != count) { p.Add("The package is damaged: its index can't be read."); return p; }
        if (entries.Any(e => e.Offset + (long)e.Size > f.Length)) p.Add("The package is damaged: part of it is missing. The download may have been cut short.");
        if (warnings is not null && entries.Any(e => e.Type == 0x073FAA07)) warnings.Add("A script mod: it runs code in the game. Install it only from a creator you trust.");
        return p;
    }

    static string Hash(string path) { using var s = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(s)); }
}
