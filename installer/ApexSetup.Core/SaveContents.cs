using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace ApexSetup;

// What a resource of a save is. Game is everything the game needs to load the save; the others are pictures.
// LotTexture is never removed: without it placed lots turn to bare ground.
public enum SaveKind { Game, Memory, Portrait, LotThumbnail, HouseholdPicture, Generated, LotTexture }

// one resource of a save file, as the file's index names it (Index: its place there)
public readonly record struct SaveItem(string File, int Index, uint Type, uint Group, ulong Instance, uint Offset, uint Size, uint MemSize, bool Compressed)
{
    public string Key => $"{Type:x8}:{Group:x8}:{Instance:x16}";
    public SaveKind Kind => SaveContents.KindOf(Type, Group);
}

// one file of a save: how much each kind takes in it (stored bytes) and the room nothing uses (DeadBytes)
public sealed record SaveFile(string Path, long Bytes, int Resources, long DeadBytes, bool Readable, IReadOnlyDictionary<SaveKind, (int Count, long Bytes)> Kinds);

// A package of a save read strictly, every index field kept as stored: what a clean rewrites from. A file that does not
// parse throws InvalidDataException; one that parses but holds something not understood is Unknown and never rewritten.
internal sealed class SavePackage
{
    public const int MaxEntries = 200_000; // (real saves stay under 15,000 a file; a bigger count is a damaged header)
    public string Path = "";
    public long Length;
    public DateTime Written;
    public byte[] Head = [];
    public uint Flags;
    public uint[] Shared = new uint[3]; // the type, group and high instance every entry shares, when Flags says so
    public uint[] Raw = [];             // 8 words an entry: type, group, instance high and low, offset, size, unpacked size, compression
    public bool Unknown;
    public int Count => Raw.Length / 8;
    public uint Offset(int i) => Raw[i * 8 + 4];
    public uint Size(int i) => Raw[i * 8 + 5] & 0x7FFFFFFF;

    int SharedCount => System.Numerics.BitOperations.PopCount(Flags);
    public int IndexBytes(int count) => 4 + 4 * SharedCount + count * (32 - 4 * SharedCount);

    // the file's exact size once only these entries are written: header, each stored block once, index
    public long SizeWith(Func<int, bool> keep)
    {
        var seen = new HashSet<(uint, uint)>();
        long data = 0;
        var n = 0;
        for (var i = 0; i < Count; i++)
        {
            if (!keep(i)) continue;
            n++;
            if (seen.Add((Offset(i), Size(i)))) data += Size(i);
        }
        return 96 + data + IndexBytes(n);
    }

    public SaveItem Item(int i) => new(Path, i, Raw[i * 8], Raw[i * 8 + 1], (ulong)Raw[i * 8 + 2] << 32 | Raw[i * 8 + 3], Offset(i), Size(i), Raw[i * 8 + 6], (Raw[i * 8 + 7] & 0xFFFF) == 0xFFFF);

    public static SavePackage Open(string path)
    {
        using var f = Dbpf.OpenShared(path);
        var p = new SavePackage { Path = path, Length = f.Length, Written = File.GetLastWriteTimeUtc(path), Head = new byte[96] };
        if (p.Length < 96) throw new InvalidDataException("too short");
        f.ReadExactly(p.Head);
        uint U(int at) => BinaryPrimitives.ReadUInt32LittleEndian(p.Head.AsSpan(at));
        if (U(0) != 0x46504244 || U(4) != 2) throw new InvalidDataException("not DBPF 2");
        uint count = U(36), size = U(44), at = U(64);
        if (count > MaxEntries) throw new InvalidDataException("index count too large");
        if (at < 96 || (long)at + size > p.Length || size < 4) throw new InvalidDataException("index outside the file");
        // (the game leaves these header fields zero; one that is set could point into the file, and a rewrite moves things)
        p.Unknown = U(40) != 0 || p.Head.AsSpan(48, 12).ContainsAnyExcept((byte)0);
        var index = new byte[Math.Min(size, 16 + 32L * count)];
        f.Position = at;
        f.ReadExactly(index);
        p.Flags = BinaryPrimitives.ReadUInt32LittleEndian(index);
        if (p.Flags > 7) throw new InvalidDataException("index flags");
        if (p.IndexBytes((int)count) > size) throw new InvalidDataException("index too small");
        p.Unknown |= p.IndexBytes((int)count) != size;
        var at2 = 4;
        uint Next() { var v = BinaryPrimitives.ReadUInt32LittleEndian(index.AsSpan(at2)); at2 += 4; return v; }
        for (var b = 0; b < 3; b++) if ((p.Flags >> b & 1) != 0) p.Shared[b] = Next();
        p.Raw = new uint[count * 8];
        for (var i = 0; i < count; i++)
        {
            for (var b = 0; b < 3; b++) p.Raw[i * 8 + b] = (p.Flags >> b & 1) != 0 ? p.Shared[b] : Next();
            for (var w = 3; w < 8; w++) p.Raw[i * 8 + w] = Next();
            long from = p.Offset(i), to = from + p.Size(i);
            if (from < 96 || to > p.Length || (from < (long)at + size && to > at)) throw new InvalidDataException("a resource lies outside the file's data");
        }
        return p;
    }
}

// What a save holds, from its files' indexes alone: bytes and counts per kind and per file, the pictures listed, and the
// room nothing uses. Pictures are read and decoded one at a time, on demand, off the UI thread. A broken file is listed
// as not readable; nothing here throws for one.
public sealed class SaveContents
{
    const uint Image = 0x00B2D882;
    public const uint MemoryGroup = 0x0269D005;
    static readonly uint[] GeneratedGroups = [0x024B9FCA, 0x02722299, 0x02BD69A0, 0x0E9928A8];
    static readonly uint[] TextureGroups = [0x00B0C507, 1, 0x00BC1E4C, 0x001DA7E9, 0x00FAE172, 0x00BC1E54, 0x007E7555];

    // (an image in a group nobody knows counts as the game's: kept)
    public static SaveKind KindOf(uint type, uint group) => type switch
    {
        0x6B6D837D or 0x6B6D837E or 0x6B6D837F => SaveKind.HouseholdPicture,
        0x0580A2CD or 0x0580A2CE or 0x0580A2CF => SaveKind.Portrait,
        0xD84E7FC6 => SaveKind.LotThumbnail,
        Image when group == MemoryGroup => SaveKind.Memory,
        Image when GeneratedGroups.Contains(group) => SaveKind.Generated,
        Image when TextureGroups.Contains(group) => SaveKind.LotTexture,
        _ => SaveKind.Game,
    };

    public string Dir { get; }
    public IReadOnlyList<SaveFile> Files { get; }
    public long Bytes { get; }           // every file of the save
    public long DeadBytes { get; }       // in the files a clean rewrites (world files and TravelDB.package)
    public ulong OwnPicture { get; }     // the instance of this save's household picture (Meta.data); 0: not known
    internal List<SavePackage> Packages { get; } = [];
    readonly List<SaveItem> _pictures = [];
    List<IReadOnlyList<SaveItem>>? _groups;

    // the files a clean may rewrite. ExportDB packages hold only the game's own data and are left alone.
    public static bool Cleanable(string file) =>
        file.EndsWith(".nhd", StringComparison.OrdinalIgnoreCase) || Path.GetFileName(file).Equals("TravelDB.package", StringComparison.OrdinalIgnoreCase);

    SaveContents(string dir)
    {
        Dir = dir;
        OwnPicture = SaveInfo.Read(dir).Picture;
        var files = new List<SaveFile>();
        foreach (var f in SaveInfo.FilesOf(dir))
        {
            Bytes += f.Length;
            if (!f.Extension.Equals(".nhd", StringComparison.OrdinalIgnoreCase) && !f.Extension.Equals(".package", StringComparison.OrdinalIgnoreCase)) continue;
            var kinds = new Dictionary<SaveKind, (int Count, long Bytes)>();
            try
            {
                var p = SavePackage.Open(f.FullName);
                for (var i = 0; i < p.Count; i++)
                {
                    var item = p.Item(i);
                    var k = item.Kind;
                    var (n, b) = kinds.GetValueOrDefault(k);
                    kinds[k] = (n + 1, b + item.Size);
                    if (k != SaveKind.Game) _pictures.Add(item);
                }
                var dead = Math.Max(0, p.Length - p.SizeWith(_ => true));
                if (Cleanable(f.Name)) { Packages.Add(p); if (!p.Unknown) DeadBytes += dead; }
                files.Add(new(f.FullName, f.Length, p.Count, dead, true, kinds));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                if (Cleanable(f.Name)) Packages.Add(new SavePackage { Path = f.FullName, Length = f.Length, Unknown = true });
                files.Add(new(f.FullName, f.Length, 0, 0, false, kinds));
            }
        }
        Files = files;
    }

    public static SaveContents Read(string dir) => new(dir);

    // how many resources of a kind, and their stored bytes. Game counts the game's own resources only: what a screen
    // shows as "the save itself" is Bytes less the kinds it shows apart.
    public (int Count, long Bytes) Of(SaveKind kind) =>
        Files.Aggregate((Count: 0, Bytes: 0L), (a, f) => f.Kinds.TryGetValue(kind, out var k) ? (a.Count + k.Count, a.Bytes + k.Bytes) : a);

    // the pictures of a kind, in file order (nothing is read)
    public IReadOnlyList<SaveItem> Items(SaveKind kind) => _pictures.Where(i => i.Kind == kind).ToList();

    // up to count of them, spread over the whole list; of portraits and household pictures, the largest size of each
    public IReadOnlyList<SaveItem> Sample(SaveKind kind, int count)
    {
        var all = Items(kind).GroupBy(i => (i.File, i.Instance)).Select(g => g.MaxBy(i => i.Type)).ToList();
        if (all.Count <= count || count <= 0) return all;
        return Enumerable.Range(0, count).Select(i => all[(int)((long)i * all.Count / count)]).ToList();
    }

    // The memories that are one picture: grouped by their unpacked content, each under its own key (the game stores the
    // same picture again for another Sim's memory). Reads every memory once; the answer is kept. One that can't be read
    // stands alone.
    public IReadOnlyList<IReadOnlyList<SaveItem>> MemoryGroups(CancellationToken ct = default)
    {
        if (_groups is { } done) return done;
        var groups = new Dictionary<string, List<SaveItem>>();
        foreach (var byFile in Items(SaveKind.Memory).GroupBy(i => i.File))
            try
            {
                using var f = Dbpf.OpenShared(byFile.Key);
                foreach (var i in byFile)
                {
                    ct.ThrowIfCancellationRequested();
                    var id = Dbpf.Read(f, Entry(i)) is { } b ? Convert.ToHexString(SHA256.HashData(b)) : i.File + i.Index;
                    (groups.TryGetValue(id, out var l) ? l : groups[id] = []).Add(i);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                foreach (var i in byFile) groups.TryAdd(i.File + i.Index, [i]);
            }
        return _groups = groups.Values.Select(g => (IReadOnlyList<SaveItem>)g).ToList();
    }

    static Dbpf.Entry Entry(SaveItem i) => new(i.Type, i.Group, i.Instance, i.Offset, i.Size, i.MemSize, i.Compressed);

    // a resource's bytes, unpacked; null when it can't be read
    public static byte[]? Data(SaveItem i) => Dbpf.Read(i.File, Entry(i));

    // a picture's pixels: a memory or generated image decoded (the smallest stored size not under maxSide; 0: full size)
    public static SavePicture? Picture(SaveItem i, int maxSide = 0) => Data(i) is { } b ? Dds.Decode(b, maxSide) : null;

    // a picture as PNG bytes: portraits, thumbnails and household pictures as the game stored them, the others decoded
    public static byte[]? Png(SaveItem i, int maxSide = 0)
    {
        if (Data(i) is not { Length: > 8 } b) return null;
        return b.AsSpan(1, 3).SequenceEqual("PNG"u8) ? b : Dds.Decode(b, maxSide)?.Png();
    }

    // Each picture as a PNG file in a folder, named after the save and the picture's own key, so one already there is
    // left as it is (never written over, never twice). The files' paths, those already there too.
    public static List<string> Export(string save, IEnumerable<SaveItem> items, string folder, CancellationToken ct = default)
    {
        Directory.CreateDirectory(folder);
        var paths = new List<string>();
        foreach (var i in items)
        {
            ct.ThrowIfCancellationRequested();
            var path = Path.Combine(folder, $"{save} {i.Key.Replace(':', '-')}.png");
            if (!File.Exists(path))
            {
                if (Png(i) is not { } png) continue;
                var part = path + ".part";
                try { File.WriteAllBytes(part, png); File.Move(part, path); }
                catch { try { File.Delete(part); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
            }
            paths.Add(path);
        }
        return paths;
    }

    // the same into the folder the Gallery shows new pictures from, each one set as this save's household
    public static List<string> ToGallery(string docs, Save s, IEnumerable<SaveItem> items, CancellationToken ct = default)
    {
        var paths = Export(s.Name, items, Gallery.Folder(docs), ct);
        var d = Gallery.Load();
        foreach (var p in paths) Gallery.Info(d, p).Household = s.Name;
        Gallery.Save(d);
        return paths;
    }
}

// pixels, top row first, 4 bytes each: blue, green, red, alpha
public sealed record SavePicture(int Width, int Height, byte[] Bgra)
{
    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n => { var c = (uint)n; for (var k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ c >> 1 : c >> 1; return c; }).ToArray();

    public byte[] Png()
    {
        // (each row as the difference from the pixel to its left: photos pack far better that way)
        var rows = new byte[Height * (1 + Width * 4)];
        for (int y = 0, o = 0, i = 0; y < Height; y++)
        {
            rows[o++] = 1;
            for (var x = 0; x < Width; x++, i += 4, o += 4)
            {
                rows[o] = (byte)(Bgra[i + 2] - (x > 0 ? Bgra[i - 2] : 0));
                rows[o + 1] = (byte)(Bgra[i + 1] - (x > 0 ? Bgra[i - 3] : 0));
                rows[o + 2] = (byte)(Bgra[i] - (x > 0 ? Bgra[i - 4] : 0));
                rows[o + 3] = (byte)(Bgra[i + 3] - (x > 0 ? Bgra[i - 1] : 0));
            }
        }
        var packed = new MemoryStream();
        using (var z = new ZLibStream(packed, CompressionLevel.Fastest, leaveOpen: true)) z.Write(rows);
        var png = new MemoryStream();
        png.Write([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
        var head = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(head, Width);
        BinaryPrimitives.WriteInt32BigEndian(head.AsSpan(4), Height);
        head[8] = 8; head[9] = 6;
        Chunk("IHDR"u8, head);
        Chunk("IDAT"u8, packed.GetBuffer().AsSpan(0, (int)packed.Length));
        Chunk("IEND"u8, []);
        return png.ToArray();

        void Chunk(ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
        {
            Span<byte> n = stackalloc byte[4];
            BinaryPrimitives.WriteInt32BigEndian(n, data.Length);
            png.Write(n);
            png.Write(type);
            png.Write(data);
            var crc = 0xFFFFFFFF;
            foreach (var b in type) crc = CrcTable[(crc ^ b) & 0xFF] ^ crc >> 8;
            foreach (var b in data) crc = CrcTable[(crc ^ b) & 0xFF] ^ crc >> 8;
            BinaryPrimitives.WriteUInt32BigEndian(n, ~crc);
            png.Write(n);
        }
    }
}

// The DDS pictures a save holds: DXT1 (memories), DXT3, DXT5 and plain 32-bit ones (generated images). Untrusted input:
// sizes are bounded and every read stays inside the file; anything else gives null.
public static class Dds
{
    const int MaxSide = 4096;

    public static SavePicture? Decode(byte[] d, int maxSide = 0)
    {
        if (d.Length < 128 || !d.AsSpan(0, 4).SequenceEqual("DDS "u8)) return null;
        int I(int at) => BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(at));
        int h = I(12), w = I(16), mips = Math.Clamp(I(28), 1, 16);
        if (w < 1 || h < 1 || w > MaxSide || h > MaxSide) return null;
        var fourCc = System.Text.Encoding.ASCII.GetString(d, 84, 4);
        var block = fourCc switch { "DXT1" => 8, "DXT3" or "DXT5" => 16, _ => 0 };
        if (block == 0 && ((I(80) & 0x40) == 0 || I(88) != 32)) return null;
        long SizeOf(int lw, int lh) => block > 0 ? (long)((lw + 3) / 4) * ((lh + 3) / 4) * block : (long)lw * lh * 4;
        // the smallest stored size that is still at least maxSide, when the file really holds it
        long at = 128;
        for (var level = 0; level + 1 < mips && maxSide > 0; level++)
        {
            int nw = Math.Max(1, w / 2), nh = Math.Max(1, h / 2);
            var next = at + SizeOf(w, h);
            if (Math.Max(nw, nh) < maxSide || next + SizeOf(nw, nh) > d.Length) break;
            (at, w, h) = (next, nw, nh);
        }
        if (at + SizeOf(w, h) > d.Length) return null;
        var o = new byte[w * h * 4];
        var p = (int)at;
        if (block == 0)
        {
            uint[] masks = [(uint)I(100), (uint)I(96), (uint)I(92), (uint)I(104)]; // blue, green, red, alpha
            for (var i = 0; i < o.Length; i += 4)
            {
                var v = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(p + i));
                for (var c = 0; c < 4; c++)
                    o[i + c] = masks[c] == 0 ? (byte)(c == 3 ? 255 : 0) : (byte)((v & masks[c]) >> System.Numerics.BitOperations.TrailingZeroCount(masks[c]));
            }
            return new(w, h, o);
        }
        Span<byte> colors = stackalloc byte[16];
        Span<byte> alphas = stackalloc byte[8];
        for (var by = 0; by < h; by += 4)
            for (var bx = 0; bx < w; bx += 4, p += block)
            {
                var c = d.AsSpan(p + block - 8, 8);
                int c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
                Rgb(c0, colors); Rgb(c1, colors[4..]);
                var four = block == 16 || c0 > c1;
                for (var k = 0; k < 3; k++)
                {
                    colors[8 + k] = (byte)(four ? (2 * colors[k] + colors[4 + k] + 1) / 3 : (colors[k] + colors[4 + k]) / 2);
                    colors[12 + k] = (byte)(four ? (colors[k] + 2 * colors[4 + k] + 1) / 3 : 0);
                }
                colors[11] = 255; colors[15] = (byte)(four ? 255 : 0);
                ulong alphaBits = 0;
                if (fourCc == "DXT5")
                {
                    alphas[0] = d[p]; alphas[1] = d[p + 1];
                    for (var k = 1; k < 7; k++)
                        alphas[k + 1] = alphas[0] > alphas[1] ? (byte)(((7 - k) * alphas[0] + k * alphas[1]) / 7) : k < 5 ? (byte)(((5 - k) * alphas[0] + k * alphas[1]) / 5) : (byte)(k == 5 ? 0 : 255);
                    alphaBits = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(p)) >> 16;
                }
                else if (fourCc == "DXT3") alphaBits = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(p));
                var bits = BinaryPrimitives.ReadUInt32LittleEndian(c[4..]);
                for (var i = 0; i < 16; i++)
                {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    if (x >= w || y >= h) continue;
                    var to = o.AsSpan((y * w + x) * 4, 4);
                    colors.Slice((int)(bits >> i * 2 & 3) * 4, 4).CopyTo(to);
                    if (fourCc == "DXT5") to[3] = alphas[(int)(alphaBits >> i * 3 & 7)];
                    else if (fourCc == "DXT3") to[3] = (byte)((alphaBits >> i * 4 & 15) * 17);
                }
            }
        return new(w, h, o);

        // 5-6-5 bits to blue, green, red, alpha
        static void Rgb(int v, Span<byte> to)
        {
            int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
            to[0] = (byte)(b << 3 | b >> 2); to[1] = (byte)(g << 2 | g >> 4); to[2] = (byte)(r << 3 | r >> 2); to[3] = 255;
        }
    }
}

// The memories the player starred, by save name and resource key, in %LOCALAPPDATA%\Apex Launcher\save-stars.json. A clean
// never removes a starred memory, so a file that exists but can't be read throws: guessing "none" would unprotect them.
public static class SaveStars
{
    static string FilePath => Path.Combine(Paths.Data, "save-stars.json");
    static readonly object Gate = new();

    static Dictionary<string, List<string>> Load()
    {
        try { return File.Exists(FilePath) ? JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.DictionaryStringListString) ?? new() : new(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            Engine.Log(e);
            throw new SetupException("The launcher can't read which memories you starred, so it removes none. Try again.");
        }
    }

    static void Change(Action<Dictionary<string, List<string>>> edit)
    {
        lock (Gate)
        {
            var all = Load();
            edit(all);
            Directory.CreateDirectory(Paths.Data);
            Fs.Retry(() => Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(all, SetupJson.Default.DictionaryStringListString)));
        }
    }

    public static HashSet<string> Of(string save) => new(Load().GetValueOrDefault(save) ?? [], StringComparer.OrdinalIgnoreCase);

    public static void Set(string save, string key, bool on) => Change(all =>
    {
        var l = all.TryGetValue(save, out var have) ? have : all[save] = [];
        l.RemoveAll(k => k.Equals(key, StringComparison.OrdinalIgnoreCase));
        if (on) l.Add(key);
        if (l.Count == 0) all.Remove(save);
    });

    // a renamed save keeps its stars; a duplicate gets the same ones (keep: the first save still has them)
    public static void Renamed(string from, string to, bool keep = false) => Change(all =>
    {
        if (!all.TryGetValue(from, out var l)) return;
        all[to] = all.TryGetValue(to, out var have) ? have.Union(l, StringComparer.OrdinalIgnoreCase).ToList() : [.. l];
        if (!keep) all.Remove(from);
    });
}
