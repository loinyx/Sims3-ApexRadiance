namespace ApexSetup;

// What the game's own launcher installed: store content and Sims3Packs of objects and Create a Sim parts are not kept as
// files in Mods but merged into Documents\...\DCCache\dcdb0.dbc, dcdb1.dbc... (DBPF 2.0 packages, checked on a player's
// 26 files, 267,766 resources). They keep no Sims3Pack manifest, so the list is per item, as the game's launcher shows it:
// every catalog item (an OBJD or a build catalog resource, named from the English string tables) and every Create a Sim
// part (its own name). Read only: removing them is the game's launcher's job.
public sealed record InstalledItem(string Name, string Kind, string File, uint Type, ulong Instance, ulong PngInstance);

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
        foreach (var f in Files(docs))
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                var index = Dbpf.Index(f);
                // English names: the string tables whose instance's top byte is 0
                var names = new Dictionary<ulong, string>();
                foreach (var e in index.Where(e => e.Type == ModKinds.Stbl && e.Instance >> 56 == 0))
                    if (Dbpf.Read(f, e) is { } b)
                        foreach (var (k, t) in ModContent.Strings(b)) names.TryAdd(k, t);
                foreach (var it in ModKinds.Read(f).Items)
                    if (names.GetValueOrDefault(it.NameKey) is { Length: > 0 } name)
                        list.Add(new(name, (it.Kinds.FirstOrDefault()).ToString(), f, it.Type, it.Instance, it.PngInstance));
                foreach (var e in index.Where(e => e.Type == ModKinds.Casp))
                    if (Dbpf.Read(f, e) is { } b && ModContent.Part(b) is { } part && part.Name.Length > 0)
                        list.Add(new(part.Name, ModKinds.CasKind(part.Clothing, part.AgeGender).ToString(), f, e.Type, e.Instance, e.Instance));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException) { Engine.Log(e); }
        }
        return list;
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

    public static IEnumerable<uint> IdsIn(byte[] d)
    {
        for (var i = 0; i + 52 <= d.Length; i++)
        {
            var s = d.AsSpan(i);
            if (!s.Slice(4, 16).SequenceEqual(s.Slice(36, 16)) || !s.Slice(20, 4).SequenceEqual(s.Slice(28, 4))
                || s.Slice(24, 4).IndexOfAnyExcept((byte)0) >= 0 || s.Slice(32, 4).IndexOfAnyExcept((byte)0) >= 0 || s.Slice(4, 16).IndexOfAnyExcept((byte)0) < 0) continue;
            yield return System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(s.Slice(12, 4));
            i += 51;
        }
    }

    // an item's thumbnail from the same file: an object's THUM by its instance or ICON by its picture, a part's own
    static readonly Dictionary<string, List<Dbpf.Entry>> Indexes = new(StringComparer.OrdinalIgnoreCase);
    public static byte[]? Thumbnail(InstalledItem item)
    {
        List<Dbpf.Entry> index;
        lock (Indexes) if (!Indexes.TryGetValue(item.File, out index!)) Indexes[item.File] = index = Dbpf.Index(item.File);
        var tries = item.Type == ModKinds.Casp
            ? new[] { (CasThumbTypes, item.Instance) }
            : new[] { (ObjectThumbTypes, item.Instance), (ModKinds.IconTypes, item.PngInstance) };
        foreach (var (types, inst) in tries)
            foreach (var t in types)
                if (inst != 0 && index.FirstOrDefault(e => e.Type == t && e.Instance == inst) is { Size: > 0 } e) return Dbpf.Read(item.File, e);
        return null;
    }
}
