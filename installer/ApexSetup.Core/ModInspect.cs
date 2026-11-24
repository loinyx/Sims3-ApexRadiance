using System.Buffers.Binary;
using System.Text;

namespace ApexSetup;

// What a resource is, in the words of the Mods screen
public enum DataKind { Tuning, Script, Object, CasPart, Texture, Picture, Text, Model, Animation, Sound, Effect, Light, Interface, Other }

// One thing a package holds: an object or another catalog entry, a Create a Sim part, or a picture that belongs to none of
// them (Kind is null then). Picture: the entry of its thumbnail, read when it is shown.
public sealed record ContentItem(Dbpf.Entry Entry, string? Name, Kind? Kind, Dbpf.Entry? Picture);

// One resource as the list shows it. Name: from the package's name map.
public sealed record ContentResource(Dbpf.Entry Entry, string? Name)
{
    public string Tag => ModInspect.Tag(Entry.Type);
    public string Key => $"{Entry.Type:X8}:{Entry.Group:X8}:{Entry.Instance:X16}";
}

// Parts: how many objects and Create a Sim parts of each kind (English keys, as ModContent.KindOf)
public sealed record PackageContent(bool Protected, IReadOnlyList<ContentItem> Items, IReadOnlyList<(string Kind, int Count)> Parts, IReadOnlyList<ContentResource> Resources)
{
    public static readonly PackageContent Empty = new(false, [], [], []);
}

// Another mod that holds some of the same game data. Wins: the mod asked about counts over it. Different: the resources that
// are not the same in both, by kind; Identical: how many are.
public sealed record SharedMod(ModFile Mod, bool Wins, IReadOnlyList<(DataKind Kind, int Count)> Different, int Identical)
{
    public bool Conflict => Different.Count > 0;
}

// One package for the details' Content and Conflicts tabs. Every read is bounded: bad data leaves out what it spoils.
public static class ModInspect
{
    const int MaxTables = 60, MaxStrings = 400_000, MaxNames = 400_000, MaxName = 200;

    // ---------- content ----------

    // The language a string table is in: the top byte of its instance, in this order (Languages' codes; Brazilian Portuguese
    // and Spanish of Spain come before the other one)
    static readonly string[] TableLanguages =
        ["en", "zh_hans", "zh_hant", "cs", "da", "nl", "fi", "fr", "de", "el", "hu", "it", "ja", "ko", "no", "pl", "pt", "pt", "ru", "es", "es", "sv", "th"];
    public static string? TableLanguage(ulong instance) => instance >> 56 is var n && n < (ulong)TableLanguages.Length ? TableLanguages[n] : null;
    static int TableOrder(ulong instance, string language)
    {
        var n = instance >> 56;
        return TableLanguage(instance) == language ? n == 0x10 ? 1 : 0 : n == 0 ? 2 : 3;
    }

    // language: the player's (a code of Languages); gi: for the names of the game's own objects a mod replaces
    public static PackageContent Read(string path, string language, GameIndex? gi = null, CancellationToken ct = default)
    {
        try
        {
            using var f = Dbpf.OpenShared(path);
            Span<byte> magic = stackalloc byte[4];
            if (f.ReadAtLeast(magic, 4, throwOnEndOfStream: false) == 4 && magic.SequenceEqual("DBPP"u8)) return PackageContent.Empty with { Protected = true };
            return Read(f, Dbpf.Index(path), language, gi, ct);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return PackageContent.Empty; }
    }

    static PackageContent Read(FileStream f, List<Dbpf.Entry> index, string language, GameIndex? gi, CancellationToken ct)
    {
        // the name map's names, and the strings in the player's language, English, then any
        var names = new Dictionary<ulong, string>();
        foreach (var e in index.Where(e => e.Type == ModKinds.Nmap))
            if (Dbpf.Read(f, e) is { } d)
                foreach (var (id, n) in NameMap(d)) { if (names.Count >= MaxNames) break; names.TryAdd(id, n); }
        var text = new Dictionary<ulong, string>();
        if (index.Any(e => ModKinds.IsCatalog(e.Type)))
            foreach (var e in index.Where(e => e.Type == ModKinds.Stbl).OrderBy(e => TableOrder(e.Instance, language)).Take(MaxTables))
            {
                ct.ThrowIfCancellationRequested();
                if (Dbpf.Read(f, e, 4 << 20) is { } d)
                    foreach (var (k, v) in ModContent.Strings(d)) { if (text.Count >= MaxStrings) break; if (Clean(v) is { } s) text.TryAdd(k, s); }
            }

        var pictures = index.Where(e => PictureOrder.ContainsKey(e.Type) && e.Size > 0).ToLookup(e => e.Instance);
        var claimed = new HashSet<ulong>();
        Dbpf.Entry? Picture(params ulong[] instances)
        {
            foreach (var i in instances)
            {
                if (i == 0 || !pictures.Contains(i)) continue;
                claimed.Add(i);
                return pictures[i].MinBy(e => PictureOrder[e.Type]);
            }
            return null;
        }
        var items = new List<ContentItem>();
        var parts = new Dictionary<string, int>();
        foreach (var e in index)
        {
            var catalog = ModKinds.IsCatalog(e.Type);
            if (!catalog && e.Type != ModKinds.Casp) continue;
            ct.ThrowIfCancellationRequested();
            var d = Dbpf.Read(f, e, 1 << 20);
            if (catalog)
            {
                if (e.Type == ModKinds.Objd) parts["Object"] = parts.GetValueOrDefault("Object") + 1;
                var item = d is null ? null : ModKinds.Catalog(e.Type, e.Instance, d);
                var name = (item is not null ? text.GetValueOrDefault(item.NameKey) : d is not null ? ModContent.NameIn(d, text) : null)
                    ?? Clean(gi?.Name(DefaultReplacements.Key(e.Type, e.Group, e.Instance))) ?? names.GetValueOrDefault(e.Instance)
                    ?? (e.Type == ModKinds.Objd && d is not null ? Clean(ModKinds.ParseObjd(d)?.InstanceName) : null);
                items.Add(new(e, name, item?.Kinds[0] ?? (e.Type == ModKinds.Objd ? Kind.BuyMisc : Kind.Other), Picture(e.Instance, item?.PngInstance ?? 0)));
            }
            else
            {
                var part = d is null ? null : ModContent.Part(d);
                // (counted only when it can be read, as a kind of part)
                if (part is { } p) parts[ModContent.KindOf(p.Clothing)] = parts.GetValueOrDefault(ModContent.KindOf(p.Clothing)) + 1;
                // (a part named only by numbers says nothing to a player)
                var name = part is { } q && q.Name.Any(char.IsLetter) ? Clean(q.Name) : null;
                items.Add(new(e, name ?? names.GetValueOrDefault(e.Instance), part is { } r ? ModKinds.CasKind(r.Clothing, r.AgeGender) : Kind.CasOther, Picture(e.Instance)));
            }
        }
        foreach (var g in pictures.Where(g => !claimed.Contains(g.Key)))
        {
            var best = g.MinBy(e => PictureOrder[e.Type]);
            items.Add(new(best, names.GetValueOrDefault(g.Key), null, best));
        }
        return new(false,
            items.OrderBy(i => i.Kind is null ? 2 : i.Entry.Type == ModKinds.Casp ? 1 : 0).ThenBy(i => i.Name is null).ThenBy(i => i.Name, StringComparer.CurrentCultureIgnoreCase).ToList(),
            parts.OrderByDescending(x => x.Value).Select(x => (x.Key, x.Value)).ToList(),
            index.Select(e => new ContentResource(e, names.GetValueOrDefault(e.Instance))).ToList());
    }

    // _KEY: version 1, a count, then per name its instance, the length in bytes and the text
    public static IEnumerable<(ulong Instance, string Name)> NameMap(byte[] d)
    {
        if (d.Length < 8 || BinaryPrimitives.ReadUInt32LittleEndian(d) != 1) yield break;
        var count = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(4));
        var at = 8;
        for (uint i = 0; i < count && at + 12 <= d.Length; i++)
        {
            var id = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(at));
            var len = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(at + 8));
            at += 12;
            if (len < 0 || len > d.Length - at) yield break;
            if (Clean(Encoding.UTF8.GetString(d, at, len)) is { } n) yield return (id, n);
            at += len;
        }
    }

    // a text of the package as one short line, or null when there is nothing to show
    static string? Clean(string? s)
    {
        if (s is null) return null;
        if (s.Length > MaxName) s = s[..MaxName];
        s = new string(s.Select(c => char.IsControl(c) ? ' ' : c).ToArray()).Trim();
        return s.Length > 0 ? s : null;
    }

    // the pictures Windows can show: an object's or a part's thumbnails (the 128 px one first), icons, interface pictures
    static readonly Dictionary<uint, int> PictureOrder = new uint[]
        { 0x626F60CD, 0x626F60CE, 0x0580A2B6, 0x0580A2B5, 0x0589DC47, 0x0589DC46, 0x2E75C766, 0x2E75C767, 0x2E75C765, 0x626F60CC, 0x0580A2B4, 0x0589DC45, 0x0589DC44, 0x2E75C764, ModKinds.UiPng }
        .Select((t, i) => (t, i)).ToDictionary(x => x.t, x => x.i);

    // a resource type's short name (the community's tags), else its number
    public static string Tag(uint type) => Tags.TryGetValue(type, out var t) ? t : type.ToString("X8");
    static readonly Dictionary<uint, string> Tags = new()
    {
        [0x0333406C] = "_XML", [0x03B33DDF] = "ITUN", [0x034AEECB] = "CASP", [0x319E4F1D] = "OBJD", [0x02DC343F] = "OBJK", [0x073FAA07] = "S3SA", [0x0166038C] = "_KEY",
        [0x00B2D882] = "_IMG", [0x033A1435] = "TXTC", [0x220557DA] = "STBL", [0x015A1849] = "GEOM", [0x01661233] = "MODL", [0x01D10F34] = "MLOD", [0x736884F1] = "VPXY",
        [0xD382BF57] = "FTPT", [0xD3044521] = "RSLT", [0x8EAF13DE] = "_RIG", [0x067CAA11] = "BGEO", [0x0355E0A6] = "BOND", [0x062C8204] = "BBLN", [0xB52F5055] = "FBLN",
        [0x0354796A] = "TONE", [0x6B20C4F3] = "CLIP", [0x02D5DF13] = "JAZZ", [0x01A527DB] = "_AUD", [0x01EEF63A] = "_AUD", [0x8070223D] = "AUDT", [0xB1CC1AF6] = "_VID",
        [0x03B4C61D] = "LITE", [0x1F886EAD] = "_INI", [0x025C95B6] = "LAYO", [0x025C90A6] = "_CSS", [0xEA5118B0] = "_SWB", [0xD4D9FBE5] = "PTRN", [0xA8D58BE5] = "SKIL",
        [0xDD3223A7] = "BUFF", [0x73E93EEB] = "INFO", [0x2F7D0002] = "IMAG", [0x2F7D0004] = "PNG", [0x2F7D0006] = "IMAG",
        [0x515CA4CD] = "CWAL", [0x9151E6BC] = "CWST", [0x0418FE2A] = "CFEN", [0x04C58103] = "CRAL", [0x049CA4CD] = "CSTR", [0x91EDBD3E] = "CRST", [0xF1EDBD86] = "CRMT",
        [0x316C78F2] = "CFND", [0x04F3CC01] = "CFIR", [0x0A36F07A] = "CCFP", [0x04ED4BB2] = "CTPT", [0x04B30669] = "CTTL", [0x060B390C] = "CWAT", [0xCF9A4ACE] = "MDLR",
        [0x2E75C764] = "ICON", [0x2E75C765] = "ICON", [0x2E75C766] = "ICON", [0x2E75C767] = "ICON", [0x626F60CC] = "THUM", [0x626F60CD] = "THUM", [0x626F60CE] = "THUM",
        [0x0580A2B4] = "THUM", [0x0580A2B5] = "THUM", [0x0580A2B6] = "THUM", [0x0589DC44] = "THUM", [0x0589DC45] = "THUM", [0x0589DC46] = "THUM", [0x0589DC47] = "THUM",
    };

    public static DataKind KindOf(uint type) => type switch
    {
        _ when ModKinds.IsCatalog(type) => DataKind.Object,
        _ => Tag(type) switch
        {
            "_XML" or "ITUN" or "SKIL" or "BUFF" => DataKind.Tuning,
            "S3SA" => DataKind.Script,
            "OBJK" or "MDLR" or "CFND" => DataKind.Object,
            "CASP" or "TONE" or "FBLN" or "BBLN" or "BOND" => DataKind.CasPart,
            "_IMG" or "TXTC" or "PTRN" => DataKind.Texture,
            "THUM" or "ICON" or "PNG" or "IMAG" => DataKind.Picture,
            "STBL" => DataKind.Text,
            "GEOM" or "MODL" or "MLOD" or "VPXY" or "FTPT" or "RSLT" or "_RIG" or "BGEO" => DataKind.Model,
            "CLIP" or "JAZZ" => DataKind.Animation,
            "_AUD" or "AUDT" => DataKind.Sound,
            "_SWB" => DataKind.Effect,
            "LITE" or "_INI" => DataKind.Light,
            "LAYO" or "_CSS" => DataKind.Interface,
            _ => DataKind.Other,
        },
    };

    // the keys as text, one resource a line: key, type, size in bytes, name
    public static string Export(IEnumerable<ContentResource> resources) =>
        string.Concat(resources.Select(r => $"{r.Key}\t{r.Tag}\t{r.Entry.Size}\t{r.Name}".TrimEnd('\t') + Environment.NewLine));

    // ---------- conflicts ----------

    // the conflict of Health's check about these two mods' data (Health names a mod by its file)
    public static ModConflict? InHealth(IEnumerable<ModConflict> found, ModFile mod, SharedMod other) => !other.Conflict ? null
        : found.Where(c => IsData(c) && Names(c, mod) && Names(c, other.Mod)).OrderBy(c => c.Level).FirstOrDefault();

    // Health's other conflicts of a mod: scripts and game code, and data in an archive, which the game does not load.
    // One that names a mod that is no longer on is left to the next check.
    public static List<ModConflict> OnlyInHealth(IEnumerable<ModConflict> found, ModFile mod, IReadOnlyList<ModFile> mods)
    {
        bool File(string party, Func<ModFile, bool> when) => mods.Any(m => when(m) && Path.GetFileNameWithoutExtension(m.Path).Equals(party, StringComparison.OrdinalIgnoreCase));
        return found.Where(c => Names(c, mod) && c.Parties.All(p => File(p, m => m.Enabled) || !File(p, _ => true))
            && (!IsData(c) || c.Parties.Any(p => !File(p, _ => true)))).ToList();
    }

    static bool IsData(ModConflict c) => c.Raw.Split(':')[0] is "resource key" or "decided" or "add-on" or "translation";
    static bool Names(ModConflict c, ModFile m) => c.Files.Contains(m.Path, StringComparer.OrdinalIgnoreCase);
}

// Which packages hold the same game data, by the rules of the conflict scan (ModConflicts): which resources count, which
// differences don't (a name map, a blanked video) and which mod comes first. The scan says which mods clash in all; this says
// it about one package, also one kept inside a merge. It reads every mod's index once; the bytes are compared in Of.
public sealed class SharedData
{
    const int MaxResource = 64 << 20;
    readonly List<ModFile> _mods;
    readonly List<(int, int, string)?> _order;  // each one's place in the load order, null when the game does not load it
    readonly Dictionary<(uint, uint, ulong), List<(int Mod, Dbpf.Entry Entry)>> _held = new(); // only what two or more hold
    readonly ILookup<int, (uint, uint, ulong)> _byMod;

    static bool Counts(uint type) => type is not (ModConflicts.TypeKey or ModConflicts.TypeManifest);
    static (uint, uint, ulong) KeyOf(Dbpf.Entry e) => (e.Type, e.Group, e.Instance);

    public SharedData(string docs, IEnumerable<ModFile> mods, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        // (ccmerged.package is the game's own merged list, never a conflict in Health)
        _mods = mods.Where(m => m.Enabled && m.FileType == "package" && m.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
            && !Path.GetFileNameWithoutExtension(m.Path).Equals("ccmerged", StringComparison.OrdinalIgnoreCase)).ToList();
        List<ModConflicts.Directive>? dirs = null;
        try { dirs = ModConflicts.ParseResourceCfg(File.ReadAllText(Path.Combine(docs, "Mods", "Resource.cfg"))); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // (by name then, as the scan does)
        _order = _mods.Select(m =>
        {
            var under = Path.GetRelativePath("Mods", m.Rel).Replace('\\', '/');
            return dirs is null ? (0, 0, under.ToLowerInvariant()) : ModConflicts.LoadKey(dirs, under);
        }).ToList();

        var first = new Dictionary<(uint, uint, ulong), (int Mod, Dbpf.Entry Entry)>();
        for (var i = 0; i < _mods.Count; i++)
        {
            ct.ThrowIfCancellationRequested();
            foreach (var e in Dbpf.Index(_mods[i].Path))
            {
                if (!Counts(e.Type)) continue;
                var k = KeyOf(e);
                if (first.TryAdd(k, (i, e))) continue;
                if (!_held.TryGetValue(k, out var l))
                {
                    if (first[k].Mod == i) continue; // (twice in one package)
                    _held[k] = l = [first[k]];
                }
                if (l[^1].Mod != i) l.Add((i, e));
            }
            progress?.Report((i + 1.0) / _mods.Count);
        }
        _byMod = _held.SelectMany(h => h.Value.Select(x => (x.Mod, h.Key))).ToLookup(x => x.Mod, x => x.Key);
    }

    // The mods that share game data with a mod that is on. inside: one package kept in it, when it is a merge: only the data
    // that package holds. The ones it conflicts with first.
    public List<SharedMod> Of(ModFile mod, string? inside = null, CancellationToken ct = default)
    {
        var me = _mods.FindIndex(m => m.Path.Equals(mod.Path, StringComparison.OrdinalIgnoreCase));
        if (me < 0) return [];
        var mine = inside is not null
            ? Dbpf.Index(inside).Where(e => Counts(e.Type)).DistinctBy(KeyOf).ToDictionary(KeyOf)
            : _byMod[me].ToDictionary(k => k, k => _held[k].First(x => x.Mod == me).Entry);
        var found = new List<SharedMod>();
        foreach (var other in mine.Where(x => _held.ContainsKey(x.Key)).SelectMany(x => _held[x.Key].Where(o => o.Mod != me).Select(o => (o.Mod, Mine: x.Value, o.Entry))).GroupBy(x => x.Mod))
        {
            // (neither is loaded: neither counts)
            if (_order[me] is null && _order[other.Key] is null) continue;
            var different = new Dictionary<DataKind, int>();
            var identical = 0;
            // (a script's adapted copy of the same build in a folder that loads first: ModConflicts.Adapted)
            var adapted = _order[me] is { } mo && _order[other.Key] is { } oo && mo.Item1 != oo.Item1;
            try
            {
                using var a = Dbpf.OpenShared(inside ?? mod.Path);
                using var b = Dbpf.OpenShared(_mods[other.Key].Path);
                foreach (var (_, ea, eb) in other)
                {
                    ct.ThrowIfCancellationRequested();
                    if (Same(a, ea, b, eb, adapted)) identical++;
                    else different[ModInspect.KindOf(ea.Type)] = different.GetValueOrDefault(ModInspect.KindOf(ea.Type)) + 1;
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; } // (moved or held meanwhile)
            var wins = _order[other.Key] is not { } theirs || _order[me] is { } ours && ModConflicts.CompareKey(ours, theirs) < 0;
            found.Add(new(_mods[other.Key], wins, different.OrderByDescending(x => x.Value).Select(x => (x.Key, x.Value)).ToList(), identical));
        }
        return found.OrderByDescending(x => x.Conflict).ThenBy(x => x.Mod.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    // the scan's rules: one that can't be read counts as different; two that blank the same media are the same fix; tuning and
    // texts by what they mean
    static bool Same(Stream a, Dbpf.Entry ea, Stream b, Dbpf.Entry eb, bool adapted)
    {
        if (ModConflicts.Blankable.Contains(ea.Type) && ea.MemSize <= ModConflicts.TinyBytes && eb.MemSize <= ModConflicts.TinyBytes) return true;
        if (ea.Compressed == eb.Compressed && ea.Size == eb.Size)
        {
            // (stored alike: no need to unpack)
            var (ra, rb) = (Dbpf.Read(a, ea with { Compressed = false }, MaxResource), Dbpf.Read(b, eb with { Compressed = false }, MaxResource));
            if (ra is not null && rb is not null && ra.AsSpan().SequenceEqual(rb)) return true;
        }
        var (da, db) = (Dbpf.Read(a, ea, MaxResource), Dbpf.Read(b, eb, MaxResource));
        if (da is null || db is null) return false;
        if (da.AsSpan().SequenceEqual(db)) return true;
        if (adapted && ea.Type == ModConflicts.TypeS3sa && ModConflicts.Build(da) is { } build) return build == ModConflicts.Build(db);
        return ModConflicts.Meaning(ea.Type, da) == ModConflicts.Meaning(eb.Type, db);
    }
}
