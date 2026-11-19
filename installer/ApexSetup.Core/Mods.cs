namespace ApexSetup;

public enum ModKind { Package, Script, Cas, Objects, Asi }

// One mod file as the Mods screen lists it. Rel is its path as the game sees it (under Mods, or under Game\Bin for an
// ASI), the same whether it is on or off. Locked: the launcher installed it (switched in Components) or it is in Game\Bin.
public sealed record ModFile(string Path, string Rel, long Size, ModKind Kind, bool Enabled, bool Locked)
{
    public string Name => System.IO.Path.GetFileName(Path);
    public string Folder => System.IO.Path.GetDirectoryName(Rel) ?? "";
}

// The player's mods: .package files under Documents\...\Mods, and the .asi files in Game\Bin. A mod is switched off by
// moving it to Mods\Disabled (Apex Launcher) (outside Packages, so Resource.cfg does not load it) and on by moving it
// back to the same place. Only the player's Documents is touched: no admin needed.
public static class Mods
{
    public const string DisabledName = "Disabled (Apex Launcher)";
    public static string Disabled(string docs) => Path.Combine(docs, "Mods", DisabledName);

    // resource types that say what a package is (from its index; unverified on every kind of custom content)
    const uint S3sa = 0x073FAA07, Casp = 0x034AEECB, Objd = 0x319E4F1D;
    // the thumbnail types of CAS parts and objects, tried in this order (unverified beyond a few packages)
    public static readonly uint[] ThumbTypes = [0x0580A2B4, 0x0580A2B5, 0x0580A2B6, 0x0589DC44, 0x0589DC45, 0x0589DC46, 0x0589DC47];

    public static List<ModFile> List(Game g, string docs, bool kinds = true)
    {
        var list = new List<ModFile>();
        var mods = Path.Combine(docs, "Mods");
        var off = Disabled(docs);
        var managed = Manifest.Load(g.Bin).Files.Select(f => Path.IsPathRooted(f.Path) ? f.Path : Path.Combine(g.Bin, f.Path))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(mods))
            foreach (var p in Files(mods, "*.package"))
            {
                var isOff = p.StartsWith(off + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
                var rel = Path.Combine("Mods", Path.GetRelativePath(isOff ? off : mods, p));
                list.Add(new ModFile(p, rel, Size(p), kinds ? KindOf(p) : ModKind.Package, !isOff, managed.Contains(p)));
            }
        foreach (var p in new[] { g.Bin, Path.Combine(g.Bin, "scripts"), Path.Combine(g.Bin, "plugins") }.Where(Directory.Exists)
                     .SelectMany(d => Directory.EnumerateFiles(d, "*.asi")))
            list.Add(new ModFile(p, Path.GetRelativePath(g.Root, p), Size(p), ModKind.Asi, true, true));
        return list.OrderBy(m => m.Rel, StringComparer.OrdinalIgnoreCase).ToList();
    }

    static IEnumerable<string> Files(string dir, string pattern)
    {
        try { return Directory.EnumerateFiles(dir, pattern, new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true }).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    static long Size(string p) { try { return new FileInfo(p).Length; } catch (IOException) { return 0; } }

    public static ModKind KindOf(string package)
    {
        var types = Dbpf.Index(package).Select(e => e.Type).ToHashSet();
        return types.Contains(S3sa) ? ModKind.Script : types.Contains(Casp) ? ModKind.Cas : types.Contains(Objd) ? ModKind.Objects : ModKind.Package;
    }

    public static byte[]? Thumbnail(ModFile m) => m.Kind == ModKind.Asi ? null : Dbpf.First(m.Path, ThumbTypes);

    // Off: moved under Mods\Disabled (Apex Launcher) at the same relative place; on: moved back. The new path, or an
    // exception the UI shows (a locked mod, the game running, a dry run).
    public static string SetEnabled(ModFile m, bool on, string docs)
    {
        if (m.Locked) throw new SetupException("This mod is part of what the launcher installs: switch it in Components.");
        Engine.CheckWritable(); // (dry run, or the game running: the game holds its packages open)
        if (m.Enabled == on) return m.Path;
        var under = Path.GetRelativePath("Mods", m.Rel);
        var target = Free(Path.Combine(on ? Path.Combine(docs, "Mods") : Disabled(docs), under));
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Move(m.Path, target);
        return target;
    }

    // every switched-off mod back in its place (the uninstall leaves no mod hidden in the launcher's folder)
    public static int EnableAll(Game g, string docs)
    {
        var n = 0;
        foreach (var m in List(g, docs, kinds: false).Where(m => !m.Enabled))
        {
            var target = Free(Path.Combine(docs, m.Rel));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Move(m.Path, target);
            n++;
        }
        try { if (Directory.Exists(Disabled(docs)) && !Directory.EnumerateFileSystemEntries(Disabled(docs), "*", SearchOption.AllDirectories).Any(e => File.Exists(e))) Directory.Delete(Disabled(docs), true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return n;
    }

    // a file of the same name already there (the player put a copy back by hand): " (2)" before the extension
    static string Free(string path)
    {
        if (!File.Exists(path)) return path;
        var dir = Path.GetDirectoryName(path)!;
        var name = Path.GetFileNameWithoutExtension(path);
        var ext = Path.GetExtension(path);
        for (int i = 2; ; i++)
            if (Path.Combine(dir, $"{name} ({i}){ext}") is var p && !File.Exists(p)) return p;
    }
}
