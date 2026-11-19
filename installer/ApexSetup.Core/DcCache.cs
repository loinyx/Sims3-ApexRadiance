using System.Buffers.Binary;
using System.Text;

namespace ApexSetup;

// The game's own content cache (Documents\...\DCCache), written the way the game's installer does it, so the launcher
// can remove store content, lots and worlds without starting the game.
//
// Each dcdbN.dbc (and missingdeps.idx) holds one record list, resource 0x7672F0C5, big-endian, as the game's writer
// writes it (TS3W 1.67 at 0x00984c20; every list of a player's 26 files, 11,856 entries, reads and writes back
// byte-identical):
//   u32 count, then per entry: u32 10, u8 flag, u32 N, u32 K, id[16], N x dependency[16],
//   u32 content type, u32 subtype, u32 spare, u16 asset version, K x (key[16], key[16]),
//   name and description (u32 length in UTF-16 units, UTF-16BE), thumbnail key[16],
//   u8 has handler, [u32 length + handler XML], u32 spare
// Removing an item, as the game does it (checked: removing a lot through the game gave exactly this): its entry leaves
// the list, a cleared copy (same id and keys, its pairs in reverse order, the rest empty) is added to missingdeps.idx,
// and a lot's copy in the Library goes. Its resources stay in the file, unlisted, until the game compacts it.
public static class DcCache
{
    public const uint ListType = 0x7672F0C5;
    public const string Missing = "missingdeps.idx";

    public sealed class Record
    {
        public byte[] Id = new byte[16];        // as stored: the whole id's two 8-byte halves swapped
        public byte Flag;
        public List<byte[]> Deps = [];
        public uint ContentType, SubType, Spare;
        public ushort AssetVersion;
        public byte[] Pairs = [];               // K x 32 bytes
        public string Name = "", Desc = "";
        public byte[] Thumb = new byte[16];
        public byte[]? Handler;                 // null: none
        public uint Last;
        public int K => Pairs.Length / 32;
        // the id as the game and Sims3Packs name it: "0x" and 32 hex digits
        public string FullId => "0x" + Convert.ToHexString(Id, 8, 8).ToLowerInvariant() + Convert.ToHexString(Id, 0, 8).ToLowerInvariant();
    }

    public static byte[] StoredId(string fullId)
    {
        var hex = fullId.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? fullId[2..] : fullId;
        if (hex.Length != 32) throw new ArgumentException("Not a 32-digit id: " + fullId);
        var b = Convert.FromHexString(hex);
        return [.. b.AsSpan(8, 8), .. b.AsSpan(0, 8)];
    }

    public static List<Record> Parse(byte[] d)
    {
        var p = 0;
        uint U32() { var v = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(p, 4)); p += 4; return v; }
        byte[] Take(int n) { var v = d.AsSpan(p, n).ToArray(); p += n; return v; }
        string Str() { var n = checked((int)U32() * 2); var s = Encoding.BigEndianUnicode.GetString(d, p, n); p += n; return s; }
        var count = U32();
        if (count > 1_000_000) throw new InvalidDataException("Record list count " + count);
        var list = new List<Record>((int)count);
        for (var i = 0; i < count; i++)
        {
            if (U32() != 10) throw new InvalidDataException("Unknown record version at " + (p - 4));
            var r = new Record { Flag = d[p++] };
            var n = (int)U32();
            var k = checked((int)U32());
            r.Id = Take(16);
            for (var j = 0; j < n; j++) r.Deps.Add(Take(16));
            r.ContentType = U32(); r.SubType = U32(); r.Spare = U32();
            r.AssetVersion = BinaryPrimitives.ReadUInt16BigEndian(d.AsSpan(p, 2)); p += 2;
            r.Pairs = Take(checked(k * 32));
            r.Name = Str(); r.Desc = Str();
            r.Thumb = Take(16);
            if (d[p++] != 0) r.Handler = Take((int)U32());
            r.Last = U32();
            list.Add(r);
        }
        if (p != d.Length) throw new InvalidDataException($"Record list has {d.Length - p} bytes past its last entry");
        return list;
    }

    public static byte[] Build(IReadOnlyList<Record> list)
    {
        var m = new MemoryStream();
        void U32(uint v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(b, v); m.Write(b); }
        void Str(string s) { U32((uint)s.Length); m.Write(Encoding.BigEndianUnicode.GetBytes(s)); }
        U32((uint)list.Count);
        foreach (var r in list)
        {
            U32(10); m.WriteByte(r.Flag); U32((uint)r.Deps.Count); U32((uint)r.K);
            m.Write(r.Id);
            foreach (var dep in r.Deps) m.Write(dep);
            U32(r.ContentType); U32(r.SubType); U32(r.Spare);
            Span<byte> av = stackalloc byte[2]; BinaryPrimitives.WriteUInt16BigEndian(av, r.AssetVersion); m.Write(av);
            m.Write(r.Pairs);
            Str(r.Name); Str(r.Desc);
            m.Write(r.Thumb);
            m.WriteByte(r.Handler is null ? (byte)0 : (byte)1);
            if (r.Handler is not null) { U32((uint)r.Handler.Length); m.Write(r.Handler); }
            U32(r.Last);
        }
        return m.ToArray();
    }

    // what the game adds to missingdeps.idx for a record it removed
    public static Record Cleared(Record r)
    {
        var pairs = new byte[r.Pairs.Length];
        for (var i = 0; i < r.K; i++) Array.Copy(r.Pairs, (r.K - 1 - i) * 32, pairs, i * 32, 32);
        return new Record { Id = r.Id, Flag = 1, Pairs = pairs, Thumb = new byte[16] };
    }

    // ---------- the package around the list ----------

    // the record list of a cache file; null when it has none
    public static List<Record>? ReadList(string path)
    {
        var e = Dbpf.Index(path).FirstOrDefault(x => x.Type == ListType);
        if (e.Type != ListType) return null;
        return Dbpf.Read(path, e, 64 << 20) is { } b ? Parse(b) : throw new InvalidDataException("Unreadable record list in " + Path.GetFileName(path));
    }

    // Replaces the record list of a cache file in place: the new list (stored uncompressed, as the game stores some of
    // its own resources) and a new index go at the end, every other index entry copied as it was, then the header points
    // at them. The old list and index stay behind as unused bytes, as in the game's own files.
    public static void WriteList(string path, byte[] list)
    {
        using var f = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var head = new byte[96];
        f.ReadExactly(head);
        uint H(int at) => BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(at, 4));
        if (H(0) != 0x46504244 || H(4) != 2) throw new InvalidDataException(Path.GetFileName(path) + " is not a DBPF 2 package");
        var count = H(0x24); var size = H(0x2C); var at = H(0x40);
        f.Position = at;
        var ix = new byte[size];
        f.ReadExactly(ix);
        uint I(int p) => BinaryPrimitives.ReadUInt32LittleEndian(ix.AsSpan(p, 4));
        var flags = I(0);
        var p = 4;
        uint cType = 0, cGroup = 0, cHigh = 0;
        if ((flags & 1) != 0) { cType = I(p); p += 4; }
        if ((flags & 2) != 0) { cGroup = I(p); p += 4; }
        if ((flags & 4) != 0) { cHigh = I(p); p += 4; }
        var newIx = new MemoryStream();
        newIx.Write(ix, 0, 4 + 4 * System.Numerics.BitOperations.PopCount(flags & 7) ); // (flags and the shared fields as they were)
        f.Seek(0, SeekOrigin.End);
        var dataAt = (uint)f.Position;
        var found = 0;
        for (var i = 0; i < count; i++)
        {
            var s = p;
            var type = (flags & 1) != 0 ? cType : I(p);
            if ((flags & 1) == 0) p += 4;
            if ((flags & 2) == 0) p += 4;
            if ((flags & 4) == 0) p += 4;
            p += 4; // (instance low)
            var offAt = p; p += 4;
            var sizeAt = p; var raw = I(p); p += 4;
            var memAt = p; p += 4;
            if ((raw & 0x80000000) != 0) p += 4;
            var entry = ix.AsSpan(s, p - s).ToArray();
            if (type == ListType)
            {
                if ((raw & 0x80000000) == 0) throw new InvalidDataException("Record list entry without its compression fields");
                found++;
                BinaryPrimitives.WriteUInt32LittleEndian(entry.AsSpan(offAt - s), dataAt);
                BinaryPrimitives.WriteUInt32LittleEndian(entry.AsSpan(sizeAt - s), (uint)list.Length | 0x80000000);
                BinaryPrimitives.WriteUInt32LittleEndian(entry.AsSpan(memAt - s), (uint)list.Length);
                BinaryPrimitives.WriteUInt16LittleEndian(entry.AsSpan(p - s - 4), 0);   // stored as is
                BinaryPrimitives.WriteUInt16LittleEndian(entry.AsSpan(p - s - 2), 1);   // committed
            }
            newIx.Write(entry);
        }
        if (found != 1) throw new InvalidDataException($"{Path.GetFileName(path)} holds {found} record lists");
        if (p != ix.Length) throw new InvalidDataException("Index of " + Path.GetFileName(path) + " was not read to its end");
        f.Write(list);
        var newAt = (uint)f.Position;
        var newIndex = newIx.ToArray();
        f.Write(newIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x2C), (uint)newIndex.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x40), newAt);
        f.Position = 0;
        f.Write(head);
        f.Flush(true);
    }

    // a fresh missingdeps.idx: one record list, uncompressed, laid out like the game's (shared type, group and high
    // instance in the index)
    public static byte[] MissingFile(byte[] list)
    {
        var m = new MemoryStream();
        var w = new BinaryWriter(m);
        var head = new byte[96];
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0), 0x46504244);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(4), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x24), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x2C), 36);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x3C), 3);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(0x40), (uint)(96 + list.Length));
        w.Write(head);
        w.Write(list);
        w.Write(7u); w.Write(ListType); w.Write(0u); w.Write(0u); // flags, type, group, instance high
        w.Write(0u); w.Write(96u); w.Write((uint)list.Length | 0x80000000); w.Write((uint)list.Length);
        w.Write((ushort)0); w.Write((ushort)1);
        return m.ToArray();
    }

    // ---------- removing ----------

    public sealed record RemoveResult(List<string> Removed, List<string> NotFound, string BackupDir);

    // Removes items by their whole id from every cache file that lists them. Every file it changes is copied to
    // backupDir first (with the Library copies it takes away), so the change can be put back.
    public static RemoveResult Remove(string docs, IReadOnlyCollection<string> fullIds, string backupDir)
    {
        if (Games.IsRunning()) throw new SetupException("Close The Sims 3 first.");
        var dir = Path.Combine(docs, InstalledContent.Folder);
        var wanted = fullIds.ToDictionary(id => Convert.ToHexString(StoredId(id)), id => id, StringComparer.OrdinalIgnoreCase);
        var removed = new List<Record>();
        var plan = new List<(string File, List<Record> Keep)>();
        foreach (var f in InstalledContent.Files(docs))
        {
            var list = ReadList(f);
            if (list is null) continue;
            var keep = list.Where(r => !wanted.ContainsKey(Convert.ToHexString(r.Id))).ToList();
            if (keep.Count == list.Count) continue;
            removed.AddRange(list.Where(r => wanted.ContainsKey(Convert.ToHexString(r.Id))));
            plan.Add((f, keep));
        }
        var removedIds = removed.Select(r => r.FullId).Distinct().ToList();
        var notFound = fullIds.Where(id => !removedIds.Contains(id.ToLowerInvariant())).ToList();
        if (plan.Count == 0) return new([], notFound, backupDir);

        var missingPath = Path.Combine(dir, Missing);
        var missing = File.Exists(missingPath) ? ReadList(missingPath) ?? [] : [];
        var lots = LibraryCopies(docs, removedIds);

        // the backup first: every file this changes
        Directory.CreateDirectory(Path.Combine(backupDir, "DCCache"));
        Directory.CreateDirectory(Path.Combine(backupDir, "Library"));
        foreach (var (f, _) in plan) File.Copy(f, Path.Combine(backupDir, "DCCache", Path.GetFileName(f)), true);
        if (File.Exists(missingPath)) File.Copy(missingPath, Path.Combine(backupDir, "DCCache", Missing), true);
        File.WriteAllLines(Path.Combine(backupDir, "removed.txt"), removed.Select(r => r.FullId + "\t" + r.Name));

        foreach (var (f, keep) in plan) WriteList(f, Build(keep));
        File.WriteAllBytes(missingPath + ".apex-new", MissingFile(Build([.. missing, .. removed.Select(Cleared)])));
        File.Move(missingPath + ".apex-new", missingPath, true);
        foreach (var lot in lots) File.Move(lot, Path.Combine(backupDir, "Library", Path.GetFileName(lot)), true);
        return new(removedIds, notFound, backupDir);
    }

    // a lot's or household's copy in the Library: the ebf_*.package whose Sims3Pack manifest has its id
    public static List<string> LibraryCopies(string docs, IReadOnlyCollection<string> fullIds)
    {
        var lib = Path.Combine(docs, "Library");
        var found = new List<string>();
        if (!Directory.Exists(lib)) return found;
        foreach (var f in Directory.EnumerateFiles(lib, "*.package"))
        {
            var e = Dbpf.Index(f).FirstOrDefault(x => x.Type == InstalledContent.ManifestType);
            if (e.Type != InstalledContent.ManifestType || Dbpf.Read(f, e) is not { } b) continue;
            var x = Encoding.UTF8.GetString(b);
            var i = x.IndexOf("<packageid>", StringComparison.OrdinalIgnoreCase);
            if (i < 0) continue;
            var id = x.Substring(i + 11, Math.Min(34, x.Length - i - 11)).ToLowerInvariant();
            if (fullIds.Any(w => w.Equals(id, StringComparison.OrdinalIgnoreCase))) found.Add(f);
        }
        return found;
    }

    // puts back what Remove changed, from its backup folder
    public static void Restore(string docs, string backupDir)
    {
        if (Games.IsRunning()) throw new SetupException("Close The Sims 3 first.");
        foreach (var f in Directory.EnumerateFiles(Path.Combine(backupDir, "DCCache")))
            File.Copy(f, Path.Combine(docs, InstalledContent.Folder, Path.GetFileName(f)), true);
        var lib = Path.Combine(backupDir, "Library");
        if (Directory.Exists(lib))
            foreach (var f in Directory.EnumerateFiles(lib)) File.Copy(f, Path.Combine(docs, "Library", Path.GetFileName(f)), true);
    }
}
