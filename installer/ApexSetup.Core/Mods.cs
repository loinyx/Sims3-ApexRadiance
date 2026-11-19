namespace ApexSetup;

public enum ModKind { Package, Script, Cas, Objects, Lighting, Sound, Textures, Tuning, Interface, Asi }

// One mod file as the Mods screen lists it. Rel is its path as the game sees it (under Mods, or under Game\Bin for an
// ASI), the same whether it is on or off. Locked: the launcher installed it (switched in Components) or it is in Game\Bin.
public sealed record ModFile(string Path, string Rel, long Size, ModKind Kind, bool Enabled, bool Locked)
{
    // the file's own type, for the Mods screen's tabs: a package of Mods or an ASI plugin
    public string FileType => Path.EndsWith(".asi", StringComparison.OrdinalIgnoreCase) ? "asi" : "package";
    public string Name => System.IO.Path.GetFileName(Path);
    public string Folder => System.IO.Path.GetDirectoryName(Rel) ?? "";
}

// The player's mods: .package files under Documents\...\Mods and the .asi files in Game\Bin. A mod is switched off by
// moving it to Mods\Disabled (Apex Launcher) (outside Packages, so Resource.cfg does not load it) and on by moving it
// back to the same place. Only the player's Documents is touched: no admin needed.
public static class Mods
{
    public const string DisabledName = "Disabled (Apex Launcher)";
    public static string Disabled(string docs) => Path.Combine(docs, "Mods", DisabledName);

    // resource types that say what a package is, read from its index (checked on a player's 80 mods)
    const uint S3sa = 0x073FAA07, Casp = 0x034AEECB, Objd = 0x319E4F1D, Objk = 0x02DC343F, Ftpt = 0xD382BF57, Lite = 0x03B4C61D,
        LightFx = 0x1F886EAD, Img = 0x00B2D882, Png = 0x2F7D0004, Xml = 0x0333406C, Itun = 0x03B33DDF, Layout = 0x025C95B6,
        Stbl = 0x220557DA, PackageManifest = 0x0166038C;
    static readonly uint[] Audio = [0x01A527DB, 0x01EEF63A, 0x8070223D];
    // the thumbnail types of CAS parts and objects, tried in this order: an object's 128, 54 then 32 px one (a 32 px picture
    // stretched over a 148 px tile reads blurred), then the others (unverified beyond a few packages)
    public static readonly uint[] ThumbTypes = [0x0580A2B6, 0x0580A2B5, 0x0580A2B4, 0x0589DC47, 0x0589DC46, 0x0589DC45, 0x0589DC44];

    public static List<ModFile> List(Game g, string docs, bool kinds = true)
    {
        var list = new List<ModFile>();
        var mods = Path.Combine(docs, "Mods");
        var off = Disabled(docs);
        var merged = ModMerge.Folder(docs) + Path.DirectorySeparatorChar; // (originals kept by a merge: not the player's mods now)
        var managed = Manifest.Load(g.Bin).Files.Select(f => Path.IsPathRooted(f.Path) ? f.Path : Path.Combine(g.Bin, f.Path))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(mods))
            foreach (var p in Files(mods, "*.package").Where(p => !p.StartsWith(merged, StringComparison.OrdinalIgnoreCase)))
            {
                var isOff = p.StartsWith(off + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
                var rel = Path.Combine("Mods", Path.GetRelativePath(isOff ? off : mods, p));
                list.Add(new ModFile(p, rel, Size(p), kinds ? KindOf(p) : ModKind.Package, !isOff, managed.Contains(p)));
            }
        foreach (var p in new[] { g.Bin, Path.Combine(g.Bin, "scripts"), Path.Combine(g.Bin, "plugins") }.Where(Directory.Exists)
                     .SelectMany(d => Directory.EnumerateFiles(d, "*.asi")))
            list.Add(new ModFile(p, Path.GetRelativePath(g.Root, p), Size(p), ModKind.Asi, true, true));
        // the player's packages first, then the game folder's plugins
        return list.OrderBy(m => m.FileType == "asi" ? 1 : 0)
            .ThenBy(m => m.Rel, StringComparer.OrdinalIgnoreCase).ToList();
    }

    static IEnumerable<string> Files(string dir, string pattern)
    {
        try { return Directory.EnumerateFiles(dir, pattern, new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true }).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    static long Size(string p) { try { return new FileInfo(p).Length; } catch (IOException) { return 0; } }

    // what a mod is, from how many resources of each type it holds: a script first (the rest of it serves the script), then
    // CAS parts and objects, then what most of it is (light, sound, images, tuning, interface layouts)
    public static ModKind KindOf(string package) => KindOf(Dbpf.Index(package).GroupBy(e => e.Type).ToDictionary(x => x.Key, x => x.Count()));

    public static ModKind KindOf(IReadOnlyDictionary<uint, int> n)
    {
        int N(uint t) => n.TryGetValue(t, out var c) ? c : 0;
        if (N(S3sa) > 0) return ModKind.Script;
        if (N(Casp) > 0 && N(Casp) >= N(Objd)) return ModKind.Cas;
        if (N(Objd) + N(Objk) + N(Ftpt) > 0) return ModKind.Objects;
        var total = n.Where(x => x.Key is not (Stbl or PackageManifest)).Sum(x => x.Value);
        if (total == 0) return ModKind.Package;
        bool Most(int c) => c * 2 >= total;
        if (Most(N(Lite) + N(LightFx))) return ModKind.Lighting;
        if (Most(Audio.Sum(N))) return ModKind.Sound;
        if (Most(N(Img) + N(Png))) return ModKind.Textures;
        if (Most(N(Layout))) return ModKind.Interface;
        if (N(Xml) + N(Itun) > 0) return ModKind.Tuning;
        return ModKind.Package;
    }

    public static byte[]? Thumbnail(ModFile m) => m.Kind switch
    {
        _ when m.FileType == "asi" => null,
        _ => Dbpf.First(m.Path, ThumbTypes),
    };

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
        Fs.Retry(() => File.Move(m.Path, target)); // (an antivirus or OneDrive scan holds a file a moment)
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
            Fs.Retry(() => File.Move(m.Path, target));
            n++;
        }
        try { if (Directory.Exists(Disabled(docs)) && !Directory.EnumerateFileSystemEntries(Disabled(docs), "*", SearchOption.AllDirectories).Any(e => File.Exists(e))) Fs.Retry(() => Directory.Delete(Disabled(docs), true)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return n;
    }

    // a file of the same name already there (the player put a copy back by hand): " (2)" before the extension. otherSide: the
    // same folder on the other side, on or off, whose names are taken too (an on and an off mod there would share one Rel)
    public static string Free(string path, string? otherSide = null)
    {
        bool Taken(string p) => File.Exists(p) || otherSide is not null && File.Exists(Path.Combine(otherSide, Path.GetFileName(p)));
        if (!Taken(path)) return path;
        var dir = Path.GetDirectoryName(path)!;
        var name = Path.GetFileNameWithoutExtension(path);
        var ext = Path.GetExtension(path);
        for (int i = 2; ; i++)
            if (Path.Combine(dir, $"{name} ({i}){ext}") is var p && !Taken(p)) return p;
    }
}
