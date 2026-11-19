namespace ApexSetup;

// The Sims 3's package format (DBPF 2.0: .package, a save's .nhd files): only the index is read to list a file's
// resources, and one resource on demand (a household photo, a thumbnail). Reads are bounded; a damaged file gives an
// empty index, never an exception the UI has to handle.
public static class Dbpf
{
    public readonly record struct Entry(uint Type, uint Group, ulong Instance, uint Offset, uint Size, uint MemSize, bool Compressed);

    const int MaxEntries = 200_000; // (no real package comes close; a bigger count is a damaged header)

    public static List<Entry> Index(string path)
    {
        try
        {
            using var f = File.OpenRead(path);
            return Index(f);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or EndOfStreamException) { return []; }
    }

    public static List<Entry> Index(Stream f)
    {
        var list = new List<Entry>();
        if (f.Length < 96) return list;
        var r = new BinaryReader(f);
        if (r.ReadUInt32() != 0x46504244 /* "DBPF" */ || r.ReadUInt32() != 2) return list;
        f.Position = 0x24;
        var count = r.ReadUInt32();
        f.Position = 0x40;
        var at = r.ReadUInt32();
        if (count == 0 || count > MaxEntries || at >= f.Length) return list;
        f.Position = at;
        var flags = r.ReadUInt32();
        uint cType = (flags & 1) != 0 ? r.ReadUInt32() : 0, cGroup = (flags & 2) != 0 ? r.ReadUInt32() : 0, cHigh = (flags & 4) != 0 ? r.ReadUInt32() : 0;
        for (uint i = 0; i < count; i++)
        {
            var type = (flags & 1) != 0 ? cType : r.ReadUInt32();
            var group = (flags & 2) != 0 ? cGroup : r.ReadUInt32();
            var high = (flags & 4) != 0 ? cHigh : r.ReadUInt32();
            var low = r.ReadUInt32();
            var offset = r.ReadUInt32();
            var size = r.ReadUInt32();
            var mem = r.ReadUInt32();
            var compressed = false;
            if ((size & 0x80000000) != 0)
            {
                compressed = r.ReadUInt16() == 0xFFFF;
                r.ReadUInt16(); // (committed)
            }
            list.Add(new Entry(type, group, (ulong)high << 32 | low, offset, size & 0x7FFFFFFF, mem, compressed));
        }
        return list;
    }

    // one resource's bytes, decompressed; null when unreadable or larger than max
    public static byte[]? Read(string path, Entry e, int max = 16 << 20)
    {
        try
        {
            using var f = File.OpenRead(path);
            return Read(f, e, max);
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException) { return null; }
    }

    // the same from an open package: many resources of one file without reopening it
    public static byte[]? Read(Stream f, Entry e, int max = 16 << 20)
    {
        if (e.Size > max || e.MemSize > max) return null;
        try
        {
            if (e.Offset + (long)e.Size > f.Length) return null;
            f.Position = e.Offset;
            var data = new byte[e.Size];
            f.ReadExactly(data);
            return e.Compressed ? RefPack.Decode(data, max) : data;
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException or EndOfStreamException or InvalidDataException) { return null; }
    }

    // the first resource of one of the types, in that order of preference
    public static byte[]? First(string path, IReadOnlyList<uint> types)
    {
        var index = Index(path);
        foreach (var t in types)
            if (index.FirstOrDefault(e => e.Type == t) is { Size: > 0 } e && Read(path, e) is { } bytes) return bytes;
        return null;
    }
}

// EA's RefPack (QFS) compression, as The Sims 3 stores compressed resources
public static class RefPack
{
    public static byte[] Decode(byte[] src, int max = 16 << 20)
    {
        if (src.Length < 5 || src[1] != 0xFB) throw new InvalidDataException("Not RefPack");
        var large = (src[0] & 0x80) != 0;
        int pos = 2, size = large ? src[2] << 24 | src[3] << 16 | src[4] << 8 | src[5] : src[2] << 16 | src[3] << 8 | src[4];
        pos += large ? 4 : 3;
        if (size < 0 || size > max) throw new InvalidDataException("RefPack size");
        var dst = new byte[size];
        int o = 0;
        while (pos < src.Length)
        {
            int b0 = src[pos++], plain, copy = 0, offset = 0;
            if (b0 <= 0x7F)
            {
                int b1 = Next();
                plain = b0 & 3; offset = ((b0 & 0x60) << 3) + b1 + 1; copy = ((b0 & 0x1C) >> 2) + 3;
            }
            else if (b0 <= 0xBF)
            {
                int b1 = Next(), b2 = Next();
                plain = b1 >> 6 & 3; offset = ((b1 & 0x3F) << 8) + b2 + 1; copy = (b0 & 0x3F) + 4;
            }
            else if (b0 <= 0xDF)
            {
                int b1 = Next(), b2 = Next(), b3 = Next();
                plain = b0 & 3; offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1; copy = ((b0 & 0x0C) << 6) + b3 + 5;
            }
            else if (b0 <= 0xFB) plain = ((b0 & 0x1F) << 2) + 4;
            else plain = b0 & 3; // (end of stream: its last literals)
            if (o + plain > size || pos + plain > src.Length) throw new InvalidDataException("RefPack literals");
            Buffer.BlockCopy(src, pos, dst, o, plain);
            pos += plain; o += plain;
            if (copy > 0)
            {
                if (offset > o || o + copy > size) throw new InvalidDataException("RefPack copy");
                for (int i = 0; i < copy; i++, o++) dst[o] = dst[o - offset]; // (overlapping copies repeat bytes: one by one)
            }
            if (b0 >= 0xFC) break;
        }
        return dst;

        int Next() => pos < src.Length ? src[pos++] : throw new InvalidDataException("RefPack truncated");
    }
}
