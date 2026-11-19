namespace ApexSetup;

// What the game's own launcher installed: store content and Sims3Packs of objects and Create a Sim parts are not kept as
// files in Mods but merged into Documents\...\DCCache\dcdb0.dbc, dcdb1.dbc... (DBPF 2.0 packages, checked on a player's
// 26 files, 267,766 resources). They keep no Sims3Pack manifest, so the list is per item, as the game's launcher shows it:
// every catalog item (an OBJD or a build catalog resource, named from the English string tables) and every Create a Sim
// part (its own name). Read only: removing them is the game's launcher's job.
public sealed record InstalledItem(string Name, string Kind, string File, uint Type, ulong Instance, ulong PngInstance)
{
    // the id lots and worlds name it by in their <Dependency> (0: its record was not found)
    public uint Id { get; init; }
    // its whole id as the game removes it by ("0x" and 32 hex digits; null: not known)
    public string? FullId { get; init; }
}

public static class InstalledContent
{
    public const string Folder = "DCCache";
    static readonly uint[] CasThumbTypes = [0x626F60CE, 0x626F60CD, 0x626F60CC];
    static readonly uint[] ObjectThumbTypes = [0x0580A2B6, 0x0580A2B5, 0x0580A2B4]; // (the 128 px one first: tiles are 128 px)

    public static IEnumerable<string> Files(string docs)
    {
        var dir = Path.Combine(docs, Folder);
        try { return Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "dcdb*.dbc").Order(StringComparer.OrdinalIgnoreCase).ToList() : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    public static long Bytes(string docs) => Files(docs).Sum(f => { try { return new FileInfo(f).Length; } catch (IOException) { return 0L; } });

    public static List<InstalledItem> Read(string docs, CancellationToken ct = default)
    {
        var list = new List<InstalledItem>();
        var full = new Dictionary<uint, string>();
        foreach (var f in Files(docs))
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                var index = Dbpf.Index(f);
                var ids = new Dictionary<(uint, ulong), uint>();
                foreach (var e in index.Where(e => e.Type == DirectoryType))
                    if (Dbpf.Read(f, e) is { } dir)
                    {
                        foreach (var (key, id) in ItemIds(dir)) ids.TryAdd(key, id);
                        foreach (var (id, fid) in FullIds(dir)) full.TryAdd(id, fid);
                    }
                // English names: the string tables whose instance's top byte is 0
                var names = new Dictionary<ulong, string>();
                foreach (var e in index.Where(e => e.Type == ModKinds.Stbl && e.Instance >> 56 == 0))
                    if (Dbpf.Read(f, e) is { } b)
                        foreach (var (k, t) in ModContent.Strings(b)) names.TryAdd(k, t);
                foreach (var it in ModKinds.Read(f).Items)
                    if (names.GetValueOrDefault(it.NameKey)?.Trim() is { Length: > 0 } name)
                        list.Add(new(name, (it.Kinds.FirstOrDefault()).ToString(), f, it.Type, it.Instance, it.PngInstance) { Id = ids.GetValueOrDefault((it.Type, it.Instance)) });
                foreach (var e in index.Where(e => e.Type == ModKinds.Casp))
                    if (Dbpf.Read(f, e) is { } b && ModContent.Part(b) is { } part && part.Name.Length > 0)
                        list.Add(new(part.Name, ModKinds.CasKind(part.Clothing, part.AgeGender).ToString(), f, e.Type, e.Instance, e.Instance) { Id = ids.GetValueOrDefault((e.Type, e.Instance)) });
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException) { Engine.Log(e); }
        }
        list.AddRange(LotsAndWorlds(docs, full.Keys.ToHashSet()));
        return list.Select(i => i.FullId is null && full.TryGetValue(i.Id, out var fid) ? i with { FullId = fid } : i).ToList();
    }

    // Installed lots, households and worlds: the game keeps each one's package, Sims3Pack manifest and all, in
    // DCBackup\0x<id>.package (resource 0x73E93EEB: title, type, picture); one whose id is no longer in the cache was
    // removed (the game leaves its backup behind).
    public const uint ManifestType = 0x73E93EEB;
    public static IEnumerable<InstalledItem> LotsAndWorlds(string docs, HashSet<uint> installed)
    {
        var dir = Path.Combine(docs, "DCBackup");
        IEnumerable<string> files;
        try { files = Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "0x*.package").ToList() : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { yield break; }
        foreach (var f in files)
        {
            InstalledItem? item = null;
            try
            {
                var index = Dbpf.Index(f);
                var m = index.FirstOrDefault(e => e.Type == ManifestType);
                if (m.Type != ManifestType || Dbpf.Read(f, m) is not { } b) continue;
                var x = System.Xml.Linq.XDocument.Parse(System.Text.Encoding.UTF8.GetString(b).TrimEnd('\0')).Root;
                var id = ((string?)x?.Element("packageid"))?.Trim().ToLowerInvariant() ?? "";
                if (x is null || id.Length != 34 || !uint.TryParse(id.AsSpan(2, 8), System.Globalization.NumberStyles.HexNumber, null, out var shortId) || !installed.Contains(shortId)) continue;
                var kind = ((string?)x.Attribute("packagetype"))?.ToLowerInvariant() switch { "world" => "World", "household" => "Household", _ => "Lot" };
                // its picture: the manifest names it as type:group:instance
                var thumb = ((string?)x.Element("thumbnail"))?.Split(':');
                uint type = 0; ulong inst = 0;
                if (thumb is { Length: 3 } && uint.TryParse(thumb[0], System.Globalization.NumberStyles.HexNumber, null, out var t) && ulong.TryParse(thumb[2], System.Globalization.NumberStyles.HexNumber, null, out var i)) (type, inst) = (t, i);
                item = new(((string?)x.Element("packagetitle"))?.Trim() is { Length: > 0 } n ? n : id, kind, f, type, inst, 0) { Id = shortId, FullId = id };
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or System.Xml.XmlException or ArgumentException or IndexOutOfRangeException) { Engine.Log(e); }
            if (item is not null) yield return item;
        }
    }

    // each record's whole id, as the game names it: the record keeps its two 8-byte halves swapped
    static IEnumerable<(uint Id, string Full)> FullIds(byte[] d)
    {
        foreach (var (id, at) in Records(d))
            yield return (id, "0x" + Convert.ToHexString(d, at + 12, 8).ToLowerInvariant() + Convert.ToHexString(d, at + 4, 8).ToLowerInvariant());
    }

    // The ids other content names installed items by: each cache file's directory (type 0x7672F0C5, big-endian) holds a
    // record per installed item: a record number, the item's 16-byte id, the product it needs (16 bytes: an id, 4 zero
    // bytes, the id, 4 zero bytes), the item's id again, then its resources. A Sims3Pack's <Dependency> on a store item is
    // bytes 8 to 11 of that item's id (checked: the 13 store items a player's 9 lots and worlds use, each found once in
    // 5,927 records).
    public const uint DirectoryType = 0x7672F0C5;
    public static HashSet<uint> InstalledIds(string docs)
    {
        var ids = new HashSet<uint>();
        foreach (var f in Files(docs))
            try
            {
                foreach (var e in Dbpf.Index(f).Where(e => e.Type == DirectoryType))
                    if (Dbpf.Read(f, e) is { } d) ids.UnionWith(IdsIn(d));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { Engine.Log(e); }
        return ids;
    }

    public static IEnumerable<uint> IdsIn(byte[] d) => Records(d).Select(r => r.Id);

    // where each record starts and its id
    static List<(uint Id, int At)> Records(byte[] d)
    {
        var list = new List<(uint, int)>();
        for (var i = 0; i + 52 <= d.Length; i++)
        {
            var s = d.AsSpan(i);
            if (!s.Slice(4, 16).SequenceEqual(s.Slice(36, 16)) || !s.Slice(20, 4).SequenceEqual(s.Slice(28, 4))
                || s.Slice(24, 4).IndexOfAnyExcept((byte)0) >= 0 || s.Slice(32, 4).IndexOfAnyExcept((byte)0) >= 0 || s.Slice(4, 16).IndexOfAnyExcept((byte)0) < 0) continue;
            list.Add((System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(s.Slice(12, 4)), i));
            i += 51;
        }
        return list;
    }

    // the catalog entries and parts each record installed, by their key: a record lists its resources as type, group and
    // an instance whose two 32-bit halves are swapped (checked: every OBJD and CASP key of a player's dcdb0 matches its index)
    public static IEnumerable<((uint Type, ulong Instance) Key, uint Id)> ItemIds(byte[] d)
    {
        var records = Records(d);
        for (var r = 0; r < records.Count; r++)
        {
            var end = r + 1 < records.Count ? records[r + 1].At : d.Length;
            for (var i = records[r].At + 52; i + 16 <= end; i++)
            {
                var t = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(i));
                if (t != ModKinds.Objd && t != ModKinds.Casp && !ModKinds.IsCatalog(t)) continue;
                var v = System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(d.AsSpan(i + 8));
                yield return ((t, v << 32 | v >> 32), records[r].Id);
                i += 15;
            }
        }
    }

    // an item's thumbnail from the same file: an object's THUM by its instance or ICON by its picture, a part's own
    static readonly Dictionary<string, List<Dbpf.Entry>> Indexes = new(StringComparer.OrdinalIgnoreCase);
    public static byte[]? Thumbnail(InstalledItem item)
    {
        List<Dbpf.Entry> index;
        lock (Indexes) if (!Indexes.TryGetValue(item.File, out index!)) Indexes[item.File] = index = Dbpf.Index(item.File);
        // (a lot or world names its own picture)
        if (item.Kind is "Lot" or "World" or "Household")
            return item.Type != 0 && index.FirstOrDefault(e => e.Type == item.Type && e.Instance == item.Instance) is { Size: > 0 } own ? Dbpf.Read(item.File, own) : null;
        var tries = item.Type == ModKinds.Casp
            ? new[] { (CasThumbTypes, item.Instance) }
            : new[] { (ObjectThumbTypes, item.Instance), (ModKinds.IconTypes, item.PngInstance) };
        foreach (var (types, inst) in tries)
            foreach (var t in types)
                if (inst != 0 && index.FirstOrDefault(e => e.Type == t && e.Instance == inst) is { Size: > 0 } e) return Dbpf.Read(item.File, e);
        return null;
    }
}
