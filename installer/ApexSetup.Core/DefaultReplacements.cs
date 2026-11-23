using System.Security.Cryptography;
using System.Text;

namespace ApexSetup;

// One game item a mod replaces: an object or other catalog resource (Name: the game's English catalog name, else the
// object's internal name), or a CAS part (Name: the part's internal name as the mod carries it). Resources: the mod's
// replacing resources of this item (itself and its thumbnails, by instance).
public sealed record ReplacedItem(uint Type, uint Group, ulong Instance, ulong PngInstance, string? Name, Kind Kind, int Resources);

// What of a package replaces the game's own resources. Resources: the package's resources (name map and manifest left
// out); Replaced: how many of them carry a key the game ships; Images: replaced textures and images that belong to no
// listed item; Other: the rest by resource type name.
public sealed record Replacements(int Resources, int Replaced, IReadOnlyList<ReplacedItem> Objects, IReadOnlyList<ReplacedItem> CasParts,
    int Images, IReadOnlyList<(string Type, int Count)> Other)
{
    // a default replacement changes something the player sees: an object, a Create a Sim part or a texture; a mod that
    // only overrides the game's tuning or scripts (gameplay overhauls) is not one
    public bool IsDefault => Objects.Count > 0 || CasParts.Count > 0 || Images > 0;
    // 1: a pure default replacement; below 1: overrides bundled with new content
    public double Share => Resources == 0 ? 0 : (double)Replaced / Resources;
    public static readonly Replacements None = new(0, 0, [], [], 0, []);
}

// The keys (type, group, instance) of every resource the game ships, base game and installed packs, as a sorted array of
// 64-bit hashes; a mod resource with one of those keys replaces the game's. Measured on a Steam install with all 11
// expansions and 9 stuff packs: 116 packages, 835,917 entries, 770,882 distinct keys (6 MB of hashes).
public sealed class GameIndex
{
    internal readonly IReadOnlyList<string> Sources;
    internal readonly ulong Fingerprint;
    readonly ulong[] _keys;
    readonly Lazy<Dictionary<ulong, string>> _names;
    readonly Lazy<Dictionary<ulong, List<(int Source, Dbpf.Entry Entry)>>> _thumbs;

    internal GameIndex(IReadOnlyList<string> sources, ulong fingerprint, ulong[] keys)
    {
        Sources = sources;
        Fingerprint = fingerprint;
        _keys = keys;
        _names = new(() => DefaultReplacements.LoadNames(this));
        _thumbs = new(() => DefaultReplacements.ThumbMap(this));
    }

    public int Count => _keys.Length;
    public bool Contains(uint type, uint group, ulong instance) => Array.BinarySearch(_keys, DefaultReplacements.Key(type, group, instance)) >= 0;
    internal string? Name(ulong key) => _names.Value.GetValueOrDefault(key);
    internal Dictionary<ulong, List<(int Source, Dbpf.Entry Entry)>> Thumbs => _thumbs.Value;
}

// Default replacements: the player's packages that override the game's own resources, and which game items they replace.
// The Mods screen gets the game's index once in the background (Index: cached in the launcher's data folder, rebuilt when
// a game package changes), then Analyze per mod.
public static class DefaultReplacements
{
    // _KEY name maps and package manifests carry fixed or package-made keys that say nothing about what a mod replaces
    // (seen colliding on 3 of a player's 80 mods each: manifest 0:0, a name map of a hair package and two fixes)
    static bool Ignored(uint type) => type is ModKinds.Nmap or ModKinds.Manifest;

    // thumbnails by the item's instance, 128 px first (measured in the base game's packages: CAS 32/128/256 px for
    // 0x626F60CC/CD/CE in CasThumbnails, groups 0 and 1; objects 32/54/128 px for 0x0580A2B4/B5/B6 in AllThumbnails)
    static readonly uint[] CasThumbTypes = [0x626F60CD, 0x626F60CE, 0x626F60CC];
    static readonly uint[] ObjectThumbTypes = [0x0580A2B6, 0x0580A2B5, 0x0580A2B4];
    static readonly HashSet<uint> ThumbTypes = [.. ModKinds.ThumbTypes, .. CasThumbTypes, .. Mods.ThumbTypes, .. ModKinds.IconTypes];
    // textures and images (_IMG; jpeg, png and tga as Resource.cfg names 0x2F7D0002/4/6)
    static readonly HashSet<uint> ImageTypes = [ModKinds.Img, 0x2F7D0002, 0x2F7D0004, 0x2F7D0006];

    static string CacheFile => Path.Combine(Paths.Data, "game-keys.bin");
    static string NamesFile => Path.Combine(Paths.Data, "game-names.bin");
    const uint KeysMagic = 0x4B475841, NamesMagic = 0x4E475841, CacheVersion = 1; // "AXGK", "AXGN"

    public static ulong Key(uint type, uint group, ulong instance) => Mix(instance ^ Mix((ulong)type << 32 | group));

    static ulong Mix(ulong z)
    {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
        return z ^ (z >> 31);
    }

    // the packages the game loads its own resources from, Deltas first (they win over the full builds for the names)
    public static List<string> Sources(Game g)
    {
        var dirs = new List<string> { g.Root };
        dirs.AddRange(Expansions.Installed(g).Select(x => x.Dir).Where(d => !dirs.Contains(d, StringComparer.OrdinalIgnoreCase)));
        var files = new List<string>();
        void Add(string dir, bool recurse)
        {
            try
            {
                if (Directory.Exists(dir))
                    files.AddRange(Directory.EnumerateFiles(dir, "*.package", recurse ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        foreach (var d in dirs)
        {
            Add(Path.Combine(d, "GameData", "Shared", "Packages"), false);
            Add(Path.Combine(d, "GameData", "Shared", "DeltaPackages"), true);
            Add(Path.Combine(d, "Thumbnails"), false);
        }
        Add(g.Bin, true); // (gameplay, scripts, simcore, UI, Jazz...)
        return files.Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(f => Path.GetFileName(f).StartsWith("Delta", StringComparison.OrdinalIgnoreCase) ? 0 : 1).ToList();
    }

    // every source's path, size and modified time: a pack installed, removed or patched changes it
    static ulong FingerprintOf(IReadOnlyList<string> sources)
    {
        var sb = new StringBuilder();
        foreach (var f in sources.Order(StringComparer.OrdinalIgnoreCase))
        {
            try { var i = new FileInfo(f); sb.Append(f.ToLowerInvariant()).Append('|').Append(i.Length).Append('|').Append(i.LastWriteTimeUtc.Ticks).Append('\n'); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { sb.Append(f).Append("|?\n"); }
        }
        return BitConverter.ToUInt64(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    // the cached index when it still matches the game, else null (the caller then runs Index in the background)
    public static GameIndex? Cached(Game g)
    {
        var sources = Sources(g);
        var fp = FingerprintOf(sources);
        try
        {
            using var r = new BinaryReader(File.OpenRead(CacheFile));
            if (r.ReadUInt32() != KeysMagic || r.ReadUInt32() != CacheVersion || r.ReadUInt64() != fp) return null;
            var n = r.ReadInt32();
            if (n < 0 || (long)n * 8 != r.BaseStream.Length - r.BaseStream.Position) return null;
            var keys = new ulong[n];
            r.BaseStream.ReadExactly(System.Runtime.InteropServices.MemoryMarshal.AsBytes(keys.AsSpan()));
            for (var i = 1; i < n; i++) if (keys[i] <= keys[i - 1]) return null; // (a damaged file: not sorted)
            return new GameIndex(sources, fp, keys);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or EndOfStreamException) { return null; }
    }

    // the cached index, else built from the game's packages (only their indexes are read) and cached. Slow the first
    // time (0.3 s warm, unverified cold); cancel with the token to stop it, progress reports 0..1 by package.
    public static GameIndex Index(Game g, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        if (Cached(g) is { } cached) return cached;
        var sources = Sources(g);
        var fp = FingerprintOf(sources);
        var list = new List<ulong>(1 << 20);
        for (var i = 0; i < sources.Count; i++)
        {
            ct.ThrowIfCancellationRequested();
            foreach (var e in Dbpf.Index(sources[i]))
                if (!Ignored(e.Type)) list.Add(Key(e.Type, e.Group, e.Instance));
            progress?.Report((i + 1.0) / sources.Count);
        }
        var keys = list.ToArray();
        list = null;
        Array.Sort(keys);
        var n = keys.Length == 0 ? 0 : 1;
        for (var i = 1; i < keys.Length; i++) if (keys[i] != keys[n - 1]) keys[n++] = keys[i];
        Array.Resize(ref keys, n);
        Save(CacheFile, w =>
        {
            w.Write(KeysMagic); w.Write(CacheVersion); w.Write(fp); w.Write(n);
            w.Write(System.Runtime.InteropServices.MemoryMarshal.AsBytes(keys.AsSpan()));
        });
        return new GameIndex(sources, fp, keys);
    }

    // written beside, then moved over: a launcher closed mid-write leaves the old cache or none, never half a file
    static void Save(string path, Action<BinaryWriter> write)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var tmp = path + ".tmp";
            using (var w = new BinaryWriter(File.Create(tmp))) write(w);
            File.Move(tmp, path, true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (no cache: built again next time)
    }

    public static Replacements Analyze(GameIndex gi, ModFile m) => Analyze(gi, m.Path);

    public static Replacements Analyze(GameIndex gi, string path)
    {
        try
        {
            var all = Dbpf.Index(path).Where(e => !Ignored(e.Type)).ToList();
            var hits = all.Where(e => gi.Contains(e.Type, e.Group, e.Instance)).ToList();
            if (hits.Count == 0) return Replacements.None with { Resources = all.Count };
            var objects = new List<ReplacedItem>();
            var parts = new List<ReplacedItem>();
            var parsed = 0;
            using (var f = File.OpenRead(path))
                foreach (var e in hits)
                {
                    var isCatalog = ModKinds.IsCatalog(e.Type);
                    if (!isCatalog && e.Type != ModKinds.Casp) continue;
                    var d = ++parsed <= 5000 ? Dbpf.Read(f, e, 1 << 20) : null;
                    if (isCatalog)
                    {
                        var item = d is null ? null : ModKinds.Catalog(e.Type, e.Instance, d);
                        var kind = item?.Kinds[0] ?? (e.Type == ModKinds.Objd ? Kind.BuyMisc : Kind.Other);
                        objects.Add(new ReplacedItem(e.Type, e.Group, e.Instance, item?.PngInstance ?? 0, gi.Name(Key(e.Type, e.Group, e.Instance)), kind, 1));
                    }
                    else
                    {
                        var p = d is null ? null : ModContent.Part(d);
                        // (a name of numbers only, as "1534988440:1283963347" in a beard pack, says nothing to a player)
                        var name = p?.Name is { } s && s.Any(char.IsLetter) ? s : null;
                        parts.Add(new ReplacedItem(e.Type, e.Group, e.Instance, 0, name,p is { } q ? ModKinds.CasKind(q.Clothing, q.AgeGender) : Kind.CasOther, 1));
                    }
                }

            // a replaced thumbnail of a replaced item counts with the item; other images and thumbnails as images
            var byInstance = objects.Concat(parts).GroupBy(i => i.Instance).ToDictionary(x => x.Key, x => 0);
            var images = 0;
            var other = new Dictionary<string, int>();
            foreach (var e in hits)
            {
                if (ModKinds.IsCatalog(e.Type) || e.Type == ModKinds.Casp) continue;
                if (ThumbTypes.Contains(e.Type) && byInstance.ContainsKey(e.Instance)) byInstance[e.Instance]++;
                else if (ImageTypes.Contains(e.Type) || ThumbTypes.Contains(e.Type)) images++;
                else other[TypeName(e.Type)] = other.GetValueOrDefault(TypeName(e.Type)) + 1;
            }
            ReplacedItem WithThumbs(ReplacedItem i) => byInstance[i.Instance] is > 0 and var t ? i with { Resources = 1 + t } : i;
            return new Replacements(all.Count, hits.Count, objects.Select(WithThumbs).ToList(), parts.Select(WithThumbs).ToList(), images,
                other.OrderByDescending(x => x.Value).Select(x => (x.Key, x.Value)).ToList());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return Replacements.None; }
    }

    // the game's own thumbnail (PNG or JPEG bytes) of a replaced item: a CAS part's from CasThumbnails, an object's THUM
    // by instance from AllThumbnails, else its ICON by png instance; null when the game has none
    public static byte[]? OriginalThumbnail(GameIndex gi, ReplacedItem item)
    {
        var types = item.Type == ModKinds.Casp ? CasThumbTypes : ObjectThumbTypes;
        foreach (var (inst, ts) in new[] { (item.Instance, types), (item.PngInstance, ModKinds.IconTypes) })
        {
            if (inst == 0 || !gi.Thumbs.TryGetValue(inst, out var found)) continue;
            foreach (var t in ts)
                foreach (var (src, e) in found.Where(x => x.Entry.Type == t).OrderBy(x => x.Entry.Group))
                    if (Dbpf.Read(gi.Sources[src], e) is { Length: > 0 } d) return d;
        }
        return null;
    }

    // the mod's own thumbnail of a replaced item, found the same way in the mod's package; null when it brings none (the
    // game then shows its own picture for it)
    static readonly Dictionary<string, List<Dbpf.Entry>> ModIndexes = new(StringComparer.OrdinalIgnoreCase);
    public static byte[]? ModThumbnail(string package, ReplacedItem item)
    {
        List<Dbpf.Entry> index;
        lock (ModIndexes) if (!ModIndexes.TryGetValue(package, out index!)) ModIndexes[package] = index = Dbpf.Index(package);
        var types = item.Type == ModKinds.Casp ? CasThumbTypes : ObjectThumbTypes;
        foreach (var (inst, ts) in new[] { (item.Instance, types), (item.PngInstance, ModKinds.IconTypes) })
        {
            if (inst == 0) continue;
            foreach (var t in ts)
                if (index.FirstOrDefault(e => e.Type == t && e.Instance == inst) is { Size: > 0 } e && Dbpf.Read(package, e) is { Length: > 0 } d) return d;
        }
        return null;
    }
    public static void ForgetModIndex(string package) { lock (ModIndexes) ModIndexes.Remove(package); }

    // the thumbnails and icons the game holds, by instance (about 110,000 entries: built once, when first asked)
    internal static Dictionary<ulong, List<(int, Dbpf.Entry)>> ThumbMap(GameIndex gi)
    {
        var map = new Dictionary<ulong, List<(int, Dbpf.Entry)>>();
        for (var i = 0; i < gi.Sources.Count; i++)
            foreach (var e in Dbpf.Index(gi.Sources[i]))
                if (ThumbTypes.Contains(e.Type))
                {
                    if (!map.TryGetValue(e.Instance, out var l)) map[e.Instance] = l = [];
                    l.Add((i, e));
                }
        return map;
    }

    // the game's catalog names: every catalog resource's (OBJD, wall patterns, fences...) English name by its key hash,
    // else its internal name. All of them rather than only the replaced ones: the 7,390 catalog resources and 45 English
    // string tables of a full install are read in 0.4 s warm and kept as a small file (about 6,000 names).
    internal static Dictionary<ulong, string> LoadNames(GameIndex gi)
    {
        try
        {
            using var r = new BinaryReader(File.OpenRead(NamesFile));
            if (r.ReadUInt32() == NamesMagic && r.ReadUInt32() == CacheVersion && r.ReadUInt64() == gi.Fingerprint)
            {
                var n = r.ReadInt32();
                var d = new Dictionary<ulong, string>(Math.Clamp(n, 0, 1 << 20));
                for (var i = 0; i < n; i++) d[r.ReadUInt64()] = r.ReadString();
                return d;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or EndOfStreamException or ArgumentException) { }
        var names = BuildNames(gi);
        Save(NamesFile, w =>
        {
            w.Write(NamesMagic); w.Write(CacheVersion); w.Write(gi.Fingerprint); w.Write(names.Count);
            foreach (var (k, v) in names) { w.Write(k); w.Write(v); }
        });
        return names;
    }

    static Dictionary<ulong, string> BuildNames(GameIndex gi)
    {
        var nameKey = new Dictionary<ulong, ulong>();
        var internalName = new Dictionary<ulong, string>();
        var tables = new List<(string, Dbpf.Entry)>();
        foreach (var src in gi.Sources)
        {
            var index = Dbpf.Index(src);
            try
            {
                using var f = File.OpenRead(src);
                foreach (var e in index)
                {
                    if (e.Type == ModKinds.Stbl && e.Instance >> 56 == 0) { tables.Add((src, e)); continue; }
                    var k = Key(e.Type, e.Group, e.Instance);
                    if (!ModKinds.IsCatalog(e.Type) || nameKey.ContainsKey(k) || Dbpf.Read(f, e, 1 << 20) is not { } d) continue;
                    if (ModKinds.Catalog(e.Type, e.Instance, d) is { } item) nameKey[k] = item.NameKey;
                    if (e.Type == ModKinds.Objd && ModKinds.ParseObjd(d)?.InstanceName is { Length: > 0 } iname) internalName[k] = iname;
                }
            }
            catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
        }
        var wanted = nameKey.Values.ToHashSet();
        var text = new Dictionary<ulong, string>();
        foreach (var (src, e) in tables)
            if (Dbpf.Read(src, e, 64 << 20) is { } d)
                foreach (var (k, v) in ModContent.Strings(d))
                    if (wanted.Contains(k) && v.Trim() is { Length: > 0 } t) text.TryAdd(k, t);
        var names = new Dictionary<ulong, string>();
        foreach (var (k, nk) in nameKey)
            if ((text.GetValueOrDefault(nk) ?? internalName.GetValueOrDefault(k)) is { } n) names[k] = n;
        return names;
    }

    // a resource type's short name: Resource.cfg's FileType names where the game gives one, else the community's
    // (simswiki Files By Type Code), else its number
    public static string TypeName(uint type) => type switch
    {
        0x0333406C => "XML tuning",
        0x03B33DDF => "Interaction tuning",
        0x025C95B6 => "UI layout",
        0x025C90A6 => "UI style",
        0x073FAA07 => "Script",
        0x220557DA => "Strings",
        0x01A527DB or 0x01EEF63A or 0x8070223D => "Sound",
        0x6B20C4F3 or 0x02D5DF13 => "Animation",
        0x8EAF13DE => "Rig",
        0xD3044521 => "Slots",
        0x03B4C61D => "Lighting",
        0x1F886EAD => "Lighting setup",
        0x0604ABDA => "Dream tree",
        0xEA5118B0 => "Effects",
        0x015A1849 => "Geometry",
        0x067CAA11 => "Blend geometry",
        0x01D10F34 or 0x01661233 => "Model",
        0x736884F1 => "Model parts",
        0x033A1435 => "Texture compositor",
        0xD382BF57 => "Footprint",
        0x02DC343F => "Object key",
        0xB1CC1AF6 => "Video", // (unverified name: the type of the intro videos two no-intro mods replace)
        _ => $"0x{type:X8}",
    };
}
