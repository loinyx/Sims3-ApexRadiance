// Offline checks of the Saves engine (SaveInfo, SaveContents, SaveClean, SaveCopies, SaveStars) on saves made here, in a
// temporary Documents folder. APEX_SAVES_REAL=<a COPY of a Saves folder> adds a run on real saves: each is copied to the
// temporary folder first and the given folder is only read.
using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using ApexSetup;

static class SavesTests
{
    const uint Img = 0x00B2D882, Mem = SaveContents.MemoryGroup, Family = 0x6B6D837E, FamilySmall = 0x6B6D837D, Face = 0x0580A2CE, Lot = 0xD84E7FC6, Game = 0x0A000001, MetaType = 0x628A788F;

    // a DBPF 2.0 package as the game writes a save's: the index in the order given, the data in the opposite order,
    // gap unused bytes before each resource
    static void Pkg(string path, int gap, params (uint T, uint G, ulong I, byte[] Data, bool Comp, int Mem)[] res)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var f = File.Create(path);
        var w = new BinaryWriter(f);
        w.Write(0x46504244u); w.Write(2u); w.Write(new byte[96 - 8]);
        var offs = new uint[res.Length];
        for (var i = res.Length - 1; i >= 0; i--) { w.Write(Enumerable.Repeat((byte)0xEE, gap).ToArray()); offs[i] = (uint)f.Position; w.Write(res[i].Data); }
        var index = (uint)f.Position;
        w.Write(0u);
        for (var i = 0; i < res.Length; i++)
        {
            w.Write(res[i].T); w.Write(res[i].G); w.Write((uint)(res[i].I >> 32)); w.Write((uint)res[i].I);
            w.Write(offs[i]); w.Write((uint)res[i].Data.Length | 0x80000000u); w.Write((uint)res[i].Mem);
            w.Write(res[i].Comp ? (ushort)0xFFFF : (ushort)0); w.Write((ushort)1);
        }
        var size = (uint)(f.Position - index);
        f.Position = 36; w.Write((uint)res.Length);
        f.Position = 44; w.Write(size);
        f.Position = 60; w.Write(3u); w.Write(index);
    }
    static (uint, uint, ulong, byte[], bool, int) Raw(uint t, uint g, ulong i, byte[] d) => (t, g, i, d, false, d.Length);
    static (uint, uint, ulong, byte[], bool, int) Packed(uint t, uint g, ulong i, byte[] d) => (t, g, i, Lit(d), true, d.Length);

    // RefPack with literals only: what the game's unpacker reads, whatever packed it
    static byte[] Lit(byte[] d)
    {
        var o = new List<byte> { 0x10, 0xFB, (byte)(d.Length >> 16), (byte)(d.Length >> 8), (byte)d.Length };
        var at = 0;
        for (; d.Length - at >= 4; ) { var n = Math.Min(112, (d.Length - at) / 4 * 4); o.Add((byte)(0xE0 + (n - 4) / 4)); o.AddRange(d.AsSpan(at, n).ToArray()); at += n; }
        o.Add((byte)(0xFC + d.Length - at));
        o.AddRange(d.AsSpan(at).ToArray());
        return o.ToArray();
    }

    // Meta.data: one resource, every key part shared (index flags 7), as the game writes it
    static void Meta(string path, string world, string household, string about, ulong picture)
    {
        var b = new MemoryStream();
        var w = new BinaryWriter(b);
        w.Write(3);
        foreach (var s in new[] { world, household, about }) { w.Write(s.Length); w.Write(Encoding.Unicode.GetBytes(s)); }
        w.Write(picture); w.Write(new byte[16]);
        var data = Lit(b.ToArray());
        using var f = File.Create(path);
        var o = new BinaryWriter(f);
        o.Write(0x46504244u); o.Write(2u); o.Write(new byte[96 - 8]);
        o.Write(data);
        var index = (uint)f.Position;
        o.Write(7u); o.Write(MetaType); o.Write(0u); o.Write(0u);
        o.Write(1u); o.Write(96u); o.Write((uint)data.Length | 0x80000000u); o.Write((uint)b.Length); o.Write((ushort)0xFFFF); o.Write((ushort)1);
        f.Position = 36; o.Write(1u);
        f.Position = 44; o.Write(36u);
        f.Position = 60; o.Write(3u); o.Write(index);
    }

    // a DXT1 picture of one colour a size, with its smaller sizes
    static byte[] Dxt1(int side, int mips, params ushort[] colours)
    {
        var d = new MemoryStream();
        var w = new BinaryWriter(d);
        w.Write(Encoding.ASCII.GetBytes("DDS ")); w.Write(124); w.Write(0x1007); w.Write(side); w.Write(side); w.Write(0); w.Write(0); w.Write(mips);
        w.Write(new byte[44]); w.Write(32); w.Write(4); w.Write(Encoding.ASCII.GetBytes("DXT1")); w.Write(new byte[40]);
        for (var m = 0; m < mips; m++, side = Math.Max(1, side / 2))
            for (var i = 0; i < ((side + 3) / 4) * ((side + 3) / 4); i++) { w.Write(colours[m]); w.Write(colours[m]); w.Write(0u); }
        return d.ToArray();
    }
    static byte[] Png(string what) => [0x89, .. Encoding.ASCII.GetBytes("PNG\r\n\x1a\n" + what)];

    static string Sha(string p) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p)));
    // every file under a folder: its hash and its time
    static Dictionary<string, string> Snap(string dir) => Directory.Exists(dir)
        ? Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Where(f => !f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) // (an antivirus's own, for a moment)
            .ToDictionary(f => Path.GetRelativePath(dir, f), f => Sha(f) + File.GetLastWriteTimeUtc(f).Ticks, StringComparer.OrdinalIgnoreCase)
        : new();
    static bool Same(Dictionary<string, string> a, Dictionary<string, string> b) => a.Count == b.Count && a.All(x => b.TryGetValue(x.Key, out var v) && v == x.Value);

    // every resource of a save's packages: "file|key" and the hash of its stored bytes with every index field but the offset
    static Dictionary<string, string> Resources(string dir, Func<SaveKind, bool>? kinds = null)
    {
        var o = new Dictionary<string, string>();
        foreach (var f in Directory.EnumerateFiles(dir).Where(f => f.EndsWith(".nhd") || f.EndsWith(".package") || f.EndsWith("Meta.data")))
        {
            using var s = File.OpenRead(f);
            var n = 0;
            foreach (var e in ModConflicts.Package.Open(f).Entries)
            {
                if (kinds is not null && !kinds(SaveContents.KindOf(e.Type, e.Group))) continue;
                var b = new byte[e.FileSize];
                s.Position = e.Offset;
                s.ReadExactly(b);
                o[$"{Path.GetFileName(f)}|{e.Key}|{n++}"] = Convert.ToHexString(SHA256.HashData(b)) + e.MemSize + ":" + e.Compressed;
            }
        }
        return o;
    }
    static bool Throws<T>(Action a) where T : Exception { try { a(); return false; } catch (T) { return true; } }

    static byte[] MemA = [], MemC = [];

    // a save with two worlds, two households' pictures, portraits, thumbnails, memories (one picture three times) and unused room
    static Save Make(string docs, string name)
    {
        var dir = Path.Combine(docs, "Saves", name + ".sims3");
        Directory.CreateDirectory(dir);
        Meta(Path.Combine(dir, "Meta.data"), "Town", "Davis", "Two adults.", 0x19);
        File.WriteAllBytes(Path.Combine(dir, "1a5tf1l3.dat"), Encoding.ASCII.GetBytes("Town_0x00000000\0\n"));
        MemA = Dxt1(8, 2, 0xF800, 0x07E0);
        MemC = Dxt1(8, 2, 0x001F, 0x001F);
        Pkg(Path.Combine(dir, "Town_0x00000000.nhd"), 7,
            Raw(Game, 0, 1, Encoding.ASCII.GetBytes("the town itself")), Packed(Game, 0, 2, Encoding.ASCII.GetBytes("its Sims and their lives, packed")),
            Raw(Family, 0, 0x0A, Png("the neighbours")), Raw(Family, 0, 0x19, Png("the Davis family")), Raw(FamilySmall, 0, 0x0A, Png("the neighbours, small")),
            Raw(Face, 0, 0x501, Png("face one")), Raw(Face, 0, 0x502, Png("face two")), Raw(0x0580A2CD, 0, 0x502, Png("face two, small")),
            Raw(Lot, 0, 0x601, Png("lot one")), Raw(Lot, 0, 0x602, Png("lot two")),
            Packed(Img, 1, 0x701, Dxt1(4, 1, 0xFFFF)), Packed(Img, 0x0E9928A8, 0x801, Dxt1(4, 1, 0x0000)), Raw(Img, 0x12345, 0x901, Dxt1(4, 1, 0x1111)));
        Pkg(Path.Combine(dir, "Uni_0x0e41c954.nhd"), 0, Raw(Game, 0, 3, Encoding.ASCII.GetBytes("the university")), Raw(Face, 0, 0x503, Png("face three")));
        Pkg(Path.Combine(dir, "Town_0x00000000ExportDB.package"), 5, Raw(Game, 0, 4, Encoding.ASCII.GetBytes("exported")), Raw(Game, 0, 5, Encoding.ASCII.GetBytes("more")));
        Pkg(Path.Combine(dir, "TravelDB.package"), 3,
            Raw(Game, 0, 6, Encoding.ASCII.GetBytes("travel data")),
            Packed(Img, Mem, 0xA1, MemA), Raw(Img, Mem, 0xA2, MemA), Packed(Img, Mem, 0xC1, MemC), Packed(Img, Mem, 0xA3, MemA));
        foreach (var f in Directory.EnumerateFiles(dir)) File.SetLastWriteTime(f, new DateTime(2024, 3, 9, 10, 0, 0));
        return Saves.List(docs).First(s => s.Name == name);
    }

    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "ApexSavesTest-" + Guid.NewGuid().ToString("N")[..8]);
        var oldLocal = Paths.LocalAppData;
        var oldRecycle = SaveCopies.Recycle;
        var recycled = Path.Combine(root, "Recycle");
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            Directory.CreateDirectory(recycled);
            SaveCopies.Recycle = p => { var to = Path.Combine(recycled, Guid.NewGuid().ToString("N")); if (Directory.Exists(p)) Directory.Move(p, to); else File.Move(p, to); };
            Pictures(check);
            var docs = Path.Combine(root, "Docs");
            var s = Make(docs, "Town");
            Info(check, docs, s);
            Contents(check, docs, s);
            Clean(check, docs, s);
            Copies(check, docs, s);
            Whole(check, docs, recycled);
            Namesakes(check, Path.Combine(root, "Docs2"), recycled);
            if (Environment.GetEnvironmentVariable("APEX_SAVES_REAL") is { Length: > 0 } real) Real(check, real, Path.Combine(root, "Real"), recycled);
        }
        catch (Exception e) { check(false, "saves: exception " + e); }
        finally
        {
            SaveCopies.Recycle = oldRecycle;
            SaveClean.FailPoint = SaveCopies.FailPoint = _ => { };
            Games.IsRunning = () => false;
            Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
            Paths.LocalAppData = oldLocal;
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    static void Pictures(Action<bool, string> check)
    {
        var red = Dds.Decode(Dxt1(8, 2, 0xF800, 0x07E0));
        var small = Dds.Decode(Dxt1(8, 2, 0xF800, 0x07E0), 4);
        check(red is { Width: 8, Height: 8 } && red.Bgra.AsSpan(0, 4).SequenceEqual(new byte[] { 0, 0, 255, 255 }) && red.Bgra.AsSpan(63 * 4, 4).SequenceEqual(new byte[] { 0, 0, 255, 255 })
              && small is { Width: 4, Height: 4 } && small.Bgra.AsSpan(0, 4).SequenceEqual(new byte[] { 0, 255, 0, 255 }) && Dds.Decode(Dxt1(8, 2, 1, 1), 64) is { Width: 8 },
            "pictures: a DXT1 memory decodes, at full size or at the smaller size asked for");

        check(SaveHealth.RecentCopy(DateTime.Now.AddDays(-40), new("", "", DateTime.Now.AddDays(-50))) && SaveHealth.RecentCopy(DateTime.Now.AddDays(-3), new("", "", DateTime.Now))
              && !SaveHealth.RecentCopy(DateTime.Now.AddDays(-40), new("", "", DateTime.Now)) && !SaveHealth.RecentCopy(null, new("", "", DateTime.Now)),
            "copies: one made since the save was played, or not long ago, protects it");

        // DXT5: alpha 255 and 0 by index, over white
        var d5 = Dxt1(4, 1, 0xFFFF)[..128].Concat(new byte[] { 255, 0, 0x01, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 }).ToArray();
        Encoding.ASCII.GetBytes("DXT5").CopyTo(d5, 84);
        var a = Dds.Decode(d5);
        // plain 32-bit pixels with their masks
        var plain = Dxt1(4, 1, 0)[..128].Concat(Enumerable.Range(0, 16).SelectMany(_ => new byte[] { 10, 20, 30, 40 })).ToArray();
        new byte[4].CopyTo(plain, 84);
        BinaryPrimitives.WriteInt32LittleEndian(plain.AsSpan(80), 0x41); BinaryPrimitives.WriteInt32LittleEndian(plain.AsSpan(88), 32);
        BinaryPrimitives.WriteUInt32LittleEndian(plain.AsSpan(92), 0x00FF0000); BinaryPrimitives.WriteUInt32LittleEndian(plain.AsSpan(96), 0x0000FF00);
        BinaryPrimitives.WriteUInt32LittleEndian(plain.AsSpan(100), 0x000000FF); BinaryPrimitives.WriteUInt32LittleEndian(plain.AsSpan(104), 0xFF000000);
        check(a is not null && a.Bgra[3] == 0 && a.Bgra[7] == 255 && a.Bgra[0] == 255 && Dds.Decode(plain) is { } p32 && p32.Bgra.AsSpan(0, 4).SequenceEqual(new byte[] { 10, 20, 30, 40 }),
            "pictures: DXT5 alpha and plain 32-bit pixels decode");

        var huge = Dxt1(8, 1, 0);
        BinaryPrimitives.WriteInt32LittleEndian(huge.AsSpan(12), 1 << 20);
        check(Dds.Decode(huge) is null && Dds.Decode(Dxt1(8, 1, 0)[..140]) is null && Dds.Decode(new byte[200]) is null && Dds.Decode([]) is null,
            "pictures: a damaged picture (a huge size, cut short, not DDS) gives none and no exception");

        // the PNG reads back: unpacked and unfiltered, it is the pixels given
        var pic = new SavePicture(2, 2, [1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 128, 10, 11, 12, 0]);
        var png = pic.Png();
        var idat = png.AsSpan(8 + 25 + 8, BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(8 + 25))).ToArray();
        var rows = new MemoryStream();
        using (var z = new ZLibStream(new MemoryStream(idat), CompressionMode.Decompress)) z.CopyTo(rows);
        var r = rows.ToArray();
        check(png.AsSpan(0, 8).SequenceEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }) && r.Length == 18 && r[0] == 1
              && r[1] == 3 && r[2] == 2 && r[3] == 1 && r[4] == 255 && (byte)(r[5] + r[1]) == 6 && (byte)(r[8] + r[4]) == 255 && r[9] == 1 && r[10] == 9 && r[13] == 128,
            "pictures: a PNG is written with the pixels' red, green, blue and alpha");
    }

    static void Info(Action<bool, string> check, string docs, Save s)
    {
        var i = SaveInfo.Of(s);
        check(i.World == "Town" && i.Household == "Davis" && i.Description == "Two adults." && i.Picture == 0x19 && i.Worlds.SequenceEqual(["Town", "Uni"])
              && i.Bytes == Directory.EnumerateFiles(s.Dir).Sum(f => new FileInfo(f).Length) && i.LastPlayed == s.LastPlayed && ReferenceEquals(i, SaveInfo.Of(s)),
            "info: world, household, description, the worlds (the one played first), size and last played, kept until a file changes");
        check(Saves.Photo(s) is { } photo && photo.AsSpan().SequenceEqual(Png("the Davis family")) && ReferenceEquals(photo, Saves.Photo(s)),
            "info: the household picture is the one Meta.data names, not the first in the world file");

        var broken = Path.Combine(docs, "Saves", "Broken.sims3");
        Directory.CreateDirectory(broken);
        File.WriteAllBytes(Path.Combine(broken, "Meta.data"), Enumerable.Repeat((byte)0xFF, 300).ToArray());
        File.WriteAllBytes(Path.Combine(broken, "1a5tf1l3.dat"), [.. Encoding.ASCII.GetBytes(@"..\..\Elsewhere_0x00000000"), 0]);
        // a count the file can't hold, and a resource that points past the end
        var count = Path.Combine(broken, "Elsewhere_0x00000000.nhd");
        Pkg(count, 0, Raw(Game, 0, 1, [1, 2, 3]));
        using (var f = File.OpenWrite(count)) { f.Position = 36; f.Write(BitConverter.GetBytes(0x7FFFFFFF)); }
        var past = Path.Combine(broken, "TravelDB.package");
        Pkg(past, 0, Raw(Img, Mem, 1, MemA));
        using (var f = File.OpenWrite(past)) { f.Position = new FileInfo(past).Length - 32 + 16; f.Write(BitConverter.GetBytes(0x7FFFFF00)); }
        var b = Saves.List(docs).First(x => x.Name == "Broken");
        var bi = SaveInfo.Of(b);
        var bc = SaveContents.Read(b.Dir);
        var plan = SaveClean.Plan(docs, b, CleanParts.Memories);
        check(bi.World == "Elsewhere" && bi.Household == "" && bi.Picture == 0 && Saves.Photo(b) is null && SaveInfo.Read(Path.Combine(docs, "Saves", "Nowhere.sims3")).Bytes == 0
              && bc.Files.Count(x => !x.Readable) == 2 && bc.Items(SaveKind.Memory).Count == 0 && bc.Bytes == bi.Bytes
              && !plan.CanRun && plan.Refusal is not null && Throws<SetupException>(() => SaveClean.Run(plan)),
            "info: a broken save (bad Meta.data, a huge count, a resource past the end) is told apart, never cleaned and never an exception");
        Directory.Delete(broken, true);
    }

    static void Contents(Action<bool, string> check, string docs, Save s)
    {
        var c = SaveContents.Read(s.Dir);
        var nhd = c.Files.First(f => f.Path.EndsWith("Town_0x00000000.nhd"));
        check(c.Of(SaveKind.Memory).Count == 4 && c.Of(SaveKind.Portrait).Count == 4 && c.Of(SaveKind.LotThumbnail).Count == 2 && c.Of(SaveKind.HouseholdPicture).Count == 3
              && c.Of(SaveKind.Generated).Count == 1 && c.Of(SaveKind.LotTexture).Count == 1 && c.Of(SaveKind.Game).Count == 7 && c.OwnPicture == 0x19
              && c.Of(SaveKind.Portrait).Bytes == Png("face one").Length + Png("face two").Length + Png("face two, small").Length + Png("face three").Length
              && nhd.Resources == 13 && nhd.DeadBytes == 13 * 7 && nhd.Kinds[SaveKind.Portrait].Count == 3 && c.DeadBytes == 13 * 7 + 5 * 3
              && c.Files.First(f => f.Path.EndsWith("ExportDB.package")).DeadBytes == 10 && c.Bytes == SaveInfo.Of(s).Bytes,
            "contents: counts and bytes per kind and per file, and the unused room (an unknown image group is the game's)");

        var groups = c.MemoryGroups();
        check(groups.Count == 2 && groups[0].Select(i => i.Instance).SequenceEqual(new ulong[] { 0xA1, 0xA2, 0xA3 }) && groups[1].Single().Instance == 0xC1 && ReferenceEquals(groups, c.MemoryGroups()),
            "contents: memories that are one picture are grouped by what they unpack to, however each is stored");

        var mem = c.Items(SaveKind.Memory)[0];
        check(SaveContents.Picture(mem) is { Width: 8 } && SaveContents.Picture(mem, 4) is { Width: 4 } && SaveContents.Png(mem) is { } png && png[1] == 'P'
              && SaveContents.Png(c.Items(SaveKind.LotThumbnail)[0])!.AsSpan().SequenceEqual(Png("lot one")) && SaveContents.Data(mem)!.AsSpan().SequenceEqual(MemA)
              && mem.Key == "00b2d882:0269d005:00000000000000a1",
            "contents: a picture on demand (a memory decoded, a lot as stored)");

        // stars and the Gallery
        SaveStars.Keep(s.Name, ["x"], ["x"]);
        SaveStars.Keep(s.Name, ["x", mem.Key], [mem.Key]);
        check(SaveStars.Of(s.Name).SetEquals([mem.Key]) && SaveStars.Of("Other").Count == 0, "stars: kept by save and key");
        File.WriteAllText(Path.Combine(Paths.Data, "save-stars.json"), "{ not json");
        var unread = SaveClean.Plan(docs, s, CleanParts.Memories);
        check(Throws<SetupException>(() => SaveStars.Of(s.Name)) && unread.Refusal is not null && !unread.CanRun, "stars: a star file that can't be read stops a clean, it never counts as no stars");
        File.Delete(Path.Combine(Paths.Data, "save-stars.json"));

        // a kept photo stays in a clean of them all
        var photo = c.Items(SaveKind.Generated).Single().Key;
        SaveStars.Keep(s.Name, [photo], [photo]);
        check(SaveClean.Plan(docs, s, CleanParts.None).Gains[CleanParts.Generated].Count == 0, "stars: a photo the player keeps is not in a clean");
        SaveStars.Keep(s.Name, [photo], []);
        var chosen = Path.Combine(docs, "Chosen");
        check(SaveContents.Export(s.Name, c.Items(SaveKind.Portrait), chosen).Count == 4 && Directory.GetFiles(chosen).Length == 4 && !Directory.GetFiles(chosen).Any(f => f.EndsWith(".part"))
              && SaveContents.Export(s.Name, c.Items(SaveKind.Portrait), chosen).Count == 4 && Directory.GetFiles(chosen).Length == 4, "pictures: any of them save to a chosen folder, once");
    }

    static void Clean(Action<bool, string> check, string docs, Save s)
    {
        var before = Snap(s.Dir);
        var all = Resources(s.Dir);
        var c = SaveContents.Read(s.Dir);
        long Stored(SaveKind k, Func<SaveItem, bool>? which = null) => c.Items(k).Where(i => SaveContents.Cleanable(i.File) && (which?.Invoke(i) ?? true)).Sum(i => (long)i.Size + 32);
        var none = SaveClean.Plan(docs, s, CleanParts.None, contents: c);
        check(none.CanRun && none.DeadBytes == 13 * 7 + 5 * 3 && none.After == none.Before - none.DeadBytes && none.Removed == 0
              && none.Gains[CleanParts.Portraits] == new CleanGain(4, Stored(SaveKind.Portrait)) && none.Gains[CleanParts.Generated].Count == 1
              && none.Gains[CleanParts.Thumbnails] == new CleanGain(4, Stored(SaveKind.LotThumbnail) + Stored(SaveKind.HouseholdPicture, i => i.Instance != 0x19))
              && none.Gains[CleanParts.RepeatedMemories] == new CleanGain(2, Stored(SaveKind.Memory, i => i.Instance is 0xA2 or 0xA3))
              && none.Gains[CleanParts.Memories] == new CleanGain(2, Stored(SaveKind.Memory, i => i.Instance is 0xA1 or 0xC1)),
            "clean: the plan says what each choice frees to the byte, the unused room apart, the save's own household picture never counted");

        // refused: the game running, a test run; nothing is written
        Games.IsRunning = () => true;
        var running = SaveClean.Plan(docs, s, CleanParts.Portraits);
        check(running.Refusal?.Key == Engine.CloseGameText && running.Gains[CleanParts.Portraits].Count == 4 && Throws<SetupException>(() => SaveClean.Run(running))
              && Throws<SetupException>(() => SaveCopies.Rename(docs, s, "Other")) && Throws<SetupException>(() => SaveCopies.Delete(s)) && Throws<SetupException>(() => SaveCopies.Store(docs, s)),
            "clean: refused while the game runs (the numbers are still told)");
        Games.IsRunning = () => false;
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
        var dry = SaveClean.Plan(docs, s, CleanParts.Portraits);
        check(!dry.CanRun && Throws<InvalidOperationException>(() => SaveClean.Run(dry)) && Throws<InvalidOperationException>(() => SaveCopies.Duplicate(docs, s)) && Same(before, Snap(s.Dir))
              && !Directory.Exists(Saves.BackupDir(docs)), "clean: a test run changes nothing");
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);

        // every step that can fail leaves the save byte for byte as it was, and no copy behind
        var plan = SaveClean.Plan(docs, s, CleanParts.Portraits | CleanParts.Thumbnails | CleanParts.RepeatedMemories | CleanParts.Generated);
        var asWas = true;
        foreach (var at in new[] { "copy", "write", "verify", "swap", "finish" })
        {
            var hits = 0;
            SaveClean.FailPoint = p => { if (p == at && ++hits == (at is "swap" or "verify" ? 2 : 1)) throw new IOException("stop at " + at); };
            asWas &= Throws<IOException>(() => SaveClean.Run(plan)) && Same(before, Snap(s.Dir)) && Snap(Saves.BackupDir(docs)).Count == 0;
        }
        SaveClean.FailPoint = _ => { };
        check(asWas, "clean: a failure while copying, writing, checking or swapping leaves the save exactly as it was");

        // a real damage in a new file is caught by the read-back, before anything moves
        SaveClean.FailPoint = p => { if (p == "write") { var made = Directory.GetFiles(s.Dir, "*.apex-new")[0]; var b = File.ReadAllBytes(made); b[100] ^= 1; File.WriteAllBytes(made, b); } };
        check(Throws<InvalidDataException>(() => SaveClean.Run(plan)) && Same(before, Snap(s.Dir)), "clean: a new file that does not read back like the original stops the clean");
        SaveClean.FailPoint = _ => { };

        var cancel = new CancellationTokenSource();
        cancel.Cancel();
        check(Throws<OperationCanceledException>(() => SaveClean.Run(plan, null, cancel.Token)) && Same(before, Snap(s.Dir)) && Directory.GetFiles(Saves.BackupDir(docs)).Length == 0, "clean: cancelled, the save is as it was");

        // the clean itself: a starred memory stays, a memory picked by key goes
        SaveStars.Keep(s.Name, [], ["00b2d882:0269d005:00000000000000a3"]);
        plan = SaveClean.Plan(docs, s, CleanParts.Portraits | CleanParts.Thumbnails | CleanParts.RepeatedMemories | CleanParts.Generated, ["00b2d882:0269d005:00000000000000c1", "00b2d882:0269d005:00000000000000a3"]);
        var steps = new List<CleanStep>();
        var r = SaveClean.Run(plan, new Sync(p => steps.Add(p.Step)));
        var after = SaveContents.Read(s.Dir);
        var kept = Resources(s.Dir);
        bool Stays(string key) => SaveContents.KindOf(Convert.ToUInt32(key.Split('|')[1][..8], 16), Convert.ToUInt32(key.Split('|')[1][9..17], 16)) is SaveKind.Game or SaveKind.LotTexture;
        string NoN(string key) => key[..key.LastIndexOf('|')];
        var keptKeys = kept.ToDictionary(x => NoN(x.Key), x => x.Value);
        check(after.Bytes == plan.After && r.After == plan.After && r.Before == plan.Before && after.DeadBytes == 0 && r.Removed == 4 + 4 + 2 + 1 + 1
              && after.Of(SaveKind.Portrait).Count == 0 && after.Of(SaveKind.LotThumbnail).Count == 0 && after.Of(SaveKind.Generated).Count == 0
              && after.Items(SaveKind.Memory).Single().Instance == 0xA3 && after.Items(SaveKind.HouseholdPicture).Single().Instance == 0x19
              && steps.Contains(CleanStep.Copy) && steps.Contains(CleanStep.Swap),
            "clean: the save is the planned size to the byte; a starred memory stays, a picked one goes, the save's own household picture stays");
        check(all.Where(x => Stays(x.Key)).All(x => keptKeys.TryGetValue(NoN(x.Key), out var v) && v == x.Value) && kept.Count == all.Count - r.Removed
              && Sha(Path.Combine(s.Dir, "Town_0x00000000ExportDB.package")) + File.GetLastWriteTimeUtc(Path.Combine(s.Dir, "Town_0x00000000ExportDB.package")).Ticks == before["Town_0x00000000ExportDB.package"]
              && before["Meta.data"] == Snap(s.Dir)["Meta.data"] && Saves.List(docs).First(x => x.Name == s.Name).LastPlayed == s.LastPlayed
              && !Directory.GetFiles(s.Dir).Any(f => f.Contains(".apex-")) && Saves.Photo(s)!.AsSpan().SequenceEqual(Png("the Davis family")),
            "clean: every game resource and lot texture is as it was, bytes and index fields; ExportDB and Meta.data are untouched; last played does not move");

        var point = SaveCopies.Timeline(docs, s).Single(p => p.Kind == CopyKind.Launcher);
        check(point.Path == r.Copy && point.Note == SaveCopies.BeforeClean && point.Bytes == plan.Before && SaveHealth.LastBackup(s, docs) is not null, "clean: the copy made first is a point on the timeline, marked as made before a clean");

        var undone = SaveClean.Undo(docs, r);
        check(Same(before.ToDictionary(x => x.Key, x => x.Value[..64]), Snap(undone.Dir).ToDictionary(x => x.Key, x => x.Value[..64]))
              && SaveCopies.Timeline(docs, s).Count(p => p.Note == SaveCopies.BeforeRestore && p.Bytes == plan.After) == 1, "clean: undo brings every file back identical, and the cleaned save stays as a copy");

        // a package with no unused room is rewritten byte for byte; one with room keeps every resource and index field
        var uni = Path.Combine(s.Dir, "Uni_0x0e41c954.nhd");
        var town = Path.Combine(s.Dir, "Town_0x00000000.nhd");
        SaveClean.Rewrite(uni, uni + ".copy");
        SaveClean.Rewrite(town, town + ".copy");
        SaveClean.Rewrite(Path.Combine(s.Dir, "Meta.data"), town + ".meta");
        static string One(string f) { var d = Path.Combine(Path.GetDirectoryName(f)!, "one"); Directory.CreateDirectory(d); File.Copy(f, Path.Combine(d, "x.package"), true); var h = string.Join(",", Resources(d).OrderBy(x => x.Key).Select(x => x.Key + x.Value)); Directory.Delete(d, true); return h; }
        check(Sha(uni) == Sha(uni + ".copy") && One(town) == One(town + ".copy") && new FileInfo(town + ".copy").Length == new FileInfo(town).Length - 13 * 7
              && Sha(Path.Combine(s.Dir, "Meta.data")) == Sha(town + ".meta") && Throws<IOException>(() => SaveClean.Rewrite(uni, uni + ".copy")),
            "clean: a rewrite with nothing removed is byte for byte the same when the file has no unused room, and never writes over a file");
        foreach (var f in Directory.GetFiles(s.Dir, "*.copy").Append(town + ".meta")) File.Delete(f);
        foreach (var f in Directory.GetFiles(Saves.BackupDir(docs))) File.Delete(f);
        SaveStars.Keep(s.Name, ["00b2d882:0269d005:00000000000000a3"], []);

        // a file that changed since the plan
        var stale = SaveClean.Plan(docs, s, CleanParts.Portraits);
        File.SetLastWriteTime(town, File.GetLastWriteTime(town).AddSeconds(2));
        check(Throws<SetupException>(() => SaveClean.Run(stale)) && Directory.GetFiles(Saves.BackupDir(docs)).Length == 0, "clean: a save that changed since the plan is not cleaned");
        File.SetLastWriteTime(town, new DateTime(2024, 3, 9, 10, 0, 0));
    }

    sealed class Sync(Action<CleanProgress> a) : IProgress<CleanProgress> { public void Report(CleanProgress v) => a(v); }

    static void Copies(Action<bool, string> check, string docs, Save s)
    {
        var first = Snap(s.Dir);
        var zip = Saves.Backup(s, docs);
        var second = Saves.Backup(s, docs); // (the same second: never over the first)
        // the game's previous version, older; then the save changes
        var game = s.Dir + ".backup";
        Directory.CreateDirectory(game);
        foreach (var f in Directory.GetFiles(s.Dir)) File.Copy(f, Path.Combine(game, Path.GetFileName(f)));
        File.WriteAllText(Path.Combine(game, "1a5tf1l3.dat"), "Uni_0x0e41c954\0\n");
        foreach (var f in Directory.GetFiles(game)) File.SetLastWriteTime(f, new DateTime(2024, 3, 1, 8, 0, 0));
        var older = Snap(game);
        File.AppendAllText(Path.Combine(s.Dir, "TravelDB.package"), "played on");
        s = Saves.List(docs).First(x => x.Name == s.Name);
        var played = Snap(s.Dir);
        var line = SaveCopies.Timeline(docs, s);
        check(zip != second && line.Select(p => p.Kind).SequenceEqual([CopyKind.Game, CopyKind.Launcher, CopyKind.Launcher, CopyKind.Now]) && line[1].Path == zip && line[1].Note is null
              && line[1].Bytes == SaveInfo.Of(s).Bytes - 9 && line[0].When == new DateTime(2024, 3, 1, 8, 0, 0) && line[3].Bytes == SaveInfo.Of(s).Bytes && SaveInfo.Read(game).Worlds[0] == "Uni",
            "copies: the timeline lists the game's previous version, the launcher's copies and the save now, with each one's size and day");

        // a failure at each step leaves the save and the copies as they were
        var zips = Directory.GetFiles(Saves.BackupDir(docs)).Length;
        var asWas = true;
        foreach (var at in new[] { "extract", "swap" })
        {
            // (what is on its way waits inside Saves, on the saves' own drive, and is not listed as a save)
            SaveCopies.FailPoint = p =>
            {
                if (p != at) return;
                asWas &= Directory.Exists(Path.Combine(docs, "Saves", ".apex-work")) && Directory.GetDirectories(docs).All(d => !d.EndsWith(".apex-work"))
                         && Saves.List(docs).Concat(SaveCopies.Lost(docs)).All(x => x.Name.StartsWith("Town"));
                throw new IOException("stop at " + at);
            };
            asWas &= Throws<IOException>(() => SaveCopies.Restore(docs, s, line[1])) && Same(played, Snap(s.Dir)) && Directory.GetFiles(Saves.BackupDir(docs)).Length == zips
                     && !Directory.Exists(Path.Combine(docs, "Saves", ".apex-work"));
        }
        SaveCopies.FailPoint = _ => { };
        check(asWas, "copies: a restore that fails leaves the save as it was, and nothing behind");

        // names that point outside the save, in a zip made elsewhere
        var evil = Path.Combine(Saves.BackupDir(docs), "Town 2020-01-01 00.00.00.zip");
        using (var z = ZipFile.Open(evil, ZipArchiveMode.Create))
        {
            z.CreateEntry("Town.sims3/Town_0x00000000.nhd").Open().Dispose();
            using var o = z.CreateEntry("Town.sims3/../../../evil.txt").Open();
            o.Write("out"u8);
        }
        var bad = SaveCopies.Timeline(docs, s).First(p => p.Path == evil);
        File.WriteAllText(Path.Combine(Saves.BackupDir(docs), "Town 2020-01-02 00.00.00.zip"), "not a zip");
        check(Throws<SetupException>(() => SaveCopies.Restore(docs, s, bad)) && Same(played, Snap(s.Dir)) && !Directory.EnumerateFiles(Path.GetDirectoryName(docs)!, "evil.txt", SearchOption.AllDirectories).Any()
              && SaveCopies.Timeline(docs, s).Count == 5 && Directory.GetFiles(Saves.BackupDir(docs)).Length == zips + 2,
            "copies: a copy whose file names point outside the save is refused whole; a file that is not a zip is not offered");
        File.Delete(evil);
        File.Delete(Path.Combine(Saves.BackupDir(docs), "Town 2020-01-02 00.00.00.zip"));

        var back = SaveCopies.Restore(docs, s, line[1]);
        var now = SaveCopies.Timeline(docs, back);
        var keptZip = now.Single(p => p.Note == SaveCopies.BeforeRestore);
        using (var z = ZipFile.OpenRead(keptZip.Path))
            check(Same(first.ToDictionary(x => x.Key, x => x.Value[..64]), Snap(back.Dir).ToDictionary(x => x.Key, x => x.Value[..64])) && Math.Abs((back.LastPlayed - new DateTime(2024, 3, 9, 10, 0, 0)).TotalSeconds) <= 2
                  && new StreamReader(z.GetEntry("Town.sims3/TravelDB.package")!.Open()).ReadToEnd().EndsWith("played on") && keptZip.Bytes == line[3].Bytes
                  && !Directory.Exists(Path.Combine(docs, "Saves", ".apex-work")) && Directory.GetFiles(Saves.BackupDir(docs)).Length == zips + 1,
                "copies: going back to a copy gives its files again, and the save of now is kept as one more copy first");

        var prev = SaveCopies.Restore(docs, back, now.First(p => p.Kind == CopyKind.Game));
        check(Same(older, Snap(prev.Dir)) && Same(older, Snap(game)) && SaveInfo.Of(prev).Worlds[0] == "Uni", "copies: going back to the game's previous version copies it; the game's own copy stays");
        SaveCopies.Restore(docs, prev, line[1]);
    }

    static void Whole(Action<bool, string> check, string docs, string recycled)
    {
        var saves = Path.Combine(docs, "Saves");
        var s = Saves.List(docs).First(x => x.Name == "Town");
        var files = Snap(s.Dir);
        var game = Snap(s.Dir + ".backup");

        // a save the game only has the previous version of
        Directory.Move(Path.Combine(saves, "Town.sims3.backup"), Path.Combine(saves, "Gone.sims3.backup"));
        var lost = SaveCopies.Lost(docs);
        check(lost.Count == 1 && lost[0].Name == "Gone" && SaveInfo.Of(lost[0]).Household == "Davis" && SaveInfo.Photo(lost[0].Dir) is not null && Saves.List(docs).All(x => x.Name != "Gone"),
            "lost: a save that is only a .sims3.backup is found, with its household and picture");
        SaveCopies.FailPoint = p => { if (p == "copy") throw new IOException("stop"); };
        var failed = Throws<IOException>(() => SaveCopies.Rescue(docs, lost[0])) && !Directory.Exists(Path.Combine(saves, "Gone.sims3")) && !Directory.Exists(Path.Combine(saves, ".apex-work"));
        SaveCopies.FailPoint = _ => { };
        var rescued = SaveCopies.Rescue(docs, lost[0]);
        check(failed && rescued.Name == "Gone" && Same(game, Snap(rescued.Dir)) && Same(game, Snap(lost[0].Dir)) && SaveCopies.Lost(docs).Count == 0 && Throws<SetupException>(() => SaveCopies.Rescue(docs, lost[0])),
            "lost: it comes back under its own name as a checked copy; the game's copy stays");

        // a save only the launcher's zip still has
        var only = Saves.Backup(rescued, docs);
        var zips = Directory.GetFiles(Saves.BackupDir(docs), "*.zip").Length;
        var there = SaveCopies.Gone(docs).Count;
        Directory.Move(rescued.Dir, rescued.Dir + ".away");
        var gone = SaveCopies.Gone(docs).Where(x => x.Save.Name == "Gone").ToList();
        SaveCopies.FailPoint = p => { if (p == "swap") throw new IOException("stop"); };
        var stopped = Throws<IOException>(() => SaveCopies.Restore(docs, gone[0].Save, gone[0].Point)) && !Directory.Exists(rescued.Dir) && !Directory.Exists(Path.Combine(saves, ".apex-work"));
        SaveCopies.FailPoint = _ => { };
        var revived = SaveCopies.Restore(docs, gone[0].Save, gone[0].Point);
        check(there == 0 && gone.Count == 1 && gone[0].Point.Path == only && stopped && Same(Snap(rescued.Dir + ".away").ToDictionary(x => x.Key, x => x.Value[..64]), Snap(revived.Dir).ToDictionary(x => x.Key, x => x.Value[..64]))
              && SaveCopies.Gone(docs).Count == 0 && Directory.GetFiles(Saves.BackupDir(docs), "*.zip").Length == zips, "gone: a save only a zip still has comes back under its name, and a failure leaves nothing behind");
        Directory.Delete(revived.Dir, true);
        Directory.Move(rescued.Dir + ".away", rescued.Dir);
        File.Delete(only);

        // duplicate
        SaveStars.Keep("Town", [], ["k1"]);
        var twin = SaveCopies.Duplicate(docs, s);
        var named = SaveCopies.Duplicate(docs, s, " Twin ");
        check(twin.Name == "Town (2)" && named.Name == "Twin" && Same(files, Snap(twin.Dir)) && Same(files, Snap(s.Dir)) && !Directory.Exists(twin.Dir + ".backup")
              && SaveStars.Of("Town (2)").Contains("k1") && SaveStars.Of("Town").Contains("k1") && Throws<SetupException>(() => SaveCopies.Duplicate(docs, s, "Gone")),
            "duplicate: a second save with the same files and stars under a free name; a taken name is refused");

        // rename: the folder, the game's copy, the launcher's zips and what it keeps by name
        SaveHealth.Check(docs);
        var g = Gallery.Load();
        Gallery.Info(g, @"C:\shots\a.png").Household = "gone";
        g.Custom.Add(new CustomAlbum { Name = "Trip", Household = "Gone" });
        Gallery.Look(g, "Gone").Font = "Arial";
        g.LastAlbum = "Gone";
        Gallery.Save(g);
        SaveStars.Keep("Gone", [], ["k2"]);
        var zip = Saves.Backup(rescued, docs);
        var refused = new[] { "", "  ", "a/b", @"..\x", "con", "Twin", "twin", new string('n', 200), "x:y" }.All(n => Throws<SetupException>(() => SaveCopies.Rename(docs, rescued, n)));
        SaveCopies.FailPoint = p => { if (p == "move") throw new IOException("stop"); };
        var half = Throws<IOException>(() => SaveCopies.Rename(docs, rescued, "Vila")) && Directory.Exists(rescued.Dir) && Directory.Exists(rescued.Dir + ".backup") && !Directory.Exists(Path.Combine(saves, "Vila.sims3"));
        SaveCopies.FailPoint = _ => { };
        var vila = SaveCopies.Rename(docs, rescued, " Vila ");
        g = Gallery.Load();
        check(refused && half && vila.Name == "Vila" && Same(game, Snap(vila.Dir)) && Same(game, Snap(vila.Dir + ".backup")) && !Directory.Exists(rescued.Dir) && !Directory.Exists(rescued.Dir + ".backup")
              && File.Exists(Path.Combine(Saves.BackupDir(docs), "Vila" + Path.GetFileName(zip)[4..])) && !File.Exists(zip) && SaveCopies.Timeline(docs, vila).Count(p => p.Kind == CopyKind.Launcher) == 1
              && SaveHealth.History("Vila").Count == 1 && SaveHealth.History("Gone").Count == 0 && SaveStars.Of("Vila").Contains("k2") && SaveStars.Of("Gone").Count == 0
              && g.Pictures[@"C:\shots\a.png"].Household == "Vila" && g.Custom[0].Household == "Vila" && g.Albums["Vila"].Font == "Arial" && g.LastAlbum == "Vila",
            "rename: invalid and taken names are refused; the folder, the game's copy, the launcher's copies, the size history, the stars and the Gallery's household follow");
        var cased = SaveCopies.Rename(docs, vila, "VILA");
        check(Directory.GetDirectories(saves).Select(Path.GetFileName).Contains("VILA.sims3") && Directory.GetDirectories(saves).Select(Path.GetFileName).Contains("VILA.sims3.backup") && cased.Name == "VILA"
              && SaveCopies.CleanName(docs, " New name ") == "New name" && Throws<SetupException>(() => SaveCopies.CleanName(docs, "vila")), "rename: only the letters' case can change too");

        // out of the game's sight and back
        var stored = SaveCopies.Store(docs, cased);
        var inSaves = Saves.List(docs).Any(x => x.Name == "VILA") || SaveCopies.Lost(docs).Count > 0;
        var listed = SaveCopies.Stored(docs);
        var withCopy = Directory.Exists(stored.Dir + ".backup");
        Directory.CreateDirectory(Path.Combine(saves, "VILA.sims3.backup")); // (the name was taken meanwhile)
        File.WriteAllText(Path.Combine(saves, "VILA.sims3.backup", "x.nhd"), "another");
        var home = SaveCopies.Unstore(docs, listed[0]);
        check(!inSaves && listed.Count == 1 && stored.Dir == Path.Combine(SaveCopies.StoredDir(docs), "VILA.sims3") && withCopy
              && home.Name == "VILA (2)" && Same(game, Snap(home.Dir)) && Same(game, Snap(home.Dir + ".backup")) && !Directory.Exists(SaveCopies.StoredDir(docs)) && SaveStars.Of("VILA (2)").Contains("k2")
              && File.ReadAllText(Path.Combine(saves, "VILA.sims3.backup", "x.nhd")) == "another",
            "store: a save and the game's copy of it leave the game's sight and come back, under a free name when its own was taken");

        // delete: to the Recycle Bin, whole
        var bin = Directory.GetDirectories(recycled).Length;
        SaveCopies.Delete(home);
        check(!Directory.Exists(home.Dir) && Directory.GetDirectories(recycled).Length == bin + 1 && Directory.Exists(home.Dir + ".backup") && SaveCopies.Lost(docs).Any(x => x.Name == "VILA (2)"),
            "delete: the save goes to the Recycle Bin whole; the game's copy stays and can bring it back");

        // what a change cut short leaves is put right at the next start
        var nhd = Path.Combine(s.Dir, "Town_0x00000000.nhd");
        var travel = Path.Combine(s.Dir, "TravelDB.package");
        File.Move(nhd, nhd + ".apex-old");                        // moved aside, nothing in its place
        File.Copy(travel, travel + ".apex-old");                  // moved aside, the new file in
        File.WriteAllText(Path.Combine(s.Dir, "Uni_0x0e41c954.nhd.apex-new"), "half written");
        var work = Path.Combine(saves, ".apex-work");
        Directory.CreateDirectory(Path.Combine(work, "Twin.sims3.new-1"));
        Directory.Move(named.Dir, Path.Combine(work, "Twin.sims3.old-2"));        // a save moved aside, nothing in its place
        Directory.CreateDirectory(Path.Combine(work, "Town.sims3.old-3"));        // one whose place was taken
        File.WriteAllText(Path.Combine(work, "Town.sims3.old-3", "x"), "old");
        // (a save left aside whose name looks free, because only the game's copy of it shows)
        var aside = SaveCopies.Lost(docs).First(x => x.Name == "VILA (2)");
        Directory.CreateDirectory(Path.Combine(work, "VILA (2).sims3.old-4"));
        File.WriteAllText(Path.Combine(work, "VILA (2).sims3.old-4", "newer.nhd"), "the save as last played");
        bin = Directory.GetFileSystemEntries(recycled).Length;
        Games.IsRunning = () => true;
        var waited = !SaveCopies.Recover(docs) && File.Exists(nhd + ".apex-old") && Directory.Exists(work);
        Games.IsRunning = () => false;
        var first = Throws<SetupException>(() => SaveCopies.Rescue(docs, aside)) && File.ReadAllText(Path.Combine(saves, "VILA (2).sims3", "newer.nhd")) == "the save as last played";
        check(waited && SaveCopies.Recover(docs) && Same(files, Snap(s.Dir)) && Same(files, Snap(named.Dir)) && !Directory.Exists(work) && Directory.GetFileSystemEntries(recycled).Length == bin + 2,
            "recover: a file or a save left aside goes back when its place is empty and to the Recycle Bin when it is taken; half-made copies go (not while the game runs, and it says it did not run)");
        check(first, "recover: bringing a lost save back puts a save left aside home first, so the game's older copy never takes its place");
    }

    // two saves with one name, one of them out of the game: what the launcher keeps by name stays with the one that keeps
    // the name. And the launcher's own copies do not pile up.
    static void Namesakes(Action<bool, string> check, string docs, string recycled)
    {
        var kept = SaveCopies.StoredDir(docs);
        int Zips(string name) => Directory.Exists(Saves.BackupDir(docs)) ? Directory.GetFiles(Saves.BackupDir(docs), name + " 20*.zip").Length : 0;

        // put back in the game while a newer save has its name
        var old = Make(docs, "Smith");
        SaveStars.Keep("Smith", [], ["old-star"]);
        Saves.Backup(old, docs);
        SaveCopies.Store(docs, old);
        var live = Make(docs, "Smith");
        File.AppendAllText(Path.Combine(live.Dir, "TravelDB.package"), "another family");
        live = Saves.List(docs).First(x => x.Name == "Smith");
        SaveStars.Keep("Smith", [], ["new-star"]);
        Saves.Backup(live, docs);
        SaveHealth.Check(docs);
        var back = SaveCopies.Unstore(docs, SaveCopies.Stored(docs)[0]);
        check(back.Name == "Smith (2)" && Zips("Smith") == 2 && Zips("Smith (2)") == 0 && SaveCopies.Timeline(docs, live).Count(p => p.Kind == CopyKind.Launcher) == 2
              && SaveStars.Of("Smith").SetEquals(["old-star", "new-star"]) && SaveStars.Of("Smith (2)").Contains("old-star")
              && SaveHealth.History("Smith").Count == 1 && SaveHealth.History("Smith (2)").Count == 0 && !Directory.Exists(kept),
            "namesakes: a save put back under a free name leaves the copies, the stars and the history of the save that has its name; its own stars come with it");

        // renamed while a save out of the game has its name; kept out while one there has its name
        var r1 = Make(docs, "R");
        Directory.CreateDirectory(r1.Dir + ".backup");
        File.WriteAllText(Path.Combine(r1.Dir + ".backup", "x.nhd"), "the game's copy");
        SaveStars.Keep("R", [], ["r-star"]);
        Saves.Backup(r1, docs);
        r1 = SaveCopies.Store(docs, r1);
        var r2 = SaveCopies.Rename(docs, Make(docs, "R"), "R new");
        var renamed = r2.Name == "R new" && Zips("R") == 1 && Zips("R new") == 0 && SaveStars.Of("R").Contains("r-star") && SaveStars.Of("R new").Contains("r-star");
        var r3 = Make(docs, "R");
        SaveStars.Keep("R", [], ["third"]);
        r3 = SaveCopies.Store(docs, r3);
        check(renamed && r3.Name == "R (2)" && Zips("R") == 1 && Zips("R (2)") == 0 && SaveStars.Of("R").Contains("third") && SaveStars.Of("R (2)").Contains("third"),
            "namesakes: the same when a save is renamed, or kept out of the game, while another one has its name");

        // a save out of the game is deleted with the game's copy of it; the last one takes the folder with it
        SaveCopies.Delete(r1);
        var gone = !Directory.Exists(r1.Dir) && !Directory.Exists(r1.Dir + ".backup") && SaveCopies.Stored(docs).Count == 1 && Directory.Exists(kept);
        var again = SaveCopies.Store(docs, Make(docs, "R"));
        SaveCopies.Delete(again);
        SaveCopies.Delete(r3);
        check(gone && again.Name == "R" && !Directory.Exists(kept), "delete: a save kept out of the game goes with the game's copy of it, so its name is free again there; the empty folder goes too");

        // the launcher's own copies: the newest three of a save stay, the ones the player made all stay
        var asked = SaveCopies.Timeline(docs, live).First(p => p.Kind == CopyKind.Launcher);
        var bin = Directory.GetFiles(recycled).Length;
        for (var i = 0; i < 5; i++) live = SaveCopies.Restore(docs, live, asked);
        var line = SaveCopies.Timeline(docs, live);
        check(line.Count(p => p.Note == SaveCopies.BeforeRestore) == 3 && line.Count(p => p.Kind == CopyKind.Launcher && p.Note is null) == 2 && Zips("Smith") == 5 && Directory.GetFiles(recycled).Length == bin + 2,
            "copies: of the ones the launcher makes by itself the newest three of a save stay and older ones go to the Recycle Bin; a copy the player made is never removed");
    }

    // a failure at every step of every change to a save leaves it, its copies and the folders around it as they were
    static bool Fuzz(string docs, Save s, CleanParts parts)
    {
        var copy = s.Dir + ".backup";
        Directory.CreateDirectory(copy);
        File.WriteAllText(Path.Combine(copy, "x.nhd"), "the game's copy");
        var zip = Saves.Backup(s, docs);
        var point = SaveCopies.Timeline(docs, s).First(p => p.Path == zip);
        var plan = SaveClean.Plan(docs, s, parts);
        var ok = true;
        void Fails(string what, string[] steps, Action act)
        {
            var before = Snap(docs);
            var dirs = Directory.GetDirectories(docs, "*", SearchOption.AllDirectories).Length;
            foreach (var at in steps)
            {
                SaveClean.FailPoint = SaveCopies.FailPoint = p => { if (p == at) throw new IOException("stop at " + at); };
                var held = Throws<IOException>(act);
                SaveClean.FailPoint = SaveCopies.FailPoint = _ => { };
                held = held && Same(before, Snap(docs)) && Directory.GetDirectories(docs, "*", SearchOption.AllDirectories).Length == dirs && SaveCopies.Recover(docs) && Same(before, Snap(docs));
                if (!held) Console.WriteLine($"     {s.Name}: {what} stopped at {at} left something changed");
                ok &= held;
            }
        }
        if (plan.CanRun) Fails("clean", ["copy", "write", "verify", "swap", "finish"], () => SaveClean.Run(plan));
        Fails("restore", ["extract", "swap"], () => SaveCopies.Restore(docs, s, point));
        Fails("rename", ["move"], () => SaveCopies.Rename(docs, s, "Fuzz name"));
        Fails("duplicate", ["copy"], () => SaveCopies.Duplicate(docs, s));
        Fails("rescue", ["copy"], () => SaveCopies.Rescue(docs, new Save(copy, "Fuzz ghost", s.LastPlayed)));
        Fails("store", ["move"], () => SaveCopies.Store(docs, s));
        // and each one done and undone gives the same files again
        var whole = Snap(docs);
        var stored = SaveCopies.Store(docs, s);
        Fails("put back", ["move"], () => SaveCopies.Unstore(docs, stored));
        var home = SaveCopies.Unstore(docs, stored);
        ok &= home.Dir == s.Dir && Same(whole, Snap(docs));
        ok &= SaveCopies.Rename(docs, SaveCopies.Rename(docs, home, "Fuzz name"), s.Name).Dir == s.Dir && Same(whole, Snap(docs));
        var files = Snap(s.Dir);
        ok &= Same(files.ToDictionary(x => x.Key, x => x.Value[..64]), Snap(SaveCopies.Restore(docs, home, point).Dir).ToDictionary(x => x.Key, x => x.Value[..64]));
        return ok;
    }

    // every save of a real Saves folder (a COPY: it is only read), each copied to the temporary folder first
    static void Real(Action<bool, string> check, string from, string root, string recycled)
    {
        var docs = Path.Combine(root, "Docs");
        var saves = Path.Combine(docs, "Saves");
        string Mb(long b) => (b / 1048576.0).ToString("0.00");
        var parts = new[] { CleanParts.Portraits, CleanParts.Thumbnails, CleanParts.RepeatedMemories, CleanParts.Memories, CleanParts.Generated };
        var everything = parts.Aggregate((a, b) => a | b);
        Console.WriteLine("REAL save | size MB | unused | portraits | thumbnails | repeated memories | memories | generated | all MB (n: count)");
        foreach (var src in Directory.GetDirectories(from).OrderBy(d => d))
        {
            var name = Path.GetFileName(src).Replace(".sims3.backup", " (game copy)").Replace(".sims3", "");
            var dir = Path.Combine(saves, name + ".sims3");
            Directory.CreateDirectory(dir);
            foreach (var f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dir, Path.GetFileName(f)));
            var s = Saves.List(docs).First(x => x.Dir == dir);
            var before = Snap(dir);
            var info = SaveInfo.Of(s);
            var photo = Saves.Photo(s);

            // 1. every package rewritten with nothing removed: the same resources, and which bytes differ
            long same = 0, differ = 0, diffBytes = 0, headBytes = 0;
            var ok = true;
            foreach (var f in Directory.GetFiles(dir).Where(f => !f.EndsWith(".dat")))
            {
                var to = Path.Combine(root, "rewrite.tmp");
                File.Delete(to);
                SaveClean.Rewrite(f, to);
                byte[] a = File.ReadAllBytes(f), b = File.ReadAllBytes(to);
                if (a.AsSpan().SequenceEqual(b)) same++;
                else
                {
                    differ++;
                    diffBytes += a.Length - b.Length;
                    for (var i = 0; i < 96; i++) if (a[i] != b[i]) headBytes++;
                    for (var i = 0; i < 96; i++) ok &= a[i] == b[i] || i is >= 36 and < 40 or >= 44 and < 48 or >= 64 and < 68;
                }
                var ea = ModConflicts.Package.Open(f).Entries;
                var eb = ModConflicts.Package.Open(to).Entries;
                ok &= ea.Count == eb.Count && b.Length == 96 + ea.Select(e => (e.Offset, e.FileSize)).Distinct().Sum(e => (long)e.FileSize) + BinaryPrimitives.ReadInt32LittleEndian(a.AsSpan(44));
                for (var i = 0; i < ea.Count && ok; i++)
                    ok &= ea[i] with { Offset = 0 } == eb[i] with { Offset = 0 } && a.AsSpan((int)ea[i].Offset, ea[i].FileSize).SequenceEqual(b.AsSpan((int)eb[i].Offset, eb[i].FileSize));
                File.Delete(to);
            }
            check(ok, $"REAL {name}: a rewrite with nothing removed keeps every resource and index field ({same} files byte-identical, {differ} differ: {diffBytes} unused bytes gone, {headBytes} header bytes for the index's new place)");

            // 2. what each choice frees, and the clean of everything against its plan
            var clock = System.Diagnostics.Stopwatch.StartNew();
            var c = SaveContents.Read(dir);
            var read = clock.ElapsedMilliseconds;
            c.MemoryGroups();
            var grouped = clock.ElapsedMilliseconds - read;
            // APEX_SAVES_REAL_PICS=<folder>: a few pictures of each save as PNG files, to look at
            if (Environment.GetEnvironmentVariable("APEX_SAVES_REAL_PICS") is { Length: > 0 } pics)
            {
                SaveContents.Export(name, new[] { SaveKind.Memory, SaveKind.Generated, SaveKind.Portrait, SaveKind.LotThumbnail, SaveKind.HouseholdPicture }.SelectMany(k => c.Items(k).Take(2)), pics);
                if (c.Items(SaveKind.Memory).FirstOrDefault() is { Size: > 0 } m && SaveContents.Png(m, 128) is { } small) File.WriteAllBytes(Path.Combine(pics, name + " small.png"), small);
            }
            var none = SaveClean.Plan(docs, s, CleanParts.None, contents: c);
            var each = parts.Select(p => SaveClean.Plan(docs, s, p, contents: c)).ToList();
            var all = SaveClean.Plan(docs, s, everything, contents: c);
            Console.WriteLine($"REAL {name} | {Mb(none.Before)} | {Mb(none.DeadBytes)} | " + string.Join(" | ", parts.Select((p, i) => $"{Mb(each[i].After)} (n {none.Gains[p].Count})")) + $" | {Mb(all.After)}"
                + $" | {info.Household}, {string.Join(" + ", info.Worlds)}, photo {(photo is null ? "none" : photo.Length + " B")}, memories {c.Of(SaveKind.Memory).Count} in {c.MemoryGroups().Count} pictures, ExportDB unused {Mb(c.Files.Where(f => f.Path.EndsWith("ExportDB.package")).Sum(f => f.DeadBytes))}, read in {read} ms, memories compared in {grouped} ms");
            ok = each.Select((p, i) => p.After == none.After - none.Gains[parts[i]].Bytes).All(x => x) && all.After == none.After - parts.Sum(p => none.Gains[p].Bytes);
            var game = Resources(dir, k => k is SaveKind.Game or SaveKind.LotTexture);
            if (all.CanRun)
            {
                var r = SaveClean.Run(all);
                var after = SaveContents.Read(dir);
                var left = Resources(dir, k => k is SaveKind.Game or SaveKind.LotTexture);
                ok &= after.Bytes == all.After && after.DeadBytes == 0 && Same(game, left) && after.Of(SaveKind.Portrait).Count + after.Of(SaveKind.Memory).Count + after.Of(SaveKind.LotThumbnail).Count + after.Of(SaveKind.Generated).Count == 0
                      && after.Of(SaveKind.HouseholdPicture).Count == (c.OwnPicture == 0 ? c.Of(SaveKind.HouseholdPicture).Count : c.Items(SaveKind.HouseholdPicture).Count(i => i.Instance == c.OwnPicture))
                      && (photo is null ? Saves.Photo(s) is null : Saves.Photo(s)!.AsSpan().SequenceEqual(photo)) && Saves.List(docs).First(x => x.Dir == dir).LastPlayed == s.LastPlayed;
                var undone = SaveClean.Undo(docs, r);
                ok &= Same(before.ToDictionary(x => x.Key, x => x.Value[..64]), Snap(undone.Dir).ToDictionary(x => x.Key, x => x.Value[..64]));
            }
            check(ok, $"REAL {name}: the plan is exact for every choice; cleaned of everything, each game resource and lot texture and its own household picture are as they were; undo brings every file back identical");
            check(Fuzz(docs, Saves.List(docs).First(x => x.Dir == dir), everything), $"REAL {name}: clean, go back, rename, duplicate, rescue, keep out and put back, each stopped at every step, leave everything as it was; done and undone they give the same files");
            Directory.Delete(docs, true);
            foreach (var d in Directory.GetDirectories(recycled)) Directory.Delete(d, true);
        }
    }
}
